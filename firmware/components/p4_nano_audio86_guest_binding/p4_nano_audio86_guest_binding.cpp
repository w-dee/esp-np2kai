#include "p4_nano_audio86_guest_binding/p4_nano_audio86_guest_binding.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <compiler.h>

extern "C" {
#include <cbus/board86.h>
#include <i286c/cpucore.h>
#include <i286c/i286c.h>
#include <mem/memtram.h>
#include <nevent.h>
#include <pccore.h>
#include "pic.h"

/* iocore.h transitively includes pic.h without an include guard.  The two
 * declarations below are the narrow board-bootstrap surface needed here. */
void iocore_create(void);
BRESULT iocore_build(void);
}

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2audio86_fixture.h"
#include "np2audio86_guest_adapter.h"
#include "np2audio86_guest_async.h"
#include "np2audio86_guest_evidence.h"
#include "np2audio86_guest_program.h"
#include "np2audio86_runtime_transport.h"
#include "np2opngen_pcm_canonical.h"
#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"
#include "np2runtime/np2runtime.hpp"
#include "p4_nano_audio86_notifications/task_notification.hpp"

namespace p4_nano_audio86_guest_binding {
namespace {

constexpr BaseType_t kWorkerCore = 0;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 6U;
constexpr uint32_t kWorkerStackBytes = 8192U;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
constexpr BaseType_t kPcmConsumerCore = 0;
constexpr UBaseType_t kPcmConsumerPriority = tskIDLE_PRIORITY + 7U;
constexpr uint32_t kPcmConsumerStackBytes = 4096U;
constexpr uint32_t kPcmPrefillSlots = 4U;
#endif
constexpr TickType_t kTimeout = pdMS_TO_TICKS(5000U);
constexpr uint32_t kErrorTransport = 1U;
constexpr uint32_t kErrorWorker = 2U;
constexpr uint32_t kErrorGuest = 3U;
constexpr uint32_t kErrorFinalRender = 4U;
constexpr uint32_t kErrorEventApply = 5U;
/* The fixture's DATA_RUN value overlaps the guest adapter's PCM-control
 * trace value.  Keep the transport semantic namespaces disjoint. */
constexpr uint32_t kEventOpnaRegister = 0x100U;
constexpr uint32_t kEventOpnaCsm = 0x101U;
constexpr uint32_t kEventPcmControl = 0x102U;
#if defined(P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE)
constexpr size_t kRenderFrames = 13U;
constexpr uint32_t kExpectedPcmSlots = 1U;
constexpr uint32_t kExpectedPartialSlots = 1U;
#else
constexpr size_t kRenderFrames = 2400U;
constexpr uint32_t kExpectedPcmSlots = 10U;
constexpr uint32_t kExpectedPartialSlots = 0U;
#endif
constexpr size_t kApplyRecordBytes = 40U;
#ifndef P4_NANO_AUDIO86_PRESSURE_SCENARIO
#define P4_NANO_AUDIO86_PRESSURE_SCENARIO 0
#endif
#ifndef P4_NANO_AUDIO86_FAILURE_KIND
#define P4_NANO_AUDIO86_FAILURE_KIND 0
#endif
#ifndef P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO
#define P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO 0
#endif
enum : uint32_t { kPressureNone = 0U, kPressureEvent = 1U,
                  kPressureByte = 2U, kPressureHorizon = 3U,
                  kPressureResetAck = 4U, kPressureByteExtend = 5U };
constexpr uint32_t kPressureScenario = P4_NANO_AUDIO86_PRESSURE_SCENARIO;
enum : uint32_t { kFailureNone = 0U, kFailureStop = 1U, kFailureFatal = 2U };
constexpr uint32_t kFailureKind = P4_NANO_AUDIO86_FAILURE_KIND;
constexpr uint32_t kErrorInjectedFatal = 86U;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
enum : uint32_t { kPcmLifecycleNone = 0U, kPcmLifecycleStopFull = 1U,
                  kPcmLifecycleFatalFull = 2U,
                  kPcmLifecycleConsumerFailureFull = 3U,
                  kPcmLifecycleConsumerFailureEmpty = 4U,
                  kPcmLifecycleRetryStop = 5U,
                  kPcmLifecycleRetryFatal = 6U,
                  kPcmLifecycleRetryPrimaryFirst = 7U,
                  kPcmLifecycleRetryConsumerFirst = 8U };
constexpr uint32_t kPcmLifecycleScenario =
    P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO;
constexpr bool kPcmRetryLifecycle =
    kPcmLifecycleScenario >= kPcmLifecycleRetryStop &&
    kPcmLifecycleScenario <= kPcmLifecycleRetryConsumerFirst;
enum : uint32_t { kPcmSinkPermissionHold = 0U,
                  kPcmSinkPermissionAccept = 1U,
                  kPcmSinkPermissionFatal = 2U };
#endif

struct ApplyRecord {
    uint64_t frame = 0U;
    uint64_t sequence = 0U;
    uint32_t opcode = 0U;
    uint32_t action = 0U;
    uint64_t byte_offset = 0U;
    uint32_t byte_count = 0U;
    uint32_t payload = 0U;
};

struct Runtime {
    np2audio86_event_ring events{};
    np2audio86_byte_ring bytes{};
    np2audio86_runtime_control control{};
    np2audio86_runtime_producer_clock producer_clock{};
    np2audio86_runtime_consumer_clock consumer_clock{};
    np2audio86_render_state render{};
    np2audio86_fixture_result render_result{};
    uint8_t source[NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES]{};
    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN]{};
    SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U]{};
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U]{};
    uint8_t full_pcm[kRenderFrames * 4U]{};
    uint8_t pre_reset_pcm[kRenderFrames * 4U]{};
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    np2opngen_pcm_ring pcm_ring{};
    np2_pcm_output_controller pcm_controller{};
    uint8_t ring_pcm[kRenderFrames * 4U]{};
    StaticTask_t pcm_consumer_tcb{};
    StackType_t pcm_consumer_stack[kPcmConsumerStackBytes / sizeof(StackType_t)]{};
    StaticSemaphore_t pcm_ready_storage{};
    StaticSemaphore_t pcm_done_storage{};
    SemaphoreHandle_t pcm_ready = nullptr;
    SemaphoreHandle_t pcm_done_semaphore = nullptr;
    TaskHandle_t pcm_consumer = nullptr;
    std::atomic<uint32_t> pcm_consumer_ready{0U};
    std::atomic<uint32_t> pcm_production_done{0U};
    std::atomic<uint32_t> pcm_consumer_quiescent{0U};
    std::atomic<uint32_t> pcm_consumer_terminal_ack{0U};
    std::atomic<uint32_t> pcm_ring_finished{0U};
    std::atomic<uint32_t> pcm_forced_abort_requested{0U};
    std::atomic<uint32_t> pcm_worker_space_waiting{0U};
    std::atomic<uint32_t> pcm_lifecycle_triggered{0U};
    std::atomic<uint32_t> pcm_forced_abort_published_before_wake{0U};
    std::atomic<uint32_t> pcm_worker_suspended_observed{0U};
    std::atomic<uint32_t> pcm_consumer_suspended_observed{0U};
    std::atomic<uint32_t> pcm_worker_deleted_after_suspended{0U};
    std::atomic<uint32_t> pcm_consumer_deleted_after_suspended{0U};
    /* Test-profile permission is authoritative; counters below are evidence
     * only and never control production RETRY correctness. */
    std::atomic<uint32_t> pcm_sink_permission{kPcmSinkPermissionAccept};
    std::atomic<uint32_t> pcm_retry_waiting{0U};
    std::atomic<uint32_t> pcm_retry_controller_driven{0U};
    std::atomic<uint32_t> pcm_retry_permission_before_wake{0U};
    std::atomic<uint32_t> pcm_post_done_retry_waiting{0U};
    std::atomic<uint32_t> pcm_post_done_permission_before_wake{0U};
    uint32_t pcm_retry_slot_captured = 0U;
    uint32_t pcm_post_done_retry_slot_captured = 0U;
    uint32_t pcm_retry_attempts = 0U;
    uint32_t pcm_retry_wakes = 0U;
    uint32_t pcm_retry_resubmits = 0U;
    uint32_t pcm_retry_identity = 1U;
    uint32_t pcm_retry_tail_held = 0U;
    uint32_t pcm_retry_accepted_held = 0U;
    uint32_t pcm_retry_full_occupancy = 0U;
    uint32_t pcm_retry_worker_resumed = 0U;
    uint32_t pcm_retry_wait_skipped_ready = 0U;
    uint32_t pcm_retry_done_only_after_empty = 0U;
    uint32_t pcm_retry_tail_before = 0U;
    uint32_t pcm_retry_tail_after = 0U;
    uint64_t pcm_retry_accepted_frames_before = 0U;
    uint64_t pcm_retry_accepted_bytes_before = 0U;
    uint64_t pcm_retry_frame_offset = 0U;
    uint32_t pcm_retry_sequence = 0U;
    uint16_t pcm_retry_valid_frames = 0U;
    uint16_t pcm_retry_flags = 0U;
    uint32_t pcm_retry_crc32 = 0U;
    uint8_t pcm_retry_pcm[NP2_OPNGEN_PCM_RING_SLOT_BYTES]{};
    uint32_t pcm_post_done_retry_attempts = 0U;
    uint32_t pcm_post_done_retry_resubmits = 0U;
    uint32_t pcm_post_done_retry_identity = 1U;
    uint32_t pcm_post_done_retry_tail_held = 0U;
    uint32_t pcm_post_done_retry_accepted_held = 0U;
    uint32_t pcm_post_done_retry_observed = 0U;
    uint32_t pcm_post_done_retry_not_eos = 0U;
    uint32_t pcm_post_done_tail_before = 0U;
    uint32_t pcm_post_done_tail_after = 0U;
    uint64_t pcm_post_done_accepted_frames_before = 0U;
    uint64_t pcm_post_done_accepted_bytes_before = 0U;
    uint32_t pcm_post_done_retry_crc32 = 0U;
    uint64_t pcm_produced_frames = 0U;
    uint64_t pcm_produced_bytes = 0U;
    uint32_t pcm_produced_slots = 0U;
    uint32_t pcm_consumed_slots = 0U;
    uint32_t pcm_partial_slots = 0U;
    uint32_t pcm_drops = 0U;
    uint32_t pcm_overwrites = 0U;
    uint32_t reset_ring_owned_frames = 0U;
    uint32_t reset_applied_after_ring = 0U;
    uint32_t reset_ack_after_ring = 0U;
    uint32_t pcm_first_submit_occupancy = 0U;
    uint32_t pcm_sink_started = 0U;
    uint32_t pcm_sink_finished = 0U;
    uint32_t pcm_forced_abort = 0U;
    uint32_t pcm_join_timeout = 0U;
    uint32_t pcm_worker_join_timeout = 0U;
    uint32_t pcm_sink_abort_calls = 0U;
    uint64_t pcm_abandoned_published_frames = 0U;
    uint64_t pcm_abandoned_partial_frames = 0U;
    uint64_t pcm_abandoned_rendered_frames = 0U;
    uint32_t pcm_abort_pre_cleanup_occupancy = 0U;
    uint16_t pcm_abort_pre_cleanup_partial = 0U;
    uint32_t pcm_ring_before_done = 0U;
    uint32_t pcm_eos_after_done = 0U;
    uint32_t pcm_finish_after_empty = 0U;
    uint32_t pcm_ack_after_finish = 0U;
    uint64_t pcm_slot_offsets[10]{};
    uint32_t pcm_slot_sequences[10]{};
    uint16_t pcm_slot_frames[10]{};
    uint16_t pcm_slot_flags[10]{};
    uint32_t pcm_slot_crc32[10]{};
#endif
    ApplyRecord applied[32]{};
    np2audio86_guest_event_t trace_events[64]{};
    np2audio86_guest_data_run_t trace_runs[8]{};
    uint8_t trace_bytes[64]{};
    np2audio86_guest_timer_trace_t trace_timers[64]{};
    np2audio86_guest_io_trace_t trace_io[128]{};
    np2audio86_guest_trace_t trace{};
    np2audio86_guest_state_snapshot_t final_state{};
    StaticTask_t worker_tcb{};
    StackType_t worker_stack[kWorkerStackBytes / sizeof(StackType_t)]{};
    StaticSemaphore_t ready_storage{};
    StaticSemaphore_t done_storage{};
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t producer = nullptr;
    TaskHandle_t worker = nullptr;
    np2runtime::Runtime *lifecycle_runtime = nullptr;
    std::atomic<uint32_t> producer_waiting{0U};
    std::atomic<uint32_t> worker_ready{0U};
    std::atomic<uint32_t> worker_quiescent{0U};
    std::atomic<uint32_t> applied_count{0U};
    std::atomic<uint32_t> producer_done{0U};
    std::atomic<uint32_t> first_error{0U};
    uint32_t generation = 0U;
    uint32_t reserved_events = 0U;
    uint32_t reserved_bytes = 0U;
    uint32_t transaction_kind = 0U;
    uint32_t transaction_generation = 0U;
    uint32_t active_generation = 0U;
    bool transaction_active = false;
    bool horizon_owned = false;
    bool event_committed = false;
    bool run_committed = false;
    uint64_t next_sequence = 0U;
    uint32_t reset_ordinal = 0U;
    uint64_t rendered_frame = 0U;
    uint64_t worker_byte_offset = 0U;
    uint64_t pre_reset_frame = 0U;
    bool reset_seen = false;
    /* Profile-only logical leases: they affect exactly the same availability
     * predicates as the real rings, but never publish a semantic record. */
    std::atomic<uint32_t> pressure_phase{0U};
    std::atomic<uint32_t> event_lease{0U};
    std::atomic<uint32_t> byte_lease{0U};
    std::atomic<uint32_t> horizon_lease{0U};
    std::atomic<uint32_t> reset_ack_held{0U};
    std::atomic<uint32_t> pressure_resume_count{0U};
    std::atomic<uint32_t> pressure_index0_isolated{0U};
    std::atomic<uint32_t> pressure_released{0U};
    std::atomic<uint32_t> pressure_ack_published{0U};
    uint32_t pressure_ip_before = 0U;
    uint32_t pressure_ip_after = 0U;
    uint32_t pressure_position_before = 0U;
    uint32_t pressure_position_after = 0U;
    uint32_t pressure_snapshot_before = 0U;
    uint32_t pressure_snapshot_after = 0U;
    std::atomic<uint32_t> failure_injected{0U};
    std::atomic<uint32_t> failure_wait_confirmed{0U};
    std::atomic<uint32_t> failure_predicate_published{0U};
    std::atomic<uint32_t> failure_producer_wake{0U};
    std::atomic<uint32_t> failure_worker_wake{0U};
    std::atomic<uint32_t> failure_sequence{0U};
    std::atomic<uint32_t> failure_later_guest_instructions{0U};
    std::atomic<uint32_t> failure_reset_closed{0U};
    std::atomic<uint32_t> failure_first_error_after_cleanup{0U};
    /* Profile-only BYTE_EXTEND evidence.  These fields observe the existing
     * adapter close path; they do not participate in authorization or commit. */
    uint32_t byte_extend_pending_at_wait = 0U;
    uint32_t byte_extend_run_bytes_at_wait = 0U;
    uint32_t byte_extend_first_byte = 0U;
    uint32_t byte_extend_transport_bytes_at_wait = 0U;
    uint32_t byte_extend_descriptor_owned_at_wait = 0U;
    uint32_t byte_extend_horizon_owned_at_wait = 0U;
    uint32_t byte_extend_terminal_order = 0U;
    uint32_t byte_extend_terminal_reserve_calls = 0U;
    uint32_t byte_extend_terminal_extend_calls = 0U;
    uint32_t byte_extend_terminal_control_rechecks = 0U;
    uint32_t byte_extend_run_commits = 0U;
    uint32_t byte_extend_horizon_commits = 0U;
    uint32_t byte_extend_sink_bound_run = 0U;
    uint32_t byte_extend_sink_bound_horizon = 0U;
    uint32_t byte_extend_run_count = 0U;
    uint32_t byte_extend_run_byte = 0U;
    uint64_t byte_extend_run_frame = 0U;
    uint64_t byte_extend_run_sequence = 0U;
    uint64_t byte_extend_run_offset = 0U;
    uint32_t byte_extend_cleanup_after_close = 0U;
    uint32_t byte_extend_done_after_close = 0U;
    std::atomic<uint32_t> byte_extend_stale_notifications_injected{0U};
    std::atomic<uint32_t> byte_extend_stale_notifications_consumed{0U};
    std::atomic<uint32_t> byte_extend_stale_wake_returns{0U};
    std::atomic<uint32_t> byte_extend_stale_phase_advances{0U};
    std::atomic<uint32_t> byte_extend_stale_guest_progress{0U};
    std::atomic<uint32_t> byte_extend_release_observed{0U};
    std::atomic<uint32_t> byte_extend_second_authorized{0U};
    std::atomic<uint32_t> byte_extend_second_committed{0U};
};

DRAM_ATTR Runtime s_runtime{};
static_assert(sizeof(np2audio86_event_ring) == 3080U);
static_assert(sizeof(np2audio86_byte_ring) == 65544U);
static_assert(sizeof(np2audio86_runtime_control) == 28U);

void notify_producer(Runtime *runtime)
{
    if (runtime->producer != nullptr)
        (void)p4_nano_audio86_notifications::notify_producer(runtime->producer);
}

void notify_worker(Runtime *runtime)
{
    if (runtime->worker != nullptr)
        (void)p4_nano_audio86_notifications::notify_worker(runtime->worker);
}

bool failed(const Runtime *runtime);
void fail(Runtime *runtime, uint32_t error);
void publish_failure(Runtime *runtime);

#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
void notify_pcm_consumer(Runtime *runtime)
{
    if (runtime->pcm_consumer != nullptr)
        (void)xTaskNotifyGiveIndexed(runtime->pcm_consumer, 0U);
}

void publish_pcm_forced_abort(Runtime *runtime, const uint32_t error)
{
    uint32_t expected = 0U;
    if (runtime->first_error.compare_exchange_strong(expected, error,
                                                      std::memory_order_acq_rel)) {
        (void)np2audio86_runtime_first_error_publish(&runtime->control, error);
        if (runtime->lifecycle_runtime != nullptr)
            (void)runtime->lifecycle_runtime->mark_failure();
    }
    runtime->pcm_forced_abort_requested.store(1U, std::memory_order_release);
    runtime->pcm_forced_abort_published_before_wake.store(1U,
                                                          std::memory_order_release);
    notify_producer(runtime);
    notify_worker(runtime);
    notify_pcm_consumer(runtime);
}

enum np2_pcm_sink_result pcm_sink_start(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr) return NP2_PCM_SINK_FATAL;
    runtime->pcm_sink_started = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

enum np2_pcm_sink_result pcm_sink_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || view == nullptr || view->valid_frames == 0U ||
        runtime->pcm_consumed_slots >= 10U ||
        view->valid_frames > NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES ||
        view->frame_offset > kRenderFrames ||
        view->valid_frames > kRenderFrames - view->frame_offset ||
        view->sequence != runtime->pcm_consumed_slots)
        return NP2_PCM_SINK_FATAL;
    const size_t bytes = static_cast<size_t>(view->valid_frames) * 4U;
    const bool post_done_retry_slot = kPcmRetryLifecycle &&
        (kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
         kPcmLifecycleScenario == kPcmLifecycleRetryFatal) &&
        view->sequence == kExpectedPcmSlots - 1U &&
        runtime->pcm_retry_controller_driven.load(std::memory_order_acquire) != 0U;
    if (post_done_retry_slot &&
        (runtime->pcm_post_done_retry_slot_captured == 0U ||
         runtime->pcm_post_done_retry_waiting.load(
             std::memory_order_acquire) != 0U)) {
        if (runtime->pcm_post_done_retry_slot_captured == 0U) {
            runtime->pcm_post_done_tail_before =
                runtime->pcm_ring.tail.load(std::memory_order_acquire);
            runtime->pcm_post_done_accepted_frames_before =
                runtime->pcm_controller.accepted_frames;
            runtime->pcm_post_done_accepted_bytes_before =
                runtime->pcm_controller.accepted_bytes;
            runtime->pcm_post_done_retry_crc32 = np2_crc32_iso_hdlc_finish(
                np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(),
                                          view->pcm, bytes));
            std::memcpy(runtime->pcm_retry_pcm, view->pcm, bytes);
            runtime->pcm_post_done_retry_slot_captured = 1U;
        } else {
            const uint32_t crc = np2_crc32_iso_hdlc_finish(
                np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(),
                                          view->pcm, bytes));
            if (runtime->pcm_post_done_retry_crc32 != crc ||
                std::memcmp(runtime->pcm_retry_pcm, view->pcm, bytes) != 0)
                runtime->pcm_post_done_retry_identity = 0U;
            ++runtime->pcm_post_done_retry_resubmits;
        }
        ++runtime->pcm_post_done_retry_attempts;
        if (runtime->pcm_sink_permission.load(std::memory_order_acquire) ==
            kPcmSinkPermissionHold) {
            runtime->pcm_post_done_retry_waiting.store(
                1U, std::memory_order_release);
            return NP2_PCM_SINK_RETRY;
        }
    }
    if (kPcmRetryLifecycle &&
        (runtime->pcm_retry_slot_captured == 0U ||
         runtime->pcm_retry_waiting.load(std::memory_order_acquire) != 0U)) {
        const uint32_t permission =
            runtime->pcm_sink_permission.load(std::memory_order_acquire);
        if (runtime->pcm_retry_slot_captured == 0U) {
            runtime->pcm_retry_tail_before =
                runtime->pcm_ring.tail.load(std::memory_order_acquire);
            runtime->pcm_retry_accepted_frames_before =
                runtime->pcm_controller.accepted_frames;
            runtime->pcm_retry_accepted_bytes_before =
                runtime->pcm_controller.accepted_bytes;
            runtime->pcm_retry_frame_offset = view->frame_offset;
            runtime->pcm_retry_sequence = view->sequence;
            runtime->pcm_retry_valid_frames = view->valid_frames;
            runtime->pcm_retry_flags = view->flags;
            runtime->pcm_retry_crc32 = np2_crc32_iso_hdlc_finish(
                np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(),
                                          view->pcm, bytes));
            std::memcpy(runtime->pcm_retry_pcm, view->pcm, bytes);
            runtime->pcm_retry_slot_captured = 1U;
        } else {
            const uint32_t crc = np2_crc32_iso_hdlc_finish(
                np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(),
                                          view->pcm, bytes));
            if (runtime->pcm_retry_frame_offset != view->frame_offset ||
                runtime->pcm_retry_sequence != view->sequence ||
                runtime->pcm_retry_valid_frames != view->valid_frames ||
                runtime->pcm_retry_flags != view->flags ||
                runtime->pcm_retry_crc32 != crc ||
                std::memcmp(runtime->pcm_retry_pcm, view->pcm, bytes) != 0)
                runtime->pcm_retry_identity = 0U;
            ++runtime->pcm_retry_resubmits;
        }
        ++runtime->pcm_retry_attempts;
        if (permission == kPcmSinkPermissionHold) {
            runtime->pcm_retry_waiting.store(1U, std::memory_order_release);
            return NP2_PCM_SINK_RETRY;
        }
        if (permission == kPcmSinkPermissionFatal)
            return NP2_PCM_SINK_FATAL;
    }
    if (runtime->pcm_consumed_slots == 0U)
        runtime->pcm_first_submit_occupancy =
            np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
    std::memcpy(runtime->ring_pcm + static_cast<size_t>(view->frame_offset) * 4U,
                view->pcm, bytes);
    const uint32_t slot = runtime->pcm_consumed_slots;
    runtime->pcm_slot_offsets[slot] = view->frame_offset;
    runtime->pcm_slot_sequences[slot] = view->sequence;
    runtime->pcm_slot_frames[slot] = view->valid_frames;
    runtime->pcm_slot_flags[slot] = view->flags;
    runtime->pcm_slot_crc32[slot] = np2_crc32_iso_hdlc_finish(
        np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(), view->pcm, bytes));
    ++runtime->pcm_consumed_slots;
    if (view->valid_frames < NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES)
        ++runtime->pcm_partial_slots;
    return NP2_PCM_SINK_ACCEPTED;
}

void drive_pcm_retry_controller(Runtime *runtime)
{
    /* Profile-only rendezvous: level state is published before the wake hint. */
    if (!kPcmRetryLifecycle ||
        runtime->pcm_retry_waiting.load(std::memory_order_acquire) == 0U ||
        runtime->pcm_worker_space_waiting.load(std::memory_order_acquire) == 0U ||
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) !=
            NP2_OPNGEN_PCM_RING_CAPACITY ||
        runtime->pcm_retry_controller_driven.exchange(
            1U, std::memory_order_acq_rel) != 0U)
        return;
    runtime->pcm_retry_full_occupancy =
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
    runtime->pcm_lifecycle_triggered.store(1U, std::memory_order_release);
    if (kPcmLifecycleScenario != kPcmLifecycleRetryConsumerFirst)
        publish_failure(runtime);
    const uint32_t permission =
        kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
        kPcmLifecycleScenario == kPcmLifecycleRetryFatal
            ? kPcmSinkPermissionAccept : kPcmSinkPermissionFatal;
    runtime->pcm_sink_permission.store(permission, std::memory_order_release);
    runtime->pcm_retry_permission_before_wake.store(1U,
                                                    std::memory_order_release);
    notify_pcm_consumer(runtime);
}

void resolve_post_done_retry(Runtime *runtime)
{
    if (runtime->pcm_post_done_retry_waiting.load(
            std::memory_order_acquire) == 0U ||
        runtime->pcm_production_done.load(std::memory_order_acquire) == 0U)
        return;
    const uint32_t occupancy =
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
    if (occupancy == 0U) return;
    runtime->pcm_post_done_retry_observed = occupancy;
    runtime->pcm_post_done_retry_not_eos = runtime->pcm_eos_after_done == 0U
        ? 1U : 0U;
    runtime->pcm_sink_permission.store(kPcmSinkPermissionAccept,
                                       std::memory_order_release);
    runtime->pcm_post_done_permission_before_wake.store(
        1U, std::memory_order_release);
    notify_pcm_consumer(runtime);
}

enum np2_pcm_sink_result pcm_sink_finish(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr) return NP2_PCM_SINK_FATAL;
    runtime->pcm_sink_finished = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

enum np2_pcm_sink_result pcm_sink_abort(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr) return NP2_PCM_SINK_FATAL;
    ++runtime->pcm_sink_abort_calls;
    runtime->pcm_forced_abort = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

const np2_pcm_sink kPcmSink{&s_runtime, pcm_sink_start, pcm_sink_submit,
                            pcm_sink_finish, pcm_sink_abort};

bool append_pcm(Runtime *runtime, const uint8_t *pcm, const size_t frames,
                const uint64_t frame_offset)
{
    size_t appended = 0U;
    while (appended < frames) {
        if (runtime->pcm_forced_abort_requested.load(
                std::memory_order_acquire) != 0U) {
            runtime->pcm_abandoned_rendered_frames += frames - appended;
            return false;
        }
        size_t consumed = 0U;
        const int status = np2opngen_pcm_ring_append(
            &runtime->pcm_ring, pcm + appended * 4U, frames - appended,
            frame_offset + appended, &consumed);
        appended += consumed;
        runtime->pcm_produced_frames += consumed;
        runtime->pcm_produced_bytes += consumed * 4U;
        notify_pcm_consumer(runtime);
        if (status == NP2_OPNGEN_PCM_RING_OK) continue;
        if (status != NP2_OPNGEN_PCM_RING_FULL) return false;
        runtime->pcm_worker_space_waiting.store(1U, std::memory_order_release);
        notify_pcm_consumer(runtime);
        drive_pcm_retry_controller(runtime);
        if (runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U) {
            runtime->pcm_worker_space_waiting.store(0U, std::memory_order_release);
            runtime->pcm_abandoned_rendered_frames += frames - appended;
            return false;
        }
        if (np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) ==
            NP2_OPNGEN_PCM_RING_CAPACITY)
            (void)p4_nano_audio86_notifications::wait_worker();
        if (kPcmRetryLifecycle &&
            runtime->pcm_forced_abort_requested.load(
                std::memory_order_acquire) == 0U)
            runtime->pcm_retry_worker_resumed = 1U;
        runtime->pcm_worker_space_waiting.store(0U, std::memory_order_release);
    }
    runtime->pcm_produced_slots =
        static_cast<uint32_t>(runtime->pcm_produced_frames /
                              NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
    return true;
}

bool finish_pcm(Runtime *runtime)
{
    for (;;) {
        if (runtime->pcm_forced_abort_requested.load(
                std::memory_order_acquire) != 0U)
            return false;
        const int status = np2opngen_pcm_ring_finish(
            &runtime->pcm_ring, runtime->pcm_produced_frames);
        if (status == NP2_OPNGEN_PCM_RING_OK) {
            if ((runtime->pcm_produced_frames % NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES) != 0U)
                ++runtime->pcm_produced_slots;
            runtime->pcm_ring_finished.store(1U, std::memory_order_release);
            runtime->pcm_ring_before_done = 1U;
            runtime->pcm_production_done.store(1U, std::memory_order_release);
            resolve_post_done_retry(runtime);
            notify_pcm_consumer(runtime);
            return true;
        }
        if (status != NP2_OPNGEN_PCM_RING_FULL) return false;
        runtime->pcm_worker_space_waiting.store(1U, std::memory_order_release);
        notify_pcm_consumer(runtime);
        if (runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U) {
            runtime->pcm_worker_space_waiting.store(0U, std::memory_order_release);
            return false;
        }
        if (np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) ==
            NP2_OPNGEN_PCM_RING_CAPACITY)
            (void)p4_nano_audio86_notifications::wait_worker();
        runtime->pcm_worker_space_waiting.store(0U, std::memory_order_release);
    }
}

void pcm_consumer_task(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || np2_pcm_output_start(&runtime->pcm_controller) !=
                                  NP2_PCM_OUTPUT_OK) {
        if (runtime != nullptr) fail(runtime, kErrorWorker);
        vTaskDelete(nullptr);
        return;
    }
    runtime->pcm_consumer_ready.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->pcm_ready);
    bool released = false;
    for (;;) {
        const uint32_t occupancy = np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
        const bool production_done =
            runtime->pcm_production_done.load(std::memory_order_acquire) != 0U;
        if (kPcmLifecycleScenario == kPcmLifecycleConsumerFailureEmpty &&
            runtime->pcm_lifecycle_triggered.exchange(1U,
                                                       std::memory_order_acq_rel) == 0U)
            publish_pcm_forced_abort(runtime, kErrorWorker);
        if ((kPcmLifecycleScenario == kPcmLifecycleStopFull ||
             kPcmLifecycleScenario == kPcmLifecycleFatalFull ||
             kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull) &&
            occupancy == NP2_OPNGEN_PCM_RING_CAPACITY &&
            runtime->pcm_worker_space_waiting.load(std::memory_order_acquire) != 0U &&
            runtime->pcm_lifecycle_triggered.exchange(1U,
                                                       std::memory_order_acq_rel) == 0U) {
            if (kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull)
                publish_pcm_forced_abort(runtime, kErrorWorker);
            else
                publish_failure(runtime);
            released = true;
        }
        if (!released && (occupancy >= kPcmPrefillSlots ||
                          (production_done && occupancy != 0U)))
            released = kPcmLifecycleScenario == kPcmLifecycleNone ||
                       kPcmRetryLifecycle;
        if (runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U) {
            if (np2_pcm_output_abort(&runtime->pcm_controller) != NP2_PCM_OUTPUT_OK)
                fail(runtime, kErrorWorker);
            runtime->pcm_consumer_terminal_ack.store(1U, std::memory_order_release);
            break;
        }
        if (released && occupancy != 0U) {
            const enum np2_pcm_output_status status =
                np2_pcm_output_step(&runtime->pcm_controller);
            if (status == NP2_PCM_OUTPUT_RETRY) {
                const bool post_done_retry =
                    runtime->pcm_post_done_retry_waiting.load(
                        std::memory_order_acquire) != 0U;
                if (post_done_retry) {
                    runtime->pcm_post_done_retry_tail_held =
                        runtime->pcm_ring.tail.load(std::memory_order_acquire) ==
                                runtime->pcm_post_done_tail_before
                            ? 1U : 0U;
                    runtime->pcm_post_done_retry_accepted_held =
                        runtime->pcm_controller.accepted_frames ==
                                runtime->pcm_post_done_accepted_frames_before &&
                            runtime->pcm_controller.accepted_bytes ==
                                runtime->pcm_post_done_accepted_bytes_before
                            ? 1U : 0U;
                    resolve_post_done_retry(runtime);
                } else {
                    runtime->pcm_retry_tail_held =
                        runtime->pcm_ring.tail.load(std::memory_order_acquire) ==
                                runtime->pcm_retry_tail_before
                            ? 1U : 0U;
                    runtime->pcm_retry_accepted_held =
                        runtime->pcm_controller.accepted_frames ==
                                runtime->pcm_retry_accepted_frames_before &&
                            runtime->pcm_controller.accepted_bytes ==
                                runtime->pcm_retry_accepted_bytes_before
                            ? 1U : 0U;
                    runtime->pcm_retry_waiting.store(
                        1U, std::memory_order_release);
                }
                notify_worker(runtime);
                /* A notification may arrive before this loop.  Rechecking the
                 * level predicates before sleeping closes that lost-wake window. */
                while (runtime->pcm_forced_abort_requested.load(
                           std::memory_order_acquire) == 0U &&
                       runtime->pcm_sink_permission.load(
                           std::memory_order_acquire) == kPcmSinkPermissionHold) {
                    (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, portMAX_DELAY);
                    ++runtime->pcm_retry_wakes;
                }
                if (runtime->pcm_retry_wakes == 0U)
                    runtime->pcm_retry_wait_skipped_ready = 1U;
                continue;
            }
            if (status != NP2_PCM_OUTPUT_CONSUMED) {
                runtime->pcm_retry_tail_after =
                    runtime->pcm_ring.tail.load(std::memory_order_acquire);
                publish_pcm_forced_abort(runtime, kErrorWorker);
                if (kPcmLifecycleScenario ==
                    kPcmLifecycleRetryConsumerFirst)
                    publish_failure(runtime);
                continue;
            }
            if (kPcmRetryLifecycle &&
                runtime->pcm_retry_waiting.load(
                    std::memory_order_acquire) != 0U) {
                runtime->pcm_retry_tail_after =
                    runtime->pcm_ring.tail.load(std::memory_order_acquire);
                runtime->pcm_retry_waiting.store(0U,
                                                 std::memory_order_release);
                if (kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
                    kPcmLifecycleScenario == kPcmLifecycleRetryFatal)
                    runtime->pcm_sink_permission.store(
                        kPcmSinkPermissionHold, std::memory_order_release);
            }
            if (runtime->pcm_post_done_retry_waiting.load(
                    std::memory_order_acquire) != 0U) {
                runtime->pcm_post_done_tail_after =
                    runtime->pcm_ring.tail.load(std::memory_order_acquire);
                runtime->pcm_post_done_retry_waiting.store(
                    0U, std::memory_order_release);
            }
            notify_worker(runtime);
            continue;
        }
        if (production_done && occupancy == 0U) {
            runtime->pcm_retry_done_only_after_empty =
                runtime->pcm_retry_waiting.load(std::memory_order_acquire) == 0U
                    ? 1U : 0U;
            runtime->pcm_eos_after_done = 1U;
            runtime->pcm_finish_after_empty = 1U;
            if (np2_pcm_output_finish(&runtime->pcm_controller) != NP2_PCM_OUTPUT_OK)
                fail(runtime, kErrorWorker);
            else {
                runtime->pcm_ack_after_finish = runtime->pcm_sink_finished;
                runtime->pcm_consumer_terminal_ack.store(1U, std::memory_order_release);
            }
            break;
        }
        (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, portMAX_DELAY);
    }
    runtime->pcm_consumer_quiescent.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->pcm_done_semaphore);
    vTaskSuspend(nullptr);
}
#endif

bool failed(const Runtime *runtime)
{
    return runtime->first_error.load(std::memory_order_acquire) != 0U ||
           np2audio86_runtime_stop_requested(&runtime->control) ||
           np2audio86_runtime_first_error(&runtime->control) != 0U;
}

void fail(Runtime *runtime, const uint32_t error)
{
    uint32_t expected = 0U;
    if (runtime->first_error.compare_exchange_strong(expected, error,
                                                      std::memory_order_acq_rel)) {
        (void)np2audio86_runtime_first_error_publish(&runtime->control, error);
        notify_producer(runtime);
        notify_worker(runtime);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        notify_pcm_consumer(runtime);
#endif
    }
}

/* C3's controller is deliberately a thin wrapper over the existing Runtime
 * lifecycle and the existing transport control word.  It owns neither a
 * second stop state nor a second fatal state. */
void publish_failure(Runtime *runtime)
{
    if (kFailureKind == kFailureNone ||
        runtime->failure_injected.exchange(1U, std::memory_order_acq_rel) != 0U)
        return;
    runtime->failure_wait_confirmed.store(1U, std::memory_order_release);
    if (kFailureKind == kFailureStop) {
        if (runtime->lifecycle_runtime != nullptr)
            (void)runtime->lifecycle_runtime->request_stop();
        np2audio86_runtime_stop_publish(&runtime->control);
    } else {
        uint32_t expected = 0U;
        if (runtime->first_error.compare_exchange_strong(expected, kErrorInjectedFatal,
                                                          std::memory_order_acq_rel)) {
            if (runtime->lifecycle_runtime != nullptr)
                (void)runtime->lifecycle_runtime->mark_failure();
            (void)np2audio86_runtime_first_error_publish(&runtime->control,
                                                         kErrorInjectedFatal);
        }
    }
    runtime->failure_predicate_published.store(1U, std::memory_order_release);
    runtime->failure_sequence.store(1U, std::memory_order_release);
    /* The predicate publication above must happen-before either indexed wake. */
    notify_producer(runtime);
    runtime->failure_producer_wake.store(1U, std::memory_order_release);
    runtime->failure_sequence.store(2U, std::memory_order_release);
    notify_worker(runtime);
    runtime->failure_worker_wake.store(1U, std::memory_order_release);
    runtime->failure_sequence.store(3U, std::memory_order_release);
    /* Controller-owned leases are released only after the terminal predicate
     * and both specified wakes.  They never create a guest semantic record. */
    runtime->event_lease.store(0U, std::memory_order_release);
    runtime->byte_lease.store(0U, std::memory_order_release);
    runtime->horizon_lease.store(0U, std::memory_order_release);
    if (runtime->reset_ack_held.exchange(0U, std::memory_order_acq_rel) != 0U) {
        np2audio86_runtime_reset_ack_publish(&runtime->control,
                                             runtime->reset_ordinal + 1U);
        runtime->pressure_ack_published.store(1U, std::memory_order_release);
    }
    runtime->pressure_released.store(1U, std::memory_order_release);
    runtime->failure_reset_closed.store(1U, std::memory_order_release);
}

uint32_t producer_position()
{
    return static_cast<uint32_t>(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
}

uint32_t pressure_snapshot(const Runtime *runtime)
{
    /* This is deliberately producer-generated: Core 0 never dereferences the
     * mutable i286 state.  It covers the stopped instruction position plus
     * the transaction state whose mutation would change the target operation. */
    return static_cast<uint32_t>(i286core.s.r.w.ip) ^ producer_position() ^
           static_cast<uint32_t>(runtime->next_sequence) ^ runtime->reserved_events ^
           runtime->reserved_bytes ^ (runtime->transaction_active ? 0x80000000U : 0U) ^
           (runtime->horizon_owned ? 0x40000000U : 0U);
}

void pressure_capture_before(Runtime *runtime)
{
    runtime->pressure_ip_before = i286core.s.r.w.ip;
    runtime->pressure_position_before = producer_position();
    runtime->pressure_snapshot_before = pressure_snapshot(runtime);
}

void pressure_capture_after(Runtime *runtime)
{
    runtime->pressure_ip_after = i286core.s.r.w.ip;
    runtime->pressure_position_after = producer_position();
    runtime->pressure_snapshot_after = pressure_snapshot(runtime);
}

NEVENTID event_id(const uint8_t timer)
{
    return timer == NP2AUDIO86_TRACE_TIMER_A ? NEVENT_FMTIMERA :
           timer == NP2AUDIO86_TRACE_TIMER_B ? NEVENT_FMTIMERB : NEVENT_86PCM;
}

void timer_dispatch(NEVENTITEM item)
{
    np2audio86_guest_host_timer_dispatch(static_cast<uint8_t>(item->userData));
}

void timer_schedule(const uint8_t timer, const uint64_t clock, const uint8_t absolute)
{
    if (clock > UINT32_MAX) { fail(&s_runtime, kErrorGuest); return; }
    nevent_set(event_id(timer), static_cast<SINT32>(clock), timer_dispatch,
               absolute ? NEVENT_ABSOLUTE : NEVENT_RELATIVE);
    g_nevent.item[event_id(timer)].userData = timer;
}

void timer_cancel(const uint8_t timer) { nevent_reset(event_id(timer)); }
uint8_t timer_iswork(const uint8_t timer)
{ return nevent_iswork(event_id(timer)) ? 1U : 0U; }
void timer_irq(const uint32_t irq, const uint8_t level)
{ if (level) pic_setirq(static_cast<REG8>(irq)); else pic_resetirq(static_cast<REG8>(irq)); }

void token_set(Runtime *runtime, np2audio86_guest_transaction_t *token,
               const uint32_t kind)
{
    std::memset(token, 0, sizeof(*token));
    token->opaque[0] = reinterpret_cast<uintptr_t>(runtime);
    token->opaque[1] = runtime->active_generation;
    token->opaque[2] = kind;
    token->opaque[3] = 1U;
}

bool token_matches(const Runtime *runtime, const np2audio86_guest_transaction_t *token,
                   const uint32_t kind)
{
    return runtime->transaction_active && token != nullptr &&
           token->opaque[0] == reinterpret_cast<uintptr_t>(runtime) &&
           token->opaque[1] == runtime->active_generation &&
           token->opaque[2] == kind && token->opaque[3] == 1U &&
           runtime->transaction_kind == kind;
}

int wait_for_capacity(Runtime *runtime, const size_t bytes)
{
    for (;;) {
        if (failed(runtime)) return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
        const bool event_space = np2audio86_event_ring_occupancy(&runtime->events) +
                                 runtime->reserved_events + runtime->event_lease.load(std::memory_order_acquire) < NP2_AUDIO86_ASYNC_EVENT_CAPACITY;
        const bool byte_space = np2audio86_byte_ring_occupancy(&runtime->bytes) +
                                runtime->reserved_bytes + runtime->byte_lease.load(std::memory_order_acquire) + bytes <= NP2_AUDIO86_ASYNC_BYTE_CAPACITY;
        const bool horizon_empty = !np2audio86_runtime_horizon_pending(&runtime->control) &&
                                   runtime->horizon_lease.load(std::memory_order_acquire) == 0U;
        if (event_space && byte_space && horizon_empty) return NP2AUDIO86_GUEST_TRANSACTION_OK;
        const bool pressure_wait = kPressureScenario != kPressureResetAck &&
            runtime->pressure_phase.load(std::memory_order_acquire) == 2U;
        if (pressure_wait) {
            pressure_capture_before(runtime);
            runtime->pressure_phase.store(3U, std::memory_order_release); /* PRODUCER_WAITING */
        }
        runtime->producer_waiting.store(1U, std::memory_order_release);
        notify_worker(runtime);
        (void)p4_nano_audio86_notifications::wait_producer();
        runtime->producer_waiting.store(0U, std::memory_order_release);
        if (pressure_wait) {
            pressure_capture_after(runtime);
            runtime->pressure_resume_count.fetch_add(1U, std::memory_order_relaxed);
            runtime->pressure_phase.store(5U, std::memory_order_release); /* PRODUCER_WOKE */
        }
    }
}

void arm_pressure_lease(Runtime *runtime, const uint32_t kind)
{
    if (runtime->pressure_phase.load(std::memory_order_acquire) != 1U) return;
    const bool target = (kPressureScenario == kPressureEvent && kind == NP2AUDIO86_GUEST_TRANSACTION_EVENT && runtime->next_sequence == 0U) ||
        (kPressureScenario == kPressureByte && kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN && runtime->next_sequence == 16U) ||
        (kPressureScenario == kPressureHorizon && kind == NP2AUDIO86_GUEST_TRANSACTION_EVENT && runtime->next_sequence == 0U);
    if (!target) return;
    if (kPressureScenario == kPressureEvent)
        runtime->event_lease.store(NP2_AUDIO86_ASYNC_EVENT_CAPACITY, std::memory_order_release);
    else if (kPressureScenario == kPressureByte)
        runtime->byte_lease.store(NP2_AUDIO86_ASYNC_BYTE_CAPACITY, std::memory_order_release);
    else
        runtime->horizon_lease.store(1U, std::memory_order_release);
    runtime->pressure_phase.store(2U, std::memory_order_release); /* LEASE_HELD */
    notify_worker(runtime);
}

void arm_byte_extend_lease(Runtime *runtime)
{
    if (kPressureScenario != kPressureByteExtend ||
        runtime->pressure_phase.load(std::memory_order_acquire) != 1U ||
        runtime->next_sequence != 16U || runtime->reserved_bytes != 0U ||
        !runtime->transaction_active || !runtime->horizon_owned ||
        np2audio86_byte_ring_occupancy(&runtime->bytes) != 1U)
        return;
    runtime->byte_extend_pending_at_wait = 1U;
    runtime->byte_extend_run_bytes_at_wait =
        static_cast<uint32_t>(runtime->trace.pcm_count);
    runtime->byte_extend_first_byte = runtime->trace.pcm_count == 1U
        ? runtime->trace.pcm_bytes[0] : UINT32_MAX;
    runtime->byte_extend_transport_bytes_at_wait =
        np2audio86_byte_ring_occupancy(&runtime->bytes);
    runtime->byte_extend_descriptor_owned_at_wait = runtime->reserved_events;
    runtime->byte_extend_horizon_owned_at_wait = runtime->horizon_owned ? 1U : 0U;
    runtime->byte_lease.store(NP2_AUDIO86_ASYNC_BYTE_CAPACITY,
                              std::memory_order_release);
    runtime->pressure_phase.store(2U, std::memory_order_release); /* LEASE_HELD */
    /* Deterministically preserve several non-release notifications.  They are
     * deliberately coalesced by ulTaskNotifyTakeIndexed(pdTRUE): the producer
     * must treat the returned count as a hint and recheck the release level. */
    constexpr uint32_t kStaleNotifications = 3U;
    runtime->byte_extend_stale_notifications_injected.store(
        kStaleNotifications, std::memory_order_release);
    for (uint32_t i = 0U; i < kStaleNotifications; ++i) notify_producer(runtime);
    notify_worker(runtime);
}

int reserve_checked(void *opaque, const uint32_t kind, const size_t bytes,
                    np2audio86_guest_transaction_t *token)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime != nullptr && kPressureScenario == kPressureByteExtend &&
        runtime->byte_extend_terminal_order != 0U)
        ++runtime->byte_extend_terminal_reserve_calls;
    if (runtime == nullptr || token == nullptr || runtime->transaction_active ||
        (kind != NP2AUDIO86_GUEST_TRANSACTION_EVENT &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_RESET) ||
        (kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN ? bytes != 1U : bytes != 0U)) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    arm_pressure_lease(runtime, kind);
    const int status = wait_for_capacity(runtime, bytes);
    if (status != NP2AUDIO86_GUEST_TRANSACTION_OK) return status;
    if (failed(runtime)) return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
    runtime->reserved_events = 1U;
    runtime->reserved_bytes = static_cast<uint32_t>(bytes);
    runtime->transaction_kind = kind;
    runtime->transaction_active = true;
    runtime->horizon_owned = true;
    runtime->event_committed = false;
    runtime->run_committed = false;
    runtime->active_generation = ++runtime->transaction_generation;
    token_set(runtime, token, kind);
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

int extend_checked(void *opaque, np2audio86_guest_transaction_t *token,
                   const size_t bytes)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime != nullptr && kPressureScenario == kPressureByteExtend &&
        runtime->byte_extend_terminal_order != 0U)
        ++runtime->byte_extend_terminal_extend_calls;
    if (runtime == nullptr || !token_matches(runtime, token,
                                               NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        bytes != 1U || runtime->run_committed) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    arm_byte_extend_lease(runtime);
    for (;;) {
        if (failed(runtime)) {
            if (kPressureScenario == kPressureByteExtend)
                runtime->byte_extend_terminal_order = 1U; /* EXTEND_TERMINATED */
            return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
        }
        if (np2audio86_byte_ring_occupancy(&runtime->bytes) + runtime->reserved_bytes +
                runtime->byte_lease.load(std::memory_order_acquire) <
            NP2_AUDIO86_ASYNC_BYTE_CAPACITY) break;
        const uint32_t phase_before_wait =
            runtime->pressure_phase.load(std::memory_order_acquire);
        const bool pressure_wait = kPressureScenario == kPressureByteExtend &&
            phase_before_wait == 2U;
        const bool byte_extend_rendezvous = kPressureScenario == kPressureByteExtend &&
            (phase_before_wait == 2U || phase_before_wait == 3U ||
             phase_before_wait == 4U);
        if (pressure_wait) {
            pressure_capture_before(runtime);
            runtime->pressure_phase.store(3U, std::memory_order_release); /* PRODUCER_WAITING */
        }
        runtime->producer_waiting.store(1U, std::memory_order_release);
        notify_worker(runtime);
        const uint32_t notifications =
            p4_nano_audio86_notifications::wait_producer();
        runtime->producer_waiting.store(0U, std::memory_order_release);
        if (byte_extend_rendezvous && !failed(runtime)) {
            const uint32_t phase_after_wait =
                runtime->pressure_phase.load(std::memory_order_acquire);
            const bool released =
                runtime->pressure_released.load(std::memory_order_acquire) != 0U &&
                runtime->byte_lease.load(std::memory_order_acquire) == 0U &&
                phase_after_wait == 4U;
            if (released) {
                pressure_capture_after(runtime);
                runtime->byte_extend_release_observed.store(1U,
                                                             std::memory_order_release);
                runtime->pressure_resume_count.fetch_add(1U, std::memory_order_relaxed);
                runtime->pressure_phase.store(5U, std::memory_order_release); /* PRODUCER_WOKE */
            } else {
                runtime->byte_extend_stale_notifications_consumed.fetch_add(
                    notifications, std::memory_order_relaxed);
                runtime->byte_extend_stale_wake_returns.fetch_add(
                    1U, std::memory_order_relaxed);
                if (i286core.s.r.w.ip != runtime->pressure_ip_before ||
                    producer_position() != runtime->pressure_position_before ||
                    pressure_snapshot(runtime) != runtime->pressure_snapshot_before)
                    runtime->byte_extend_stale_guest_progress.fetch_add(
                        1U, std::memory_order_relaxed);
                if (phase_after_wait != 3U)
                    runtime->byte_extend_stale_phase_advances.fetch_add(
                        1U, std::memory_order_relaxed);
            }
        }
    }
    if (kPressureScenario == kPressureByteExtend &&
        runtime->byte_extend_release_observed.load(std::memory_order_acquire) != 0U &&
        runtime->trace.pcm_count == 1U)
        runtime->byte_extend_second_authorized.fetch_add(1U,
                                                         std::memory_order_relaxed);
    ++runtime->reserved_bytes;
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

void commit_pcm_byte(void *opaque, np2audio86_guest_transaction_t *token,
                     uint64_t, uint64_t sequence, const uint8_t value)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || !token_matches(runtime, token,
                                               NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        runtime->reserved_bytes == 0U || sequence != runtime->next_sequence ||
        np2audio86_byte_ring_push(&runtime->bytes, &value, 1U) != NP2_AUDIO86_TRANSPORT_OK) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return;
    }
    --runtime->reserved_bytes;
    if (kPressureScenario == kPressureByteExtend && value == 0x20U &&
        runtime->trace.pcm_count == 2U &&
        runtime->byte_extend_release_observed.load(std::memory_order_acquire) != 0U)
        runtime->byte_extend_second_committed.fetch_add(1U,
                                                        std::memory_order_relaxed);
    notify_worker(runtime);
}

void commit_data_run(void *opaque, np2audio86_guest_transaction_t *token,
                     const np2audio86_guest_data_run_t *run)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || run == nullptr ||
        !token_matches(runtime, token, NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        runtime->reserved_events != 1U || runtime->reserved_bytes != 0U ||
        run->sequence != runtime->next_sequence || run->count == 0U) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return;
    }
    const np2audio86_event event{run->frame_timestamp, run->sequence,
                                  NP2_AUDIO86_EVENT_PCM86_DATA_RUN, run->count};
    if (np2audio86_event_ring_enqueue(&runtime->events, &event) != NP2_AUDIO86_TRANSPORT_OK) {
        fail(runtime, kErrorTransport); return;
    }
    runtime->reserved_events = 0U;
    runtime->run_committed = true;
    ++runtime->next_sequence;
    if (kPressureScenario == kPressureByteExtend &&
        runtime->byte_extend_terminal_order == 1U) {
        runtime->byte_extend_terminal_order = 2U; /* DATA_RUN_COMMIT */
        runtime->byte_extend_sink_bound_run = 1U;
        ++runtime->byte_extend_run_commits;
        runtime->byte_extend_run_count = run->count;
        runtime->byte_extend_run_byte = runtime->trace.pcm_count == 1U
            ? runtime->trace.pcm_bytes[0] : UINT32_MAX;
        runtime->byte_extend_run_frame = run->frame_timestamp;
        runtime->byte_extend_run_sequence = run->sequence;
        runtime->byte_extend_run_offset = run->byte_offset;
    }
    notify_worker(runtime);
}

void commit_event(void *opaque, np2audio86_guest_transaction_t *token,
                  const np2audio86_guest_event_t *guest)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || guest == nullptr ||
        (!token_matches(runtime, token, NP2AUDIO86_GUEST_TRANSACTION_EVENT) &&
         !token_matches(runtime, token, NP2AUDIO86_GUEST_TRANSACTION_RESET)) ||
        runtime->reserved_events != 1U || guest->sequence != runtime->next_sequence) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return;
    }
    uint32_t opcode = guest->opcode;
    if (opcode == NP2AUDIO86_TRACE_OPNA_REGISTER) opcode = kEventOpnaRegister;
    else if (opcode == NP2AUDIO86_TRACE_OPNA_CSM) opcode = kEventOpnaCsm;
    else if (opcode == NP2AUDIO86_TRACE_PCM_CONTROL) opcode = kEventPcmControl;
    const np2audio86_event event{guest->frame_timestamp, guest->sequence,
                                  opcode, guest->payload};
    if (np2audio86_event_ring_enqueue(&runtime->events, &event) != NP2_AUDIO86_TRANSPORT_OK) {
        fail(runtime, kErrorTransport); return;
    }
    runtime->reserved_events = 0U;
    runtime->event_committed = true;
    ++runtime->next_sequence;
    notify_worker(runtime);
}

void commit_horizon(void *opaque, np2audio86_guest_transaction_t *token,
                    const uint64_t frame)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || token == nullptr || !runtime->transaction_active ||
        !runtime->horizon_owned || (!runtime->event_committed && !runtime->run_committed) ||
        token->opaque[1] != runtime->active_generation) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return;
    }
    if (np2audio86_runtime_horizon_publish(&runtime->control, &runtime->producer_clock,
                                            frame) != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        fail(runtime, kErrorTransport); return;
    }
    const bool reset = runtime->transaction_kind == NP2AUDIO86_GUEST_TRANSACTION_RESET;
    runtime->transaction_active = false;
    runtime->horizon_owned = false;
    runtime->transaction_kind = 0U;
    runtime->event_committed = false;
    runtime->run_committed = false;
    if (kPressureScenario == kPressureByteExtend &&
        runtime->byte_extend_terminal_order == 2U) {
        runtime->byte_extend_terminal_order = 3U; /* HORIZON_COMMIT / RUN_CLOSED */
        runtime->byte_extend_sink_bound_horizon = 1U;
        ++runtime->byte_extend_horizon_commits;
    }
    notify_worker(runtime);
    if (reset) {
        const uint32_t ordinal = ++runtime->reset_ordinal;
        if (kPressureScenario == kPressureResetAck)
            pressure_capture_before(runtime);
        while (!failed(runtime) && np2audio86_runtime_reset_ack(&runtime->control) < ordinal) {
            runtime->producer_waiting.store(1U, std::memory_order_release);
            (void)p4_nano_audio86_notifications::wait_producer();
            runtime->producer_waiting.store(0U, std::memory_order_release);
        }
        if (kPressureScenario == kPressureResetAck) {
            pressure_capture_after(runtime);
            runtime->pressure_resume_count.store(1U, std::memory_order_release);
            runtime->pressure_phase.store(5U, std::memory_order_release); /* PRODUCER_WOKE */
        }
    }
}

const np2audio86_guest_sink_t kSink{
    &s_runtime, reserve_checked, extend_checked, commit_event, commit_pcm_byte,
    commit_data_run, commit_horizon};

bool render_until(Runtime *runtime, const uint64_t target_frame)
{
    if (target_frame < runtime->rendered_frame || target_frame > kRenderFrames) return false;
    while (runtime->rendered_frame < target_frame) {
        const size_t frames = static_cast<size_t>(
            (target_frame - runtime->rendered_frame) > NP2_AUDIO86_QUANTUM_FRAMES
                ? NP2_AUDIO86_QUANTUM_FRAMES
                : target_frame - runtime->rendered_frame);
        struct np2opngen_pcm_stats stats{};
        std::memset(runtime->mix, 0, sizeof(runtime->mix));
        std::memset(&runtime->render_result, 0, sizeof(runtime->render_result));
        if (np2audio86_render_span(&runtime->render, runtime->mix, frames,
                                   &runtime->render_result) != 0 ||
            np2opngen_pcm_canonicalize_s16le(runtime->mix, frames, 2U,
                                             runtime->canonical, frames * 4U,
                                             &stats) != 0) return false;
        const size_t offset = static_cast<size_t>(runtime->rendered_frame) * 4U;
        std::memcpy(runtime->full_pcm + offset, runtime->canonical, frames * 4U);
        if (!runtime->reset_seen)
            std::memcpy(runtime->pre_reset_pcm + offset, runtime->canonical, frames * 4U);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        if (!append_pcm(runtime, runtime->canonical, frames, runtime->rendered_frame))
            return false;
#endif
        runtime->rendered_frame += frames;
    }
    return true;
}

bool apply_event(Runtime *runtime, const np2audio86_event *event)
{
    if (event == nullptr || runtime->applied_count.load(std::memory_order_relaxed) >= 32U)
        return false;
    if (!render_until(runtime, event->frame_timestamp)) return false;
    uint32_t opcode = event->opcode;
    uint32_t action = 0U;
    uint32_t action_payload = event->payload;
    uint64_t byte_offset = 0U;
    uint32_t byte_count = 0U;
    const uint8_t *data = nullptr;
    if (event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
        if (event->payload == 0U || event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            np2audio86_byte_ring_pop(&runtime->bytes, runtime->worker_run,
                                     event->payload) != NP2_AUDIO86_TRANSPORT_OK)
            return false;
        opcode = NP2AUDIO86_TRACE_PCM;
        action = NP2_AUDIO86_GUEST_ACTION_DATA_RUN;
        action_payload = 0U;
        byte_offset = runtime->worker_byte_offset;
        byte_count = event->payload;
        data = runtime->worker_run;
        runtime->worker_byte_offset += event->payload;
    } else if (event->opcode == kEventOpnaRegister) {
        opcode = NP2AUDIO86_TRACE_OPNA_REGISTER;
        action = NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER;
    } else if (event->opcode == kEventOpnaCsm) {
        opcode = NP2AUDIO86_TRACE_OPNA_CSM;
        action = NP2_AUDIO86_GUEST_ACTION_OPNA_CSM;
    } else if (event->opcode == kEventPcmControl) {
        opcode = NP2AUDIO86_TRACE_PCM_CONTROL;
        action = NP2_AUDIO86_GUEST_ACTION_PCM_CONTROL;
    } else if (event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER) {
        opcode = NP2AUDIO86_TRACE_RESET_BARRIER;
        action = NP2_AUDIO86_GUEST_ACTION_RESET;
        if (runtime->reset_seen) return false;
        runtime->pre_reset_frame = event->frame_timestamp;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        runtime->reset_ring_owned_frames =
            static_cast<uint32_t>(runtime->pcm_ring.next_frame_offset);
        if (runtime->reset_ring_owned_frames < event->frame_timestamp) return false;
        runtime->reset_applied_after_ring = 1U;
#endif
        runtime->reset_seen = true;
    } else return false;
    const np2audio86_guest_action guest_action{event->frame_timestamp, event->sequence,
                                                opcode, action_payload, byte_offset,
                                                byte_count, static_cast<uint8_t>(action)};
    const int result = np2audio86_guest_action_apply(&runtime->render, &guest_action, data,
                                                      byte_count, runtime->source,
                                                      sizeof(runtime->source));
    if (result != 0) return false;
    const uint32_t apply_index = runtime->applied_count.fetch_add(1U, std::memory_order_relaxed);
    runtime->applied[apply_index] = {event->frame_timestamp, event->sequence, opcode, action,
                                     byte_offset, byte_count, action_payload};
    if (event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER) {
        if (kPressureScenario == kPressureResetAck) {
            runtime->reset_ack_held.store(1U, std::memory_order_release);
            runtime->pressure_phase.store(3U, std::memory_order_release);
            notify_worker(runtime);
        } else {
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
            runtime->reset_ack_after_ring = runtime->reset_applied_after_ring;
#endif
            np2audio86_runtime_reset_ack_publish(&runtime->control, runtime->reset_ordinal + 1U);
            notify_producer(runtime);
        }
    }
    return np2audio86_event_ring_consume(&runtime->events) == NP2_AUDIO86_TRANSPORT_OK;
}

void worker_task(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr ||
        np2audio86_guest_action_prime_worker(&runtime->render, runtime->source,
                                             sizeof(runtime->source)) != 0) {
        if (runtime != nullptr) fail(runtime, kErrorWorker);
        vTaskDelete(nullptr); return;
    }
    runtime->worker_ready.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->ready);
    for (;;) {
        /* The Core 0 worker is the smallest profile-only controller: it never
         * creates guest records; it releases only a controller-owned lease
         * after the real Core 1 producer has entered its indexed wait. */
        if (runtime->producer_waiting.load(std::memory_order_acquire) != 0U) {
            if (kFailureKind != kFailureNone &&
                runtime->pressure_phase.load(std::memory_order_acquire) == 3U) {
                /* This is the C2 rendezvous: producer has published its real
                 * indexed wait and the normal lease release has not occurred. */
                publish_failure(runtime);
            } else if (runtime->reset_ack_held.exchange(0U, std::memory_order_acq_rel) != 0U) {
                np2audio86_runtime_reset_ack_publish(&runtime->control, runtime->reset_ordinal + 1U);
                runtime->pressure_ack_published.store(1U, std::memory_order_release);
                runtime->pressure_released.store(1U, std::memory_order_release);
                runtime->pressure_phase.store(4U, std::memory_order_release);
                notify_producer(runtime);
            } else if (runtime->pressure_phase.load(std::memory_order_acquire) == 3U &&
                       !(kPressureScenario == kPressureByteExtend &&
                         kFailureKind == kFailureNone &&
                         runtime->byte_extend_stale_notifications_injected.load(
                             std::memory_order_acquire) != 0U &&
                         runtime->byte_extend_stale_notifications_consumed.load(
                             std::memory_order_acquire) == 0U)) {
                if (kPressureScenario == kPressureEvent) {
                    /* Terminal slot 0 must not release audio slot 1. */
                    (void)xTaskNotifyGiveIndexed(runtime->producer, 0U);
                    if (runtime->producer_waiting.load(std::memory_order_acquire) != 0U)
                        runtime->pressure_index0_isolated.store(1U, std::memory_order_release);
                }
                runtime->event_lease.store(0U, std::memory_order_release);
                runtime->byte_lease.store(0U, std::memory_order_release);
                runtime->horizon_lease.store(0U, std::memory_order_release);
                runtime->pressure_released.store(1U, std::memory_order_release);
                runtime->pressure_phase.store(4U, std::memory_order_release); /* RELEASED */
                notify_producer(runtime);
            }
        }
        const np2audio86_event *event = nullptr;
        const int horizon = np2audio86_runtime_horizon_try_observe(
            &runtime->control, &runtime->consumer_clock);
        const int peek = np2audio86_event_ring_peek(&runtime->events, &event);
        if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_OK) notify_producer(runtime);
        if (peek == NP2_AUDIO86_TRANSPORT_OK && event != nullptr &&
            event->frame_timestamp <= runtime->consumer_clock.committed_frame_reconstructed) {
            if (!apply_event(runtime, event)) { fail(runtime, kErrorEventApply); break; }
            notify_producer(runtime);
            continue;
        }
        if (runtime->producer_done.load(std::memory_order_acquire) != 0U &&
            np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
            np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
            !np2audio86_runtime_horizon_pending(&runtime->control)) {
            if (!failed(runtime) && !render_until(runtime, kRenderFrames))
                fail(runtime, kErrorFinalRender);
            break;
        }
        /* A failure stops further producer authorization but does not let the
         * worker abandon an already-authorized event, byte run, or horizon. */
        if (failed(runtime) && np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
            np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
            !np2audio86_runtime_horizon_pending(&runtime->control)) break;
        (void)p4_nano_audio86_notifications::wait_worker();
    }
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    if (!finish_pcm(runtime)) fail(runtime, kErrorFinalRender);
#endif
    runtime->worker_quiescent.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->done);
    vTaskSuspend(nullptr);
}

bool execute_real_i286(Runtime *runtime)
{
    /* The real profile deliberately uses the frozen 86R.2 clock tuple.  The
     * enclosing Runtime has already established its lifecycle; this only
     * selects the guest board configuration consumed by the canonical i286
     * fixture and makes its evidence comparable to the frozen oracle. */
    pccore.baseclock = 2457600U;
    pccore.multiple = 20U;
    pccore.realclock = 49152000U;
    pccore.sound = SOUNDID_PC_9801_86;
    pccore.cpumode = CPUMODE_8MHZ;
    np2audio86_guest_host_set_clock(pccore.baseclock, pccore.multiple);
    np2audio86_guest_host_set_cpumode(pccore.cpumode);
    np2audio86_guest_host_set_cpu_position_fn(producer_position);
    np2audio86_guest_host_set_timer_hooks(timer_schedule, timer_cancel, timer_iswork, timer_irq);
    runtime->trace = {runtime->trace_events, 64U, 0U, runtime->trace_runs, 8U, 0U,
                      runtime->trace_bytes, sizeof(runtime->trace_bytes), 0U,
                      runtime->trace_timers, 64U, 0U, runtime->trace_io, 128U, 0U, 0U};
    /* Establish board86 while no transport is bound.  The initial board
     * reset configures the guest-domain device but must never become a
     * runtime RESET record; canonical sequence zero starts only below. */
    np2cfg.snd86opt = 0xd1U;
    iocore_create();
    if (iocore_build() != SUCCESS) return false;
    nevent_allreset();
    pic_reset(&np2cfg);
    board86_reset(&np2cfg, FALSE);
    board86_bind();
    np2audio86_guest_host_test_seed(0U, 0U);
    np2audio86_guest_host_trace_attach(&runtime->trace);
    np2audio86_guest_sink_bind(&kSink);
    const size_t program_size = np2audio86_guest_program_build(mem, 0x90000U);
    if (program_size != 4971U) return false;
    i286c_initialize();
    i286c_reset();
    i286core.s.r.w.cs = 0U; i286core.s.cs_base = 0U;
    i286core.s.r.w.ds = 0U; i286core.s.ds_base = 0U;
    i286core.s.r.w.ss = 0U; i286core.s.ss_base = 0U;
    i286core.s.r.w.ip = 0U; i286core.s.r.w.flag = I_FLAG;
    i286core.s.adrsmask = 0xfffffU;
    nevent_get1stevent();
    while (!failed(runtime)) {
        if (mem[i286core.s.r.w.ip] == 0xf4U) break;
        if (i286core.s.remainclock <= 0) { nevent_progress(); continue; }
        i286c_step();
        if (CPU_CLOCK > 100000000U) return false;
    }
    if (failed(runtime)) return false;
    board86_reset(&np2cfg, FALSE);
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    np2audio86_guest_host_snapshot(&runtime->final_state);
    return !np2audio86_guest_host_failed();
}

bool wait_task_suspended(TaskHandle_t task)
{
    const TickType_t start = xTaskGetTickCount();
    while (task != nullptr && eTaskGetState(task) != eSuspended) {
        if (xTaskGetTickCount() - start >= kTimeout) return false;
        taskYIELD();
    }
    return task != nullptr;
}

void put_le32(uint8_t *out, const uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
    out[2] = static_cast<uint8_t>(value >> 16U);
    out[3] = static_cast<uint8_t>(value >> 24U);
}

void put_le64(uint8_t *out, const uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i)
        out[i] = static_cast<uint8_t>(value >> (i * 8U));
}

void print_digest(const char *name, const uint8_t *bytes, const size_t length)
{
    uint8_t digest[NP2_SHA256_DIGEST_SIZE]{};
    np2_sha256_context context{};
    np2_sha256_init(&context);
    np2_sha256_update(&context, bytes, length);
    np2_sha256_final(&context, digest);
    std::printf("%s_SERIALIZED_BYTES=%zu\n%s_CRC32=%08" PRIx32 "\n%s_SHA256=",
                name, length, name, np2_crc32_iso_hdlc(bytes, length), name);
    for (const uint8_t byte : digest) std::printf("%02x", byte);
    std::printf("\n");
}

size_t serialize_apply(const Runtime *runtime, uint8_t *out)
{
    const uint32_t count = runtime->applied_count.load(std::memory_order_acquire);
    for (uint32_t i = 0U; i < count; ++i) {
        const ApplyRecord &record = runtime->applied[i];
        uint8_t *const at = out + static_cast<size_t>(i) * kApplyRecordBytes;
        put_le64(at, record.frame);
        put_le64(at + 8U, record.sequence);
        put_le32(at + 16U, record.opcode);
        put_le32(at + 20U, record.action);
        put_le64(at + 24U, record.byte_offset);
        put_le32(at + 32U, record.byte_count);
        put_le32(at + 36U, record.payload);
    }
    return static_cast<size_t>(count) * kApplyRecordBytes;
}

void emit_exact_evidence(const Runtime *runtime)
{
    uint8_t serialized[2048]{};
    size_t bytes = np2audio86_guest_evidence_serialize_io(&runtime->trace, serialized);
    print_digest("GUEST_IO", serialized, bytes);
    std::printf("GUEST_IO_RECORDS=%zu\n", runtime->trace.io_count);
    bytes = np2audio86_guest_evidence_serialize_events(&runtime->trace, serialized);
    print_digest("AUDIO_EVENTS", serialized, bytes);
    std::printf("AUDIO_EVENTS_RECORDS=%zu\n", runtime->trace.event_count);
    print_digest("PCM86_BYTES", runtime->trace.pcm_bytes, runtime->trace.pcm_count);
    std::printf("PCM86_BYTES_PAYLOAD_BYTES=%zu\n", runtime->trace.pcm_count);
    bytes = np2audio86_guest_evidence_serialize_runs(&runtime->trace, serialized);
    print_digest("PCM86_DATA_RUNS", serialized, bytes);
    std::printf("PCM86_DATA_RUNS_RECORDS=%zu\nPCM86_DATA_RUNS_PAYLOAD_BYTES=%zu\n",
                runtime->trace.data_run_count, runtime->trace.pcm_count);
    bytes = np2audio86_guest_evidence_serialize_timers(&runtime->trace, serialized);
    print_digest("TIMER_PIC", serialized, bytes);
    std::printf("TIMER_PIC_RECORDS=%zu\n", runtime->trace.timer_count);
    bytes = np2audio86_guest_evidence_serialize_state(&runtime->final_state, serialized);
    print_digest("FINAL_G_STATE", serialized, bytes);
    std::printf("FINAL_G_STATE_RECORDS=1\n");
    bytes = serialize_apply(runtime, serialized);
    print_digest("WORKER_APPLY_TRACE", serialized, bytes);
    std::printf("WORKER_APPLY_TRACE_RECORDS=%" PRIu32 "\n", runtime->applied_count.load());
    for (uint32_t i = 0U; i < runtime->applied_count.load(); ++i) {
        const ApplyRecord &record = runtime->applied[i];
        std::printf("P4_AUDIO86_ACTION sequence=%" PRIu64 " frame=%" PRIu64
                    " opcode=%" PRIu32 " action=%" PRIu32
                    " byte_offset=%" PRIu64 " byte_count=%" PRIu32
                    " payload=%" PRIu32 "\n",
                    record.sequence, record.frame, record.opcode, record.action,
                    record.byte_offset, record.byte_count, record.payload);
    }
    print_digest("PRE_RESET_PCM", runtime->pre_reset_pcm,
                 static_cast<size_t>(runtime->pre_reset_frame) * 4U);
    std::printf("PRE_RESET_PCM_FRAMES=%" PRIu64 "\n", runtime->pre_reset_frame);
    print_digest("FULL_PCM", runtime->full_pcm, sizeof(runtime->full_pcm));
    std::printf("FULL_PCM_FRAMES=%zu\n", kRenderFrames);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    print_digest("RING_PRE_RESET_PCM", runtime->ring_pcm, 13U * 4U);
    std::printf("RING_PRE_RESET_PCM_FRAMES=13\n");
    print_digest("RING_FULL_PCM", runtime->ring_pcm, sizeof(runtime->ring_pcm));
    std::printf("RING_FULL_PCM_FRAMES=%zu\n", kRenderFrames);
    std::printf("P4_AUDIO86_PCM_OUTPUT profile=1 producer_core=0 producer_priority=6"
                " consumer_core=0 consumer_priority=7 consumer_index=0"
                " prefill=4 ring_capacity=8 ring_quantum=240 ring_bytes=%zu"
                " consumer_stack=%" PRIu32 " internal=1 psram_fallback=NO"
                " i2s_active=0 physical_timing_validated=0\n",
                sizeof(runtime->pcm_ring), kPcmConsumerStackBytes);
    std::printf("P4_AUDIO86_PCM_RESET rendered=13 ring_owned=%" PRIu32
                " applied_after_ring=%" PRIu32 " ack_after_ring=%" PRIu32
                " forced_publish=0 first_slot_valid=%zu\n",
                runtime->reset_ring_owned_frames,
                runtime->reset_applied_after_ring, runtime->reset_ack_after_ring,
                kRenderFrames < NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES
                    ? kRenderFrames : NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
    std::printf("P4_AUDIO86_PCM_COMPLETION ring_finished=%" PRIu32
                " pcm_done=%" PRIu32 " worker_quiescent=%" PRIu32
                " consumer_ack=%" PRIu32 " consumer_quiescent=%" PRIu32
                " worker_suspended=%" PRIu32 " consumer_suspended=%" PRIu32
                " worker_deleted_after_suspended=%" PRIu32
                " consumer_deleted_after_suspended=%" PRIu32
                " worker_join_timeout=%" PRIu32 " consumer_join_timeout=%" PRIu32
                " sink_started=%" PRIu32 " sink_finished=%" PRIu32
                " ring_before_done=%" PRIu32 " eos_after_done=%" PRIu32
                " finish_after_empty=%" PRIu32 " ack_after_finish=%" PRIu32 "\n",
                runtime->pcm_ring_finished.load(), runtime->pcm_production_done.load(),
                runtime->worker_quiescent.load(), runtime->pcm_consumer_terminal_ack.load(),
                runtime->pcm_consumer_quiescent.load(),
                runtime->pcm_worker_suspended_observed.load(),
                runtime->pcm_consumer_suspended_observed.load(),
                runtime->pcm_worker_deleted_after_suspended.load(),
                runtime->pcm_consumer_deleted_after_suspended.load(),
                runtime->pcm_worker_join_timeout, runtime->pcm_join_timeout,
                runtime->pcm_sink_started, runtime->pcm_sink_finished,
                runtime->pcm_ring_before_done,
                runtime->pcm_eos_after_done, runtime->pcm_finish_after_empty,
                runtime->pcm_ack_after_finish);
    std::printf("P4_AUDIO86_PCM_RESIDUAL occupancy=%" PRIu32
                " partial=%u produced_frames=%" PRIu64
                " consumed_frames=%" PRIu64 " produced_bytes=%" PRIu64
                " consumed_bytes=%" PRIu64 " produced_slots=%" PRIu32
                " consumed_slots=%" PRIu32 " partial_slots=%" PRIu32
                " drops=%" PRIu32 " overwrite=%" PRIu32
                " sequence_errors=0 offset_errors=0 forced_abort=%" PRIu32
                " abandoned_published=%" PRIu64 " abandoned_partial=%" PRIu64
                " abandoned_rendered=%" PRIu64
                " first_submit_occupancy=%" PRIu32 "\n",
                np2opngen_pcm_ring_occupancy(&runtime->pcm_ring),
                np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring),
                runtime->pcm_produced_frames, runtime->pcm_controller.accepted_frames,
                runtime->pcm_produced_bytes, runtime->pcm_controller.accepted_bytes,
                runtime->pcm_produced_slots, runtime->pcm_consumed_slots,
                runtime->pcm_partial_slots, runtime->pcm_drops,
                runtime->pcm_overwrites, runtime->pcm_forced_abort,
                runtime->pcm_abandoned_published_frames,
                runtime->pcm_abandoned_partial_frames,
                runtime->pcm_abandoned_rendered_frames,
                runtime->pcm_first_submit_occupancy);
    std::printf("P4_AUDIO86_PCM_DIRECT_RING_EQUAL=%u\n",
                std::memcmp(runtime->full_pcm, runtime->ring_pcm,
                            sizeof(runtime->full_pcm)) == 0 ? 1U : 0U);
    for (uint32_t slot = 0U; slot < runtime->pcm_consumed_slots; ++slot) {
        std::printf("P4_AUDIO86_PCM_SLOT sequence=%" PRIu32
                    " frame_offset=%" PRIu64 " valid_frames=%u flags=%u"
                    " crc32=%08" PRIx32 "\n",
                    runtime->pcm_slot_sequences[slot],
                    runtime->pcm_slot_offsets[slot], runtime->pcm_slot_frames[slot],
                    runtime->pcm_slot_flags[slot], runtime->pcm_slot_crc32[slot]);
    }
    std::printf("P4_AUDIO86_PCM_OUTPUT_RESULT=PASS\n");
#endif
    std::printf("P4_AUDIO86_ACTION_ORDER=CANONICAL_19\n");
    std::printf("PCM86_NOT_EXERCISED_REQUIRES_SUPPLEMENTAL_EXISTING_86H_EVIDENCE\n");
    std::printf("REAL_P4_AUDIO_TIMING=NOT_VALIDATED\n");
}

void emit_summary(const Runtime *runtime, const bool ok)
{
    std::printf("P4_AUDIO86_REAL_GUEST profile=1 producer=p4_nano_pc98 producer_core=1 producer_priority=3 terminal_index=0 worker_core=0 worker_priority=6 producer_index=1 worker_index=0\n");
    std::printf("P4_AUDIO86_REAL_GUEST_FIXTURE bytes=4971 crc32=544b2e8c\n");
    std::printf("P4_AUDIO86_REAL_GUEST_RESIDUAL events=%" PRIu32 " bytes=%" PRIu32 " horizon=%u first_error=%" PRIu32 " pcm_fifo=%" PRId32 "\n",
                np2audio86_event_ring_occupancy(&runtime->events),
                np2audio86_byte_ring_occupancy(&runtime->bytes),
                np2audio86_runtime_horizon_pending(&runtime->control) ? 1U : 0U,
                runtime->first_error.load(std::memory_order_acquire),
                runtime->render.pcm86.pcm.realbuf);
    std::printf("P4_AUDIO86_REAL_GUEST_MEMORY psram_fallback=NO free_internal=%" PRIu32 " largest_internal=%" PRIu32 "\n",
                static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    bool exact_evidence = ok && kFailureKind == kFailureNone;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    exact_evidence = exact_evidence && kPcmLifecycleScenario == kPcmLifecycleNone;
#endif
    if (exact_evidence) emit_exact_evidence(runtime);
    if (kPressureScenario != kPressureNone && kFailureKind == kFailureNone) {
        const char *const name = kPressureScenario == kPressureEvent ? "EVENT" :
            kPressureScenario == kPressureByte ? "BYTE" :
            kPressureScenario == kPressureHorizon ? "HORIZON" :
            kPressureScenario == kPressureByteExtend ? "BYTE_EXTEND" : "RESET_ACK";
        const char *const cause = kPressureScenario == kPressureEvent ? "EVENT_CAPACITY_ONLY" :
            kPressureScenario == kPressureByte ? "BYTE_CAPACITY_ONLY" :
            kPressureScenario == kPressureHorizon ? "HORIZON_ONLY" :
            kPressureScenario == kPressureByteExtend ? "BYTE_CAPACITY_ONLY" : "POSTCOMMIT_ACK";
        const char *const target = kPressureScenario == kPressureEvent ? "EVENT_SEQUENCE_0" :
            kPressureScenario == kPressureByte ? "DATA_RUN_SEQUENCE_16" :
            kPressureScenario == kPressureHorizon ? "HORIZON_EVENT_SEQUENCE_0" :
            kPressureScenario == kPressureByteExtend ? "DATA_RUN_SEQUENCE_16_BYTE_2" :
            "RESET_ORDINAL_1";
        std::printf("P4_AUDIO86_PRESSURE scenario=%s target=%s cause=%s producer=p4_nano_pc98 core=1 priority=3 wait_index=1 phase=%" PRIu32 " state=COMPLETE\n",
                    name, target, cause, runtime->pressure_phase.load());
        std::printf("P4_AUDIO86_PRESSURE_WAIT ip_before=%" PRIu32 " ip_after=%" PRIu32
                    " pos_before=%" PRIu32 " pos_after=%" PRIu32
                    " snapshot_before=%08" PRIx32 " snapshot_after=%08" PRIx32
                    " resumes=%" PRIu32 "\n", runtime->pressure_ip_before,
                    runtime->pressure_ip_after, runtime->pressure_position_before,
                    runtime->pressure_position_after, runtime->pressure_snapshot_before,
                    runtime->pressure_snapshot_after, runtime->pressure_resume_count.load());
        std::printf("P4_AUDIO86_PRESSURE_LEASES events=%" PRIu32 " bytes=%" PRIu32
                    " horizon=%" PRIu32 " reset_ack=%" PRIu32 "\n",
                    runtime->event_lease.load(), runtime->byte_lease.load(),
                    runtime->horizon_lease.load(), runtime->reset_ack_held.load());
        std::printf("P4_AUDIO86_PRESSURE_RELEASE released=%" PRIu32
                    " index0_isolated=%" PRIu32 " ack_published=%" PRIu32 "\n",
                    runtime->pressure_released.load(), runtime->pressure_index0_isolated.load(),
                    runtime->pressure_ack_published.load());
        std::printf("P4_AUDIO86_PRESSURE_RESULT=%s\n", ok ? "PASS" : "FAIL");
    }
    if (kFailureKind != kFailureNone) {
        const char *const kind = kFailureKind == kFailureStop ? "STOP" : "FATAL";
        const char *const wait = kPressureScenario == kPressureEvent ? "EVENT" :
            kPressureScenario == kPressureByte ? "BYTE" :
            kPressureScenario == kPressureHorizon ? "HORIZON" :
            kPressureScenario == kPressureByteExtend ? "BYTE_EXTEND" : "RESET_ACK";
        const np2runtime::State state = runtime->lifecycle_runtime == nullptr
            ? np2runtime::State::Created : runtime->lifecycle_runtime->state();
        const char *const final_state = state == np2runtime::State::Stopped ? "Stopped" :
            state == np2runtime::State::Failed ? "Failed" : "NOT_TERMINAL";
        std::printf("P4_AUDIO86_FAILURE kind=%s wait=%s reason=%" PRIu32
                    " producer_waiting=%" PRIu32 " predicate_published=%" PRIu32
                    " producer_wake_index=%" PRIu32 " worker_wake_index=%" PRIu32
                    " order=%" PRIu32 " lifecycle=%s first_error=%" PRIu32
                    " later_guest_instructions=%" PRIu32 "\n",
                    kind, wait, kFailureKind == kFailureFatal ? kErrorInjectedFatal : 0U,
                    runtime->failure_wait_confirmed.load(),
                    runtime->failure_predicate_published.load(),
                    static_cast<uint32_t>(runtime->failure_producer_wake.load()),
                    static_cast<uint32_t>(runtime->failure_worker_wake.load() ? 0U : 1U),
                    runtime->failure_sequence.load(), final_state,
                    runtime->first_error.load(),
                    runtime->failure_later_guest_instructions.load());
        std::printf("P4_AUDIO86_FAILURE_CLEANUP worker_quiescent=%" PRIu32
                    " leases_events=%" PRIu32 " leases_bytes=%" PRIu32
                    " leases_horizon=%" PRIu32 " reset_ack=%" PRIu32
                    " events=%" PRIu32 " bytes=%" PRIu32 " horizon=%u"
                    " reset_closed=%" PRIu32 " first_error_after_cleanup=%" PRIu32 "\n",
                    runtime->worker_quiescent.load(), runtime->event_lease.load(),
                    runtime->byte_lease.load(), runtime->horizon_lease.load(),
                    runtime->reset_ack_held.load(),
                    np2audio86_event_ring_occupancy(&runtime->events),
                    np2audio86_byte_ring_occupancy(&runtime->bytes),
                    np2audio86_runtime_horizon_pending(&runtime->control) ? 1U : 0U,
                    runtime->failure_reset_closed.load(),
                    runtime->failure_first_error_after_cleanup.load());
        std::printf("P4_AUDIO86_FAILURE_RESULT=%s\n", ok ? "PASS" : "FAIL");
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        std::printf("P4_AUDIO86_PCM_FAILURE ring_finished=%" PRIu32
                    " pcm_done=%" PRIu32 " occupancy=%" PRIu32
                    " partial=%u produced_frames=%" PRIu64
                    " consumed_frames=%" PRIu64 " consumer_ack=%" PRIu32
                    " consumer_quiescent=%" PRIu32 " sink_finished=%" PRIu32
                    " forced_abort=%" PRIu32 " worker_suspended=%" PRIu32
                    " consumer_suspended=%" PRIu32
                    " worker_deleted_after_suspended=%" PRIu32
                    " consumer_deleted_after_suspended=%" PRIu32
                    " worker_join_timeout=%" PRIu32
                    " consumer_join_timeout=%" PRIu32
                    " abandoned_published=%" PRIu64
                    " abandoned_partial=%" PRIu64
                    " abandoned_rendered=%" PRIu64 "\n",
                    runtime->pcm_ring_finished.load(),
                    runtime->pcm_production_done.load(),
                    np2opngen_pcm_ring_occupancy(&runtime->pcm_ring),
                    np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring),
                    runtime->pcm_produced_frames,
                    runtime->pcm_controller.accepted_frames,
                    runtime->pcm_consumer_terminal_ack.load(),
                    runtime->pcm_consumer_quiescent.load(),
                    runtime->pcm_sink_finished, runtime->pcm_forced_abort,
                    runtime->pcm_worker_suspended_observed.load(),
                    runtime->pcm_consumer_suspended_observed.load(),
                    runtime->pcm_worker_deleted_after_suspended.load(),
                    runtime->pcm_consumer_deleted_after_suspended.load(),
                    runtime->pcm_worker_join_timeout, runtime->pcm_join_timeout,
                    runtime->pcm_abandoned_published_frames,
                    runtime->pcm_abandoned_partial_frames,
                    runtime->pcm_abandoned_rendered_frames);
#endif
    }
    if (kPressureScenario == kPressureByteExtend) {
        std::printf("P4_AUDIO86_BYTE_EXTEND_WAIT pending_run=%" PRIu32
                    " run_bytes=%" PRIu32 " first_byte=%02" PRIx32
                    " transport_bytes=%" PRIu32 " descriptor_owned=%" PRIu32
                    " horizon_owned=%" PRIu32
                    " rejected_ordinal=2 rejected_byte=20 second_authorized=0"
                    " second_mutated=0 second_appended=0 wait_index=1\n",
                    runtime->byte_extend_pending_at_wait,
                    runtime->byte_extend_run_bytes_at_wait,
                    runtime->byte_extend_first_byte,
                    runtime->byte_extend_transport_bytes_at_wait,
                    runtime->byte_extend_descriptor_owned_at_wait,
                    runtime->byte_extend_horizon_owned_at_wait);
        if (kFailureKind == kFailureNone) {
            std::printf("P4_AUDIO86_BYTE_EXTEND_STALE_WAKE notifications=%" PRIu32
                        " consumed=%" PRIu32 " wake_returns=%" PRIu32
                        " phase_advance=%" PRIu32 " guest_progress=%" PRIu32
                        " second_authorized=0\n",
                        runtime->byte_extend_stale_notifications_injected.load(),
                        runtime->byte_extend_stale_notifications_consumed.load(),
                        runtime->byte_extend_stale_wake_returns.load(),
                        runtime->byte_extend_stale_phase_advances.load(),
                        runtime->byte_extend_stale_guest_progress.load());
            std::printf("P4_AUDIO86_BYTE_EXTEND_RELEASE signalled=%" PRIu32
                        " observed=%" PRIu32 " lease=%" PRIu32
                        " second_authorized=%" PRIu32
                        " second_mutated=%" PRIu32 " second_appended=%" PRIu32 "\n",
                        runtime->pressure_released.load(),
                        runtime->byte_extend_release_observed.load(),
                        runtime->byte_lease.load(),
                        runtime->byte_extend_second_authorized.load(),
                        runtime->byte_extend_second_committed.load(),
                        runtime->byte_extend_second_committed.load());
        }
        if (kFailureKind != kFailureNone) {
            const bool rejected_absent = runtime->trace.pcm_count == 1U &&
                runtime->trace.pcm_bytes[0] == 0x10U;
            std::printf("P4_AUDIO86_BYTE_EXTEND_TERMINAL order=%" PRIu32
                        " semantic_handler_flush=%u sink_bound_run=%" PRIu32
                        " sink_bound_horizon=%" PRIu32
                        " reserve_calls=%" PRIu32 " extend_calls=%" PRIu32
                        " control_rechecks=%" PRIu32 " run_commits=%" PRIu32
                        " horizon_commits=%" PRIu32 " run_count=%" PRIu32
                        " run_byte=%02" PRIx32 " run_frame=%" PRIu64
                        " run_sequence=%" PRIu64 " run_offset=%" PRIu64
                        " rejected_absent=%u cleanup_after_close=%" PRIu32
                        " producer_done_after_close=%" PRIu32
                        " transaction_active=%u join_timeout=%u\n",
                        runtime->byte_extend_terminal_order,
                        runtime->byte_extend_terminal_order >= 3U ? 1U : 0U,
                        runtime->byte_extend_sink_bound_run,
                        runtime->byte_extend_sink_bound_horizon,
                        runtime->byte_extend_terminal_reserve_calls,
                        runtime->byte_extend_terminal_extend_calls,
                        runtime->byte_extend_terminal_control_rechecks,
                        runtime->byte_extend_run_commits,
                        runtime->byte_extend_horizon_commits,
                        runtime->byte_extend_run_count,
                        runtime->byte_extend_run_byte,
                        runtime->byte_extend_run_frame,
                        runtime->byte_extend_run_sequence,
                        runtime->byte_extend_run_offset,
                        rejected_absent ? 1U : 0U,
                        runtime->byte_extend_cleanup_after_close,
                        runtime->byte_extend_done_after_close,
                        runtime->transaction_active ? 1U : 0U,
                        runtime->worker_quiescent.load() ? 0U : 1U);
            std::printf("P4_AUDIO86_BYTE_EXTEND_RESULT=%s\n", ok ? "PASS" : "FAIL");
        }
    }
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    const char *const pcm_scenario =
        kPcmLifecycleScenario == kPcmLifecycleStopFull ? "STOP_FULL" :
        kPcmLifecycleScenario == kPcmLifecycleFatalFull ? "FATAL_FULL" :
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull
            ? "CONSUMER_FAILURE_FULL" :
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureEmpty
            ? "CONSUMER_FAILURE_EMPTY" :
        kPcmLifecycleScenario == kPcmLifecycleRetryStop ? "RETRY_STOP" :
        kPcmLifecycleScenario == kPcmLifecycleRetryFatal ? "RETRY_FATAL" :
        kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst
            ? "RETRY_PRIMARY_FIRST" :
        kPcmLifecycleScenario == kPcmLifecycleRetryConsumerFirst
            ? "RETRY_CONSUMER_FIRST" : "NONE";
    std::printf("P4_AUDIO86_PCM_LIFECYCLE scenario=%s triggered=%" PRIu32
                " forced_abort=%" PRIu32 " forced_before_wake=%" PRIu32
                " ring_finished=%" PRIu32 " pcm_done=%" PRIu32
                " worker_quiescent=%" PRIu32 " consumer_ack=%" PRIu32
                " consumer_quiescent=%" PRIu32 " worker_suspended=%" PRIu32
                " consumer_suspended=%" PRIu32
                " worker_deleted_after_suspended=%" PRIu32
                " consumer_deleted_after_suspended=%" PRIu32
                " worker_join_timeout=%" PRIu32 " consumer_join_timeout=%" PRIu32
                " sink_abort_calls=%" PRIu32 " worker_waiting=%" PRIu32
                " pre_cleanup_occupancy=%" PRIu32 " pre_cleanup_partial=%u"
                " final_occupancy=%" PRIu32 " final_partial=%u"
                " produced_frames=%" PRIu64 " consumed_frames=%" PRIu64
                " abandoned_published=%" PRIu64 " abandoned_partial=%" PRIu64
                " abandoned_rendered=%" PRIu64 " first_error=%" PRIu32
                " result=%s\n",
                pcm_scenario, runtime->pcm_lifecycle_triggered.load(),
                runtime->pcm_forced_abort,
                runtime->pcm_forced_abort_published_before_wake.load(),
                runtime->pcm_ring_finished.load(), runtime->pcm_production_done.load(),
                runtime->worker_quiescent.load(),
                runtime->pcm_consumer_terminal_ack.load(),
                runtime->pcm_consumer_quiescent.load(),
                runtime->pcm_worker_suspended_observed.load(),
                runtime->pcm_consumer_suspended_observed.load(),
                runtime->pcm_worker_deleted_after_suspended.load(),
                runtime->pcm_consumer_deleted_after_suspended.load(),
                runtime->pcm_worker_join_timeout, runtime->pcm_join_timeout,
                runtime->pcm_sink_abort_calls,
                runtime->pcm_worker_space_waiting.load(),
                runtime->pcm_abort_pre_cleanup_occupancy,
                runtime->pcm_abort_pre_cleanup_partial,
                np2opngen_pcm_ring_occupancy(&runtime->pcm_ring),
                np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring),
                runtime->pcm_produced_frames,
                runtime->pcm_controller.accepted_frames,
                runtime->pcm_abandoned_published_frames,
                runtime->pcm_abandoned_partial_frames,
                runtime->pcm_abandoned_rendered_frames,
                runtime->first_error.load(), ok ? "PASS" : "FAIL");
    if (kPcmRetryLifecycle) {
        std::printf("P4_AUDIO86_PCM_RETRY scenario=%s attempts=%" PRIu32
                    " wakes=%" PRIu32 " resubmits=%" PRIu32
                    " identity=%" PRIu32 " tail_held=%" PRIu32
                    " accepted_held=%" PRIu32
                    " full_occupancy=%" PRIu32
                    " worker_resumed=%" PRIu32 " permission_before_wake=%" PRIu32
                    " wait_skipped_ready=%" PRIu32
                    " done_only_after_empty=%" PRIu32
                    " tail_before=%" PRIu32 " tail_after=%" PRIu32
                    " accepted_frames_before=%" PRIu64
                    " accepted_frames_after=%" PRIu64
                    " accepted_bytes_before=%" PRIu64
                    " accepted_bytes_after=%" PRIu64
                    " sequence=%" PRIu32 " frame_offset=%" PRIu64
                    " valid_frames=%u flags=%u crc32=%08" PRIx32
                    " forced_abort=%" PRIu32 " first_error=%" PRIu32
                    " result=%s\n",
                    pcm_scenario, runtime->pcm_retry_attempts,
                    runtime->pcm_retry_wakes, runtime->pcm_retry_resubmits,
                    runtime->pcm_retry_identity,
                    runtime->pcm_retry_tail_held,
                    runtime->pcm_retry_accepted_held,
                    runtime->pcm_retry_full_occupancy,
                    runtime->pcm_retry_worker_resumed,
                    runtime->pcm_retry_permission_before_wake.load(),
                    runtime->pcm_retry_wait_skipped_ready,
                    runtime->pcm_retry_done_only_after_empty,
                    runtime->pcm_retry_tail_before, runtime->pcm_retry_tail_after,
                    runtime->pcm_retry_accepted_frames_before,
                    runtime->pcm_controller.accepted_frames,
                    runtime->pcm_retry_accepted_bytes_before,
                    runtime->pcm_controller.accepted_bytes,
                    runtime->pcm_retry_sequence, runtime->pcm_retry_frame_offset,
                    runtime->pcm_retry_valid_frames, runtime->pcm_retry_flags,
                    runtime->pcm_retry_crc32, runtime->pcm_forced_abort,
                    runtime->first_error.load(), ok ? "PASS" : "FAIL");
        if (kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
            kPcmLifecycleScenario == kPcmLifecycleRetryFatal)
            std::printf("P4_AUDIO86_PCM_POST_DONE_RETRY scenario=%s"
                        " attempts=%" PRIu32 " resubmits=%" PRIu32
                        " identity=%" PRIu32 " tail_held=%" PRIu32
                        " accepted_held=%" PRIu32 " observed_occupancy=%" PRIu32
                        " not_eos=%" PRIu32 " permission_before_wake=%" PRIu32
                        " tail_before=%" PRIu32 " tail_after=%" PRIu32
                        " accepted_frames_before=%" PRIu64
                        " accepted_bytes_before=%" PRIu64
                        " crc32=%08" PRIx32 " result=%s\n",
                        pcm_scenario, runtime->pcm_post_done_retry_attempts,
                        runtime->pcm_post_done_retry_resubmits,
                        runtime->pcm_post_done_retry_identity,
                        runtime->pcm_post_done_retry_tail_held,
                        runtime->pcm_post_done_retry_accepted_held,
                        runtime->pcm_post_done_retry_observed,
                        runtime->pcm_post_done_retry_not_eos,
                        runtime->pcm_post_done_permission_before_wake.load(),
                        runtime->pcm_post_done_tail_before,
                        runtime->pcm_post_done_tail_after,
                        runtime->pcm_post_done_accepted_frames_before,
                        runtime->pcm_post_done_accepted_bytes_before,
                        runtime->pcm_post_done_retry_crc32,
                        ok ? "PASS" : "FAIL");
    }
#endif
    std::printf("P4_AUDIO86_REAL_GUEST_RESULT=%s\n", ok ? "PASS" : "FAIL");
}

} // namespace

esp_err_t run_on_pc98_task(TaskHandle_t producer,
                           np2runtime::Runtime *lifecycle_runtime) noexcept
{
    Runtime *const runtime = &s_runtime;
    /* This one-shot profile owns static storage for its complete lifetime.
     * Do not bulk-reset atomics: all cross-core state is initialized through
     * its public transport operations below. */
    runtime->producer = producer;
    runtime->lifecycle_runtime = lifecycle_runtime;
    runtime->event_lease.store(0U, std::memory_order_relaxed);
    runtime->byte_lease.store(0U, std::memory_order_relaxed);
    runtime->horizon_lease.store(0U, std::memory_order_relaxed);
    runtime->reset_ack_held.store(0U, std::memory_order_relaxed);
    runtime->pressure_resume_count.store(0U, std::memory_order_relaxed);
    runtime->pressure_index0_isolated.store(0U, std::memory_order_relaxed);
    runtime->pressure_released.store(0U, std::memory_order_relaxed);
    runtime->pressure_ack_published.store(0U, std::memory_order_relaxed);
    runtime->failure_injected.store(0U, std::memory_order_relaxed);
    runtime->failure_wait_confirmed.store(0U, std::memory_order_relaxed);
    runtime->failure_predicate_published.store(0U, std::memory_order_relaxed);
    runtime->failure_producer_wake.store(0U, std::memory_order_relaxed);
    runtime->failure_worker_wake.store(0U, std::memory_order_relaxed);
    runtime->failure_sequence.store(0U, std::memory_order_relaxed);
    runtime->failure_later_guest_instructions.store(0U, std::memory_order_relaxed);
    runtime->failure_reset_closed.store(0U, std::memory_order_relaxed);
    runtime->failure_first_error_after_cleanup.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_pending_at_wait = 0U;
    runtime->byte_extend_run_bytes_at_wait = 0U;
    runtime->byte_extend_first_byte = 0U;
    runtime->byte_extend_transport_bytes_at_wait = 0U;
    runtime->byte_extend_descriptor_owned_at_wait = 0U;
    runtime->byte_extend_horizon_owned_at_wait = 0U;
    runtime->byte_extend_terminal_order = 0U;
    runtime->byte_extend_terminal_reserve_calls = 0U;
    runtime->byte_extend_terminal_extend_calls = 0U;
    runtime->byte_extend_terminal_control_rechecks = 0U;
    runtime->byte_extend_run_commits = 0U;
    runtime->byte_extend_horizon_commits = 0U;
    runtime->byte_extend_sink_bound_run = 0U;
    runtime->byte_extend_sink_bound_horizon = 0U;
    runtime->byte_extend_run_count = 0U;
    runtime->byte_extend_run_byte = 0U;
    runtime->byte_extend_run_frame = 0U;
    runtime->byte_extend_run_sequence = 0U;
    runtime->byte_extend_run_offset = 0U;
    runtime->byte_extend_cleanup_after_close = 0U;
    runtime->byte_extend_done_after_close = 0U;
    runtime->byte_extend_stale_notifications_injected.store(0U,
                                                             std::memory_order_relaxed);
    runtime->byte_extend_stale_notifications_consumed.store(0U,
                                                             std::memory_order_relaxed);
    runtime->byte_extend_stale_wake_returns.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_stale_phase_advances.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_stale_guest_progress.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_release_observed.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_second_authorized.store(0U, std::memory_order_relaxed);
    runtime->byte_extend_second_committed.store(0U, std::memory_order_relaxed);
    runtime->pressure_ip_before = runtime->pressure_ip_after = 0U;
    runtime->pressure_position_before = runtime->pressure_position_after = 0U;
    runtime->pressure_snapshot_before = runtime->pressure_snapshot_after = 0U;
    runtime->pressure_phase.store(kPressureScenario == kPressureNone ? 0U : 1U,
                                  std::memory_order_release); /* ARMED */
    runtime->generation++;
    np2audio86_event_ring_init(&runtime->events);
    np2audio86_byte_ring_init(&runtime->bytes);
    np2audio86_runtime_control_init(&runtime->control);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    np2opngen_pcm_ring_init(&runtime->pcm_ring);
    std::memset(runtime->ring_pcm, 0, sizeof(runtime->ring_pcm));
    runtime->pcm_consumer_ready.store(0U, std::memory_order_relaxed);
    runtime->pcm_production_done.store(0U, std::memory_order_relaxed);
    runtime->pcm_consumer_quiescent.store(0U, std::memory_order_relaxed);
    runtime->pcm_consumer_terminal_ack.store(0U, std::memory_order_relaxed);
    runtime->pcm_ring_finished.store(0U, std::memory_order_relaxed);
    runtime->pcm_forced_abort_requested.store(0U, std::memory_order_relaxed);
    runtime->pcm_worker_space_waiting.store(0U, std::memory_order_relaxed);
    runtime->pcm_lifecycle_triggered.store(0U, std::memory_order_relaxed);
    runtime->pcm_forced_abort_published_before_wake.store(0U,
                                                          std::memory_order_relaxed);
    runtime->pcm_worker_suspended_observed.store(0U, std::memory_order_relaxed);
    runtime->pcm_consumer_suspended_observed.store(0U, std::memory_order_relaxed);
    runtime->pcm_worker_deleted_after_suspended.store(0U, std::memory_order_relaxed);
    runtime->pcm_consumer_deleted_after_suspended.store(0U,
                                                        std::memory_order_relaxed);
    runtime->pcm_sink_permission.store(
        kPcmRetryLifecycle ? kPcmSinkPermissionHold : kPcmSinkPermissionAccept,
        std::memory_order_relaxed);
    runtime->pcm_retry_waiting.store(0U, std::memory_order_relaxed);
    runtime->pcm_retry_controller_driven.store(0U, std::memory_order_relaxed);
    runtime->pcm_retry_permission_before_wake.store(0U,
                                                    std::memory_order_relaxed);
    runtime->pcm_post_done_retry_waiting.store(0U,
                                               std::memory_order_relaxed);
    runtime->pcm_post_done_permission_before_wake.store(
        0U, std::memory_order_relaxed);
    runtime->pcm_retry_slot_captured = 0U;
    runtime->pcm_post_done_retry_slot_captured = 0U;
    runtime->pcm_retry_attempts = 0U;
    runtime->pcm_retry_wakes = 0U;
    runtime->pcm_retry_resubmits = 0U;
    runtime->pcm_retry_identity = 1U;
    runtime->pcm_retry_tail_held = 0U;
    runtime->pcm_retry_accepted_held = 0U;
    runtime->pcm_retry_full_occupancy = 0U;
    runtime->pcm_retry_worker_resumed = 0U;
    runtime->pcm_retry_wait_skipped_ready = 0U;
    runtime->pcm_retry_done_only_after_empty = 0U;
    runtime->pcm_retry_tail_before = 0U;
    runtime->pcm_retry_tail_after = 0U;
    runtime->pcm_retry_accepted_frames_before = 0U;
    runtime->pcm_retry_accepted_bytes_before = 0U;
    runtime->pcm_retry_frame_offset = 0U;
    runtime->pcm_retry_sequence = 0U;
    runtime->pcm_retry_valid_frames = 0U;
    runtime->pcm_retry_flags = 0U;
    runtime->pcm_retry_crc32 = 0U;
    std::memset(runtime->pcm_retry_pcm, 0, sizeof(runtime->pcm_retry_pcm));
    runtime->pcm_post_done_retry_attempts = 0U;
    runtime->pcm_post_done_retry_resubmits = 0U;
    runtime->pcm_post_done_retry_identity = 1U;
    runtime->pcm_post_done_retry_tail_held = 0U;
    runtime->pcm_post_done_retry_accepted_held = 0U;
    runtime->pcm_post_done_retry_observed = 0U;
    runtime->pcm_post_done_retry_not_eos = 0U;
    runtime->pcm_post_done_tail_before = 0U;
    runtime->pcm_post_done_tail_after = 0U;
    runtime->pcm_post_done_accepted_frames_before = 0U;
    runtime->pcm_post_done_accepted_bytes_before = 0U;
    runtime->pcm_post_done_retry_crc32 = 0U;
    runtime->pcm_produced_frames = 0U;
    runtime->pcm_produced_bytes = 0U;
    runtime->pcm_produced_slots = 0U;
    runtime->pcm_consumed_slots = 0U;
    runtime->pcm_partial_slots = 0U;
    runtime->pcm_drops = 0U;
    runtime->pcm_overwrites = 0U;
    runtime->reset_ring_owned_frames = 0U;
    runtime->reset_applied_after_ring = 0U;
    runtime->reset_ack_after_ring = 0U;
    runtime->pcm_first_submit_occupancy = 0U;
    runtime->pcm_sink_started = 0U;
    runtime->pcm_sink_finished = 0U;
    runtime->pcm_forced_abort = 0U;
    runtime->pcm_join_timeout = 0U;
    runtime->pcm_worker_join_timeout = 0U;
    runtime->pcm_sink_abort_calls = 0U;
    runtime->pcm_abandoned_published_frames = 0U;
    runtime->pcm_abandoned_partial_frames = 0U;
    runtime->pcm_abandoned_rendered_frames = 0U;
    runtime->pcm_abort_pre_cleanup_occupancy = 0U;
    runtime->pcm_abort_pre_cleanup_partial = 0U;
    runtime->pcm_ring_before_done = 0U;
    runtime->pcm_eos_after_done = 0U;
    runtime->pcm_finish_after_empty = 0U;
    runtime->pcm_ack_after_finish = 0U;
    if (np2_pcm_output_controller_init(&runtime->pcm_controller,
                                       &runtime->pcm_ring, &kPcmSink) != 0)
        return ESP_FAIL;
    runtime->pcm_ready = xSemaphoreCreateBinaryStatic(&runtime->pcm_ready_storage);
    runtime->pcm_done_semaphore =
        xSemaphoreCreateBinaryStatic(&runtime->pcm_done_storage);
    if (runtime->pcm_ready == nullptr || runtime->pcm_done_semaphore == nullptr)
        return ESP_ERR_NO_MEM;
    runtime->pcm_consumer = xTaskCreateStaticPinnedToCore(
        pcm_consumer_task, "audio86_pcm_out",
        kPcmConsumerStackBytes / sizeof(StackType_t), runtime,
        kPcmConsumerPriority, runtime->pcm_consumer_stack,
        &runtime->pcm_consumer_tcb, kPcmConsumerCore);
    if (runtime->pcm_consumer == nullptr ||
        xSemaphoreTake(runtime->pcm_ready, kTimeout) != pdTRUE ||
        runtime->pcm_consumer_ready.load(std::memory_order_acquire) == 0U) {
        fail(runtime, kErrorWorker);
        emit_summary(runtime, false);
        return ESP_FAIL;
    }
#endif
    runtime->ready = xSemaphoreCreateBinaryStatic(&runtime->ready_storage);
    runtime->done = xSemaphoreCreateBinaryStatic(&runtime->done_storage);
    if (runtime->ready == nullptr || runtime->done == nullptr) return ESP_ERR_NO_MEM;
    runtime->worker = xTaskCreateStaticPinnedToCore(worker_task, "audio86_worker",
        kWorkerStackBytes / sizeof(StackType_t), runtime, kWorkerPriority,
        runtime->worker_stack, &runtime->worker_tcb, kWorkerCore);
    if (runtime->worker == nullptr || xSemaphoreTake(runtime->ready, kTimeout) != pdTRUE ||
        runtime->worker_ready.load(std::memory_order_acquire) == 0U) {
        fail(runtime, kErrorWorker); emit_summary(runtime, false); return ESP_FAIL;
    }
    const bool guest_ok = execute_real_i286(runtime);
    if (kPressureScenario == kPressureByteExtend && kFailureKind != kFailureNone &&
        runtime->byte_extend_terminal_order == 3U)
        runtime->byte_extend_terminal_order = 4U; /* GUEST_EXECUTION_EXIT */
    if (kPressureScenario == kPressureByteExtend && kFailureKind != kFailureNone)
        runtime->byte_extend_cleanup_after_close =
            runtime->byte_extend_terminal_order >= 4U ? 1U : 0U;
    np2audio86_guest_sink_unbind();
    np2audio86_guest_host_trace_detach();
    board86_unbind();
    if (kPressureScenario == kPressureByteExtend && kFailureKind != kFailureNone &&
        runtime->byte_extend_terminal_order == 4U)
        runtime->byte_extend_terminal_order = 5U; /* CLEANUP_COMPLETE */
    if (kPressureScenario == kPressureByteExtend && kFailureKind != kFailureNone)
        runtime->byte_extend_done_after_close =
            runtime->byte_extend_terminal_order >= 5U ? 1U : 0U;
    runtime->producer_done.store(1U, std::memory_order_release);
    np2audio86_runtime_producer_done_publish(&runtime->control);
    notify_worker(runtime);
    const bool worker_terminal = xSemaphoreTake(runtime->done, kTimeout) == pdTRUE;
    const bool worker_quiescent =
        runtime->worker_quiescent.load(std::memory_order_acquire) != 0U;
    const bool worker_suspended = worker_terminal && worker_quiescent &&
        wait_task_suspended(runtime->worker);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    runtime->pcm_worker_suspended_observed.store(worker_suspended ? 1U : 0U,
                                                  std::memory_order_release);
    runtime->pcm_worker_join_timeout = worker_suspended ? 0U : 1U;
#endif
    const bool joined = worker_terminal && worker_quiescent && worker_suspended;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    const bool pcm_terminal =
        xSemaphoreTake(runtime->pcm_done_semaphore, kTimeout) == pdTRUE;
    const bool pcm_quiescent =
        runtime->pcm_consumer_quiescent.load(std::memory_order_acquire) != 0U;
    const bool pcm_ack =
        runtime->pcm_consumer_terminal_ack.load(std::memory_order_acquire) != 0U;
    const bool pcm_suspended = pcm_terminal && pcm_quiescent && pcm_ack &&
        wait_task_suspended(runtime->pcm_consumer);
    runtime->pcm_consumer_suspended_observed.store(pcm_suspended ? 1U : 0U,
                                                    std::memory_order_release);
    runtime->pcm_join_timeout = pcm_suspended ? 0U : 1U;
    const bool pcm_joined = pcm_terminal && pcm_quiescent && pcm_ack && pcm_suspended;

    if (runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U &&
        joined && pcm_joined) {
        runtime->pcm_abort_pre_cleanup_occupancy =
            np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
        runtime->pcm_abort_pre_cleanup_partial =
            np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring);
        const uint64_t abandoned =
            runtime->pcm_produced_frames >= runtime->pcm_controller.accepted_frames
                ? runtime->pcm_produced_frames -
                      runtime->pcm_controller.accepted_frames
                : 0U;
        runtime->pcm_abandoned_partial_frames =
            runtime->pcm_abort_pre_cleanup_partial;
        runtime->pcm_abandoned_published_frames =
            abandoned >= runtime->pcm_abandoned_partial_frames
                ? abandoned - runtime->pcm_abandoned_partial_frames
                : 0U;
        np2opngen_pcm_ring_init(&runtime->pcm_ring);
    }
#else
    const bool pcm_joined = true;
#endif

    if (joined && runtime->worker != nullptr) {
        vTaskDelete(runtime->worker);
        runtime->worker = nullptr;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        runtime->pcm_worker_deleted_after_suspended.store(1U,
                                                           std::memory_order_release);
#endif
    }
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    if (pcm_joined && runtime->pcm_consumer != nullptr) {
        vTaskDelete(runtime->pcm_consumer);
        runtime->pcm_consumer = nullptr;
        runtime->pcm_consumer_deleted_after_suspended.store(1U,
                                                             std::memory_order_release);
    }
#endif
    const bool pressure_ok = kPressureScenario == kPressureNone ||
        (runtime->pressure_phase.load(std::memory_order_acquire) == 5U &&
         runtime->pressure_resume_count.load(std::memory_order_acquire) == 1U &&
         runtime->pressure_released.load(std::memory_order_acquire) != 0U &&
         runtime->event_lease.load(std::memory_order_acquire) == 0U &&
         runtime->byte_lease.load(std::memory_order_acquire) == 0U &&
         runtime->horizon_lease.load(std::memory_order_acquire) == 0U &&
         runtime->reset_ack_held.load(std::memory_order_acquire) == 0U &&
         runtime->pressure_ip_before == runtime->pressure_ip_after &&
         runtime->pressure_position_before == runtime->pressure_position_after &&
         runtime->pressure_snapshot_before == runtime->pressure_snapshot_after &&
         (kPressureScenario != kPressureEvent ||
          runtime->pressure_index0_isolated.load(std::memory_order_acquire) != 0U) &&
         (kPressureScenario != kPressureResetAck ||
          runtime->pressure_ack_published.load(std::memory_order_acquire) != 0U));
    const bool normal_ok = guest_ok && joined && pcm_joined && pressure_ok && !failed(runtime) &&
                    np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
                    np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
                    !np2audio86_runtime_horizon_pending(&runtime->control)
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                    && runtime->pcm_ring_finished.load(std::memory_order_acquire) != 0U
                    && np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) == 0U
                    && np2opngen_pcm_ring_producer_partial_valid_frames(
                           &runtime->pcm_ring) == 0U
                    && runtime->pcm_controller.accepted_frames == kRenderFrames
                    && runtime->pcm_controller.accepted_bytes == kRenderFrames * 4U
                    && runtime->pcm_produced_frames == kRenderFrames
                    && runtime->pcm_produced_bytes == kRenderFrames * 4U
                    && runtime->pcm_produced_slots == kExpectedPcmSlots
                    && runtime->pcm_consumed_slots == kExpectedPcmSlots
                    && runtime->pcm_partial_slots == kExpectedPartialSlots
                    && runtime->pcm_drops == 0U
                    && runtime->pcm_overwrites == 0U
                    && runtime->pcm_forced_abort == 0U
                    && runtime->pcm_forced_abort_requested.load(
                           std::memory_order_acquire) == 0U
                    && runtime->pcm_join_timeout == 0U
                    && runtime->pcm_worker_join_timeout == 0U
                    && runtime->pcm_consumer_suspended_observed.load(
                           std::memory_order_acquire) == 1U
                    && runtime->pcm_worker_suspended_observed.load(
                           std::memory_order_acquire) == 1U
                    && runtime->pcm_consumer_deleted_after_suspended.load(
                           std::memory_order_acquire) == 1U
                    && runtime->pcm_worker_deleted_after_suspended.load(
                           std::memory_order_acquire) == 1U
                    && runtime->pcm_abandoned_published_frames == 0U
                    && runtime->pcm_abandoned_partial_frames == 0U
                    && runtime->pcm_abandoned_rendered_frames == 0U
                    && runtime->pcm_sink_started == 1U
                    && runtime->pcm_sink_finished == 1U
                    && runtime->pcm_ring_before_done == 1U
                    && runtime->pcm_eos_after_done == 1U
                    && runtime->pcm_finish_after_empty == 1U
                    && runtime->pcm_ack_after_finish == 1U
                    && runtime->pcm_first_submit_occupancy >=
                       (kRenderFrames < NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES
                            ? 1U : kPcmPrefillSlots)
                    && runtime->reset_ring_owned_frames >= 13U
                    && runtime->reset_applied_after_ring == 1U
                    && runtime->reset_ack_after_ring == 1U
                    && std::memcmp(runtime->full_pcm, runtime->ring_pcm,
                                   sizeof(runtime->full_pcm)) == 0
#endif
                    ;
    if ((kFailureKind != kFailureNone
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
         || runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U
#endif
         ) && lifecycle_runtime != nullptr)
        (void)lifecycle_runtime->run();
    runtime->failure_first_error_after_cleanup.store(
        runtime->first_error.load(std::memory_order_acquire), std::memory_order_release);
    const bool failure_ok = kFailureKind != kFailureNone && joined && pcm_joined &&
        runtime->failure_injected.load(std::memory_order_acquire) != 0U &&
        runtime->failure_wait_confirmed.load(std::memory_order_acquire) != 0U &&
        runtime->failure_predicate_published.load(std::memory_order_acquire) != 0U &&
        runtime->failure_producer_wake.load(std::memory_order_acquire) != 0U &&
        runtime->failure_worker_wake.load(std::memory_order_acquire) != 0U &&
        runtime->failure_sequence.load(std::memory_order_acquire) == 3U &&
        runtime->failure_later_guest_instructions.load(std::memory_order_acquire) == 0U &&
        runtime->event_lease.load(std::memory_order_acquire) == 0U &&
        runtime->byte_lease.load(std::memory_order_acquire) == 0U &&
        runtime->horizon_lease.load(std::memory_order_acquire) == 0U &&
        runtime->reset_ack_held.load(std::memory_order_acquire) == 0U &&
        np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
        np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
        !np2audio86_runtime_horizon_pending(&runtime->control) &&
        runtime->failure_reset_closed.load(std::memory_order_acquire) != 0U &&
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        runtime->pcm_consumer_suspended_observed.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_worker_suspended_observed.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_consumer_deleted_after_suspended.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_worker_deleted_after_suspended.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_join_timeout == 0U && runtime->pcm_worker_join_timeout == 0U &&
        runtime->pcm_consumer_terminal_ack.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_ring_finished.load(std::memory_order_acquire) == 1U &&
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) == 0U &&
        np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring) == 0U &&
        runtime->pcm_produced_frames == runtime->pcm_controller.accepted_frames &&
        runtime->pcm_produced_bytes == runtime->pcm_controller.accepted_bytes &&
        runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) == 0U &&
        runtime->pcm_forced_abort == 0U && runtime->pcm_sink_finished == 1U &&
        runtime->pcm_abandoned_published_frames == 0U &&
        runtime->pcm_abandoned_partial_frames == 0U &&
        runtime->pcm_abandoned_rendered_frames == 0U &&
#endif
        ((kFailureKind == kFailureStop &&
          lifecycle_runtime->state() == np2runtime::State::Stopped &&
          runtime->first_error.load(std::memory_order_acquire) == 0U) ||
         (kFailureKind == kFailureFatal &&
          lifecycle_runtime->state() == np2runtime::State::Failed &&
          runtime->first_error.load(std::memory_order_acquire) == kErrorInjectedFatal &&
          runtime->failure_first_error_after_cleanup.load(std::memory_order_acquire) == kErrorInjectedFatal));
    const bool byte_extend_failure_ok = kPressureScenario != kPressureByteExtend ||
        (runtime->byte_extend_pending_at_wait == 1U &&
         runtime->byte_extend_run_bytes_at_wait == 1U &&
         runtime->byte_extend_first_byte == 0x10U &&
         runtime->byte_extend_transport_bytes_at_wait == 1U &&
         runtime->byte_extend_descriptor_owned_at_wait == 1U &&
         runtime->byte_extend_horizon_owned_at_wait == 1U &&
         runtime->byte_extend_terminal_order == 5U &&
         runtime->byte_extend_terminal_reserve_calls == 0U &&
         runtime->byte_extend_terminal_extend_calls == 0U &&
         runtime->byte_extend_terminal_control_rechecks == 0U &&
         runtime->byte_extend_run_commits == 1U &&
         runtime->byte_extend_horizon_commits == 1U &&
         runtime->byte_extend_sink_bound_run == 1U &&
         runtime->byte_extend_sink_bound_horizon == 1U &&
         runtime->byte_extend_run_count == 1U &&
         runtime->byte_extend_run_byte == 0x10U &&
         runtime->byte_extend_run_frame == 0U &&
         runtime->byte_extend_run_sequence == 16U &&
         runtime->byte_extend_run_offset == 0U &&
         runtime->trace.pcm_count == 1U &&
         runtime->byte_extend_cleanup_after_close == 1U &&
         runtime->byte_extend_done_after_close == 1U &&
         !runtime->transaction_active);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    const bool retry_common_ok = !kPcmRetryLifecycle ||
        (runtime->pcm_lifecycle_triggered.load(std::memory_order_acquire) == 1U &&
         runtime->pcm_retry_identity == 1U &&
         runtime->pcm_retry_tail_held == 1U &&
         runtime->pcm_retry_accepted_held == 1U &&
         runtime->pcm_retry_full_occupancy == NP2_OPNGEN_PCM_RING_CAPACITY &&
         runtime->pcm_retry_permission_before_wake.load(
             std::memory_order_acquire) == 1U);
    const bool retry_healthy_ok = !kPcmRetryLifecycle ||
        ((kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
          kPcmLifecycleScenario == kPcmLifecycleRetryFatal) &&
         retry_common_ok && runtime->pcm_retry_worker_resumed == 1U &&
         runtime->pcm_retry_done_only_after_empty == 1U &&
         runtime->pcm_retry_tail_after ==
             (runtime->pcm_retry_tail_before + 1U) %
                 NP2_OPNGEN_PCM_RING_CAPACITY &&
         runtime->pcm_post_done_retry_identity == 1U &&
         runtime->pcm_post_done_retry_tail_held == 1U &&
         runtime->pcm_post_done_retry_accepted_held == 1U &&
         runtime->pcm_post_done_retry_observed != 0U &&
         runtime->pcm_post_done_retry_not_eos == 1U &&
         runtime->pcm_post_done_permission_before_wake.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_post_done_tail_after ==
             runtime->pcm_post_done_tail_before + 1U);
    const bool retry_dual_control_ok =
        (kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst ||
         kPcmLifecycleScenario == kPcmLifecycleRetryConsumerFirst) &&
        retry_common_ok &&
        runtime->failure_injected.load(std::memory_order_acquire) == 1U &&
        runtime->failure_wait_confirmed.load(std::memory_order_acquire) == 1U &&
        runtime->failure_predicate_published.load(std::memory_order_acquire) == 1U &&
        runtime->failure_producer_wake.load(std::memory_order_acquire) == 1U &&
        runtime->failure_worker_wake.load(std::memory_order_acquire) == 1U &&
        runtime->failure_sequence.load(std::memory_order_acquire) == 3U &&
        runtime->failure_first_error_after_cleanup.load(
            std::memory_order_acquire) ==
            (kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst
                 ? kErrorInjectedFatal : kErrorWorker) &&
        runtime->pcm_retry_tail_after == runtime->pcm_retry_tail_before &&
        runtime->pcm_retry_worker_resumed == 0U;
    const bool forced_abort_ok =
        (kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull ||
         kPcmLifecycleScenario == kPcmLifecycleConsumerFailureEmpty ||
         kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst ||
         kPcmLifecycleScenario == kPcmLifecycleRetryConsumerFirst) &&
        joined && pcm_joined &&
        runtime->first_error.load(std::memory_order_acquire) ==
            (kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst
                 ? kErrorInjectedFatal : kErrorWorker) &&
        lifecycle_runtime != nullptr &&
        lifecycle_runtime->state() == np2runtime::State::Failed &&
        runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_forced_abort_published_before_wake.load(
            std::memory_order_acquire) == 1U &&
        runtime->pcm_forced_abort == 1U && runtime->pcm_sink_abort_calls == 1U &&
        runtime->pcm_consumer_terminal_ack.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_consumer_suspended_observed.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_worker_suspended_observed.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_consumer_deleted_after_suspended.load(
            std::memory_order_acquire) == 1U &&
        runtime->pcm_worker_deleted_after_suspended.load(
            std::memory_order_acquire) == 1U &&
        runtime->pcm_join_timeout == 0U && runtime->pcm_worker_join_timeout == 0U &&
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) == 0U &&
        np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring) == 0U &&
        runtime->pcm_produced_frames == runtime->pcm_controller.accepted_frames +
            runtime->pcm_abandoned_published_frames +
            runtime->pcm_abandoned_partial_frames &&
        (!kPcmRetryLifecycle || retry_dual_control_ok);
    const bool pcm_lifecycle_failure =
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull ||
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureEmpty ||
        kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst ||
        kPcmLifecycleScenario == kPcmLifecycleRetryConsumerFirst;
#else
    const bool retry_healthy_ok = true;
    const bool forced_abort_ok = false;
    const bool pcm_lifecycle_failure = false;
#endif
    const bool ok = pcm_lifecycle_failure ? forced_abort_ok :
        (kFailureKind == kFailureNone ? normal_ok :
         (failure_ok && byte_extend_failure_ok && retry_healthy_ok));
    if (kPressureScenario != kPressureNone && pressure_ok)
        runtime->pressure_phase.store(6U, std::memory_order_release); /* COMPLETE */
    emit_summary(runtime, ok);
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio86_guest_binding
