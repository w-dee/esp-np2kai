#include "p4_nano_audio86_guest_binding/p4_nano_audio86_guest_binding.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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
#include "np2audio86_sustained_evidence.h"
#include "np2opngen_pcm_canonical.h"
#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"
#include "np2runtime/np2runtime.hpp"
#include "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_predicate.hpp"
#include "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_worker_timing.hpp"
#include "p4_nano_audio86_notifications/task_notification.hpp"
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink_idf.hpp"
#endif

#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE) && \
    !defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE) && \
    !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE) && \
    !defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
#define P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE 1
#endif

#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE) && \
    (!defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE) || \
     !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE) || \
     !defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE))
#error "sustained physical evidence requires sustained PCM over physical I2S"
#endif
#if defined(P4_NANO_AUDIO86_TERMINAL_POST_PCM_FAILURE_TEST) && \
    !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
#error "terminal post-PCM producer failure test requires sustained profile"
#endif
#ifndef P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST
#define P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST 0
#endif
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0 && \
    !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
#error "terminal publication test requires sustained profile"
#endif

namespace p4_nano_audio86_guest_binding {
namespace {

namespace terminal_timing = p4_nano_audio86_terminal_worker_timing;

constexpr BaseType_t kWorkerCore = 0;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 6U;
constexpr uint32_t kWorkerStackBytes = 8192U;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
constexpr BaseType_t kPcmConsumerCore = 0;
constexpr UBaseType_t kPcmConsumerPriority = tskIDLE_PRIORITY + 7U;
constexpr uint32_t kPcmConsumerStackBytes = 4096U;
constexpr uint32_t kPcmPrefillSlots = 4U;
#endif
constexpr uint32_t kTimeoutMs = 5000U;
constexpr TickType_t kTimeout = pdMS_TO_TICKS(kTimeoutMs);
constexpr uint32_t kErrorTransport = 1U;
constexpr uint32_t kErrorWorker = 2U;
constexpr uint32_t kErrorGuest = 3U;
constexpr uint32_t kErrorFinalRender = 4U;
constexpr uint32_t kErrorEventApply = 5U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
constexpr uint64_t kSustainedTerminalResetFrame = 95761U;
#endif
/* The fixture's DATA_RUN value overlaps the guest adapter's PCM-control
 * trace value.  Keep the transport semantic namespaces disjoint. */
constexpr uint32_t kEventOpnaRegister = 0x100U;
constexpr uint32_t kEventOpnaCsm = 0x101U;
constexpr uint32_t kEventPcmControl = 0x102U;
#ifndef P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO
#define P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO 0
#endif
constexpr size_t kS2ResetFrameOffset = 1920U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
constexpr uint32_t kSustainedExpectedPcmCrc32 = 0x5bb15277U;
constexpr uint8_t kSustainedExpectedPcmSha256[NP2_SHA256_DIGEST_SIZE] = {
    0xb3U, 0x15U, 0xa9U, 0x47U, 0x6eU, 0x4fU, 0xc3U, 0x0cU,
    0xbbU, 0x7aU, 0xeaU, 0x0cU, 0x7aU, 0x1bU, 0xfaU, 0x9cU,
    0xd4U, 0xaaU, 0x31U, 0xa0U, 0x33U, 0xc9U, 0x22U, 0x3eU,
    0xb2U, 0x25U, 0x00U, 0x60U, 0x4fU, 0xffU, 0x62U, 0xa0U,
};
constexpr uint8_t kSustainedExpectedPreResetPcmSha256[
    NP2_SHA256_DIGEST_SIZE] = {
    0x5eU, 0xa6U, 0x10U, 0xe1U, 0xe9U, 0x3fU, 0x21U, 0x19U,
    0xf9U, 0xf2U, 0x17U, 0x5bU, 0xe5U, 0x09U, 0xa9U, 0x16U,
    0x57U, 0xaaU, 0x5dU, 0xe0U, 0x74U, 0x65U, 0x0eU, 0xe2U,
    0xb7U, 0x8fU, 0xe7U, 0x92U, 0xe7U, 0x82U, 0xc8U, 0xd8U,
};
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
#if !defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
#error "sustained evidence requires the q240 output path"
#endif
constexpr size_t kRenderFrames = NP2_AUDIO86_GUEST_SUSTAINED_2S_FRAMES;
constexpr uint32_t kExpectedPcmSlots =
    NP2_AUDIO86_GUEST_SUSTAINED_2S_QUANTA_240;
constexpr uint32_t kExpectedPartialSlots = 0U;
#elif defined(P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE)
constexpr size_t kRenderFrames = 13U;
constexpr uint32_t kExpectedPcmSlots = 1U;
constexpr uint32_t kExpectedPartialSlots = 1U;
#elif P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO >= 9 && \
      P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO <= 11
/* RESET/full profiles shift the complete guest timeline by eight q240 slots.
 * This preserves the guest ordering while making the pre-RESET durability
 * boundary physically reachable with the frozen eight-slot ring. */
constexpr size_t kRenderFrames = kS2ResetFrameOffset + 2400U;
constexpr uint32_t kExpectedPcmSlots = 18U;
constexpr uint32_t kExpectedPartialSlots = 0U;
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
                  kPcmLifecycleRetryConsumerFirst = 8U,
                  kPcmLifecycleResetStop = 9U,
                  kPcmLifecycleResetFatal = 10U,
                  kPcmLifecycleResetConsumerFatal = 11U,
                  kPcmLifecyclePartialStop = 12U,
                  kPcmLifecyclePartialFatal = 13U,
                  kPcmLifecyclePartialConsumerFatal = 14U,
                  kPcmLifecyclePostDoneConsumerFatal = 15U,
                  kPcmLifecycleFinishFatal = 16U };
constexpr uint32_t kPcmLifecycleScenario =
    P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO;
constexpr bool kPcmRetryLifecycle =
    kPcmLifecycleScenario >= kPcmLifecycleRetryStop &&
    kPcmLifecycleScenario <= kPcmLifecycleRetryConsumerFirst;
constexpr bool kPcmS2ResetLifecycle =
    kPcmLifecycleScenario >= kPcmLifecycleResetStop &&
    kPcmLifecycleScenario <= kPcmLifecycleResetConsumerFatal;
constexpr bool kPcmS2PartialLifecycle =
    kPcmLifecycleScenario >= kPcmLifecyclePartialStop &&
    kPcmLifecycleScenario <= kPcmLifecyclePartialConsumerFatal;
constexpr bool kPcmS2Lifecycle =
    kPcmLifecycleScenario >= kPcmLifecycleResetStop &&
    kPcmLifecycleScenario <= kPcmLifecycleFinishFatal;
constexpr bool kPcmPermissionLifecycle =
    kPcmRetryLifecycle || kPcmS2ResetLifecycle;
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

#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
struct PhysicalSnapshot {
    p4_nano_audio86_physical_telemetry sink{};
    uint64_t semantic_frames = 0U;
    uint64_t semantic_bytes = 0U;
    uint64_t controller_accepted_frames = 0U;
    uint64_t controller_accepted_bytes = 0U;
    uint32_t semantic_crc32 = 0U;
    uint8_t semantic_sha256[NP2_SHA256_DIGEST_SIZE]{};
    enum np2_pcm_output_state controller_state = NP2_PCM_OUTPUT_INITIAL;
    uint32_t first_error = 0U;
    uint32_t forced_abort = 0U;
    uint32_t sink_destroyed = 0U;
    uint64_t produced_frames = 0U;
    uint64_t produced_bytes = 0U;
    uint32_t produced_slots = 0U;
    uint32_t final_ring_occupancy = 0U;
    uint32_t final_ring_partial = 0U;
    uint32_t drops = 0U;
    uint32_t overwrites = 0U;
    uint64_t abandoned_published_frames = 0U;
    uint64_t abandoned_partial_frames = 0U;
    uint64_t abandoned_rendered_frames = 0U;
    bool captured = false;
};
#endif

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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    np2audio86_sustained_evidence sustained{};
    np2_pcm_sink sustained_downstream{};
#else
    uint8_t full_pcm[kRenderFrames * 4U]{};
    uint8_t pre_reset_pcm[kRenderFrames * 4U]{};
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    np2opngen_pcm_ring pcm_ring{};
    np2_pcm_output_controller pcm_controller{};
#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    uint8_t ring_pcm[kRenderFrames * 4U]{};
#endif
    StaticTask_t pcm_consumer_tcb{};
    StackType_t pcm_consumer_stack[kPcmConsumerStackBytes / sizeof(StackType_t)]{};
    StaticSemaphore_t pcm_ready_storage{};
    StaticSemaphore_t pcm_done_storage{};
    SemaphoreHandle_t pcm_ready = nullptr;
    SemaphoreHandle_t pcm_done_semaphore = nullptr;
    TaskHandle_t pcm_consumer = nullptr;
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    p4_nano_audio86_physical_sink *physical_sink = nullptr;
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    uint64_t physical_diagnostic_origin_us = 0U;
    bool physical_diagnostic_origin_valid = false;
    std::atomic<uint32_t> physical_diagnostic_rendered_frames{0U};
    std::atomic<uint32_t> physical_diagnostic_next_published_sequence{0U};
    terminal_timing::Snapshot terminal_worker_timing{};
#endif
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    PhysicalSnapshot physical{};
#endif
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
    uint32_t test_ready_wait = 0U;
    uint32_t test_terminal_wait = 0U;
    uint32_t test_quiescent_observed = 0U;
    uint32_t test_ack_observed = 0U;
    uint32_t test_suspended_observed = 0U;
    uint32_t test_delete_performed = 0U;
    uint32_t test_sink_destroy_performed = 0U;
#endif
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
    std::atomic<uint32_t> pcm_s2_controller_driven{0U};
    std::atomic<uint32_t> pcm_s2_reset_rendering{0U};
    std::atomic<uint32_t> pcm_s2_final_rendering{0U};
    std::atomic<uint32_t> pcm_s2_consumer_fault_ready{0U};
    std::atomic<uint32_t> pcm_s2_reset_guest_linearized{0U};
    std::atomic<uint32_t> pcm_s2_reset_abandoned{0U};
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
    uint64_t pcm_semantic_rendered_frames = 0U;
    uint32_t pcm_s2_cutpoint_occupancy = 0U;
    uint16_t pcm_s2_cutpoint_partial = 0U;
    uint32_t pcm_s2_cutpoint_rendered = 0U;
    uint32_t pcm_s2_cutpoint_unappended = 0U;
    uint32_t pcm_s2_reset_event_residual_before_cleanup = 0U;
    uint32_t pcm_s2_reset_horizon_residual_before_cleanup = 0U;
    uint32_t pcm_s2_reset_transport_residual_after_cleanup = 0U;
    uint32_t pcm_s2_finish_calls = 0U;
    uint32_t pcm_s2_finish_fatal_observed = 0U;
    uint32_t pcm_s2_terminal_success = 0U;
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
#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    uint64_t pcm_slot_offsets[kExpectedPcmSlots]{};
    uint32_t pcm_slot_sequences[kExpectedPcmSlots]{};
    uint16_t pcm_slot_frames[kExpectedPcmSlots]{};
    uint16_t pcm_slot_flags[kExpectedPcmSlots]{};
    uint32_t pcm_slot_crc32[kExpectedPcmSlots]{};
#endif
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    /* Producer-owned until the terminal mailbox release publication. */
    bool terminal_horizon_armed = false;
    /* Producer-owned transaction state.  The final RESET event is already
     * release-visible while this is true, but its ordinary event-side wake is
     * deferred until the matching terminal mailbox is release-visible. */
    bool terminal_reset_notify_deferred = false;
    uint32_t terminal_reset_transaction_ordinal = 0U;
    /* Fail-safe worker authority for the sole case where the RESET head is
     * visible but the matching terminal mailbox publication cannot complete. */
    std::atomic<uint32_t> terminal_reset_publication_failed_ordinal{0U};
    /* Worker-owned; read only after task join outside the worker. */
    uint32_t terminal_reset_applied_ordinal = 0U;
    std::atomic<uint32_t> terminal_horizon_published{0U};
    std::atomic<uint32_t> terminal_horizon_observed{0U};
    std::atomic<uint32_t> terminal_pcm_ready{0U};
    std::atomic<uint32_t> terminal_pcm_before_guest_done{0U};
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    std::atomic<uint32_t> terminal_test_worker_hold{0U};
    std::atomic<uint32_t> terminal_test_worker_hold_ack{0U};
    std::atomic<uint32_t> terminal_test_phase{0U};
    std::atomic<uint32_t> terminal_test_worker_notify_count{0U};
    std::atomic<uint32_t> terminal_test_q398_accepted{0U};
    std::atomic<uint32_t> terminal_test_q399_ring_visible{0U};
    std::atomic<uint32_t> terminal_test_q399_accepted{0U};
    uint32_t terminal_test_notify_before_event = 0U;
    uint32_t terminal_test_notify_after_event = 0U;
    uint32_t terminal_test_event_before_terminal = 0U;
    uint32_t terminal_test_terminal_absent_before_release = 0U;
    uint32_t terminal_test_pre_ack_state = 0U;
    uint32_t terminal_test_worker_observed_pair = 0U;
    uint32_t terminal_test_reset_before_remainder = 0U;
    uint32_t terminal_test_retained_until_pcm_done = 0U;
    uint32_t terminal_test_q399_before_producer_continuation = 0U;
    uint32_t terminal_test_partial_failure_event_visible = 0U;
    uint32_t terminal_test_partial_failure_wake_issued = 0U;
    uint32_t terminal_test_deadline_virtual_gap_ms = UINT32_MAX;
#endif
#endif
    uint64_t next_sequence = 0U;
    uint32_t reset_ordinal = 0U;
    uint64_t rendered_frame = 0U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    std::atomic<uint64_t> rendered_frame_published{0U};
#endif
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
    uint32_t reset_ack_held_ordinal = 0U;
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
static_assert(sizeof(np2audio86_runtime_control) == 36U);

void notify_producer(Runtime *runtime)
{
    if (runtime->producer != nullptr)
        (void)p4_nano_audio86_notifications::notify_producer(runtime->producer);
}

void notify_worker(Runtime *runtime)
{
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    if (runtime != nullptr)
        runtime->terminal_test_worker_notify_count.fetch_add(
            1U, std::memory_order_relaxed);
#endif
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
        runtime->pcm_consumed_slots >= kExpectedPcmSlots ||
        view->valid_frames > NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES ||
        view->frame_offset > kRenderFrames ||
        view->valid_frames > kRenderFrames - view->frame_offset ||
        view->sequence != runtime->pcm_consumed_slots)
        return NP2_PCM_SINK_FATAL;
    const size_t bytes = static_cast<size_t>(view->valid_frames) * 4U;
    const bool s2_post_done_fault =
        kPcmLifecycleScenario == kPcmLifecyclePostDoneConsumerFatal &&
        view->sequence == kExpectedPcmSlots - 1U;
    if (s2_post_done_fault &&
        runtime->pcm_post_done_retry_slot_captured == 0U)
        runtime->pcm_sink_permission.store(kPcmSinkPermissionHold,
                                           std::memory_order_release);
    const bool post_done_retry_slot = view->sequence == kExpectedPcmSlots - 1U &&
        (((kPcmLifecycleScenario == kPcmLifecycleRetryStop ||
           kPcmLifecycleScenario == kPcmLifecycleRetryFatal) &&
          runtime->pcm_retry_controller_driven.load(
              std::memory_order_acquire) != 0U) || s2_post_done_fault);
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
        const uint32_t permission =
            runtime->pcm_sink_permission.load(std::memory_order_acquire);
        if (permission == kPcmSinkPermissionHold) {
            runtime->pcm_post_done_retry_waiting.store(
                1U, std::memory_order_release);
            return NP2_PCM_SINK_RETRY;
        }
        if (permission == kPcmSinkPermissionFatal)
            return NP2_PCM_SINK_FATAL;
    }
    if (kPcmPermissionLifecycle &&
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
    if (kPcmLifecycleScenario == kPcmLifecyclePartialConsumerFatal &&
        runtime->pcm_s2_consumer_fault_ready.load(
            std::memory_order_acquire) != 0U)
        return NP2_PCM_SINK_FATAL;
    if (runtime->pcm_consumed_slots == 0U)
        runtime->pcm_first_submit_occupancy =
            np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    std::memcpy(runtime->ring_pcm + static_cast<size_t>(view->frame_offset) * 4U,
                view->pcm, bytes);
    const uint32_t slot = runtime->pcm_consumed_slots;
    runtime->pcm_slot_offsets[slot] = view->frame_offset;
    runtime->pcm_slot_sequences[slot] = view->sequence;
    runtime->pcm_slot_frames[slot] = view->valid_frames;
    runtime->pcm_slot_flags[slot] = view->flags;
    runtime->pcm_slot_crc32[slot] = np2_crc32_iso_hdlc_finish(
        np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(), view->pcm, bytes));
#endif
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

void drive_pcm_s2_reset_controller(Runtime *runtime,
                                   const size_t unappended_frames)
{
    if (!kPcmS2ResetLifecycle || unappended_frames == 0U ||
        runtime->pcm_s2_reset_rendering.load(std::memory_order_acquire) == 0U ||
        runtime->pcm_retry_waiting.load(std::memory_order_acquire) == 0U ||
        runtime->pcm_worker_space_waiting.load(std::memory_order_acquire) == 0U ||
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) !=
            NP2_OPNGEN_PCM_RING_CAPACITY ||
        runtime->pcm_s2_controller_driven.exchange(
            1U, std::memory_order_acq_rel) != 0U)
        return;
    runtime->pcm_s2_cutpoint_occupancy =
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
    runtime->pcm_s2_cutpoint_partial =
        np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring);
    runtime->pcm_s2_cutpoint_rendered = static_cast<uint32_t>(
        runtime->pcm_produced_frames + unappended_frames);
    runtime->pcm_s2_cutpoint_unappended =
        static_cast<uint32_t>(unappended_frames);
    runtime->pcm_lifecycle_triggered.store(1U, std::memory_order_release);
    if (kPcmLifecycleScenario == kPcmLifecycleResetStop ||
        kPcmLifecycleScenario == kPcmLifecycleResetFatal)
        publish_failure(runtime);
    const uint32_t permission =
        kPcmLifecycleScenario == kPcmLifecycleResetConsumerFatal
            ? kPcmSinkPermissionFatal : kPcmSinkPermissionAccept;
    runtime->pcm_sink_permission.store(permission, std::memory_order_release);
    runtime->pcm_retry_permission_before_wake.store(1U,
                                                    std::memory_order_release);
    notify_pcm_consumer(runtime);
}

void drive_pcm_s2_partial_controller(Runtime *runtime)
{
    constexpr uint32_t kPartialCutpointFrames = 253U;
    constexpr uint16_t kPartialCutpointProducerFrames = 13U;
    if (!kPcmS2PartialLifecycle ||
        runtime->pcm_s2_final_rendering.load(std::memory_order_acquire) == 0U ||
        runtime->pcm_produced_frames != kPartialCutpointFrames ||
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) != 1U ||
        np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring) !=
            kPartialCutpointProducerFrames ||
        runtime->pcm_s2_controller_driven.exchange(
            1U, std::memory_order_acq_rel) != 0U)
        return;
    runtime->pcm_s2_cutpoint_occupancy = 1U;
    runtime->pcm_s2_cutpoint_partial = kPartialCutpointProducerFrames;
    runtime->pcm_s2_cutpoint_rendered = kPartialCutpointFrames;
    runtime->pcm_s2_cutpoint_unappended = 0U;
    runtime->pcm_lifecycle_triggered.store(1U, std::memory_order_release);
    if (kPcmLifecycleScenario == kPcmLifecyclePartialStop ||
        kPcmLifecycleScenario == kPcmLifecyclePartialFatal) {
        publish_failure(runtime);
        return;
    }
    runtime->pcm_s2_consumer_fault_ready.store(1U,
                                               std::memory_order_release);
    notify_pcm_consumer(runtime);
    while (runtime->pcm_forced_abort_requested.load(
               std::memory_order_acquire) == 0U)
        (void)p4_nano_audio86_notifications::wait_worker();
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
    if (kPcmLifecycleScenario == kPcmLifecyclePostDoneConsumerFatal &&
        runtime->pcm_s2_controller_driven.exchange(
            1U, std::memory_order_acq_rel) == 0U) {
        runtime->pcm_lifecycle_triggered.store(1U, std::memory_order_release);
        runtime->pcm_s2_cutpoint_occupancy = occupancy;
        runtime->pcm_s2_cutpoint_partial =
            np2opngen_pcm_ring_producer_partial_valid_frames(
                &runtime->pcm_ring);
        runtime->pcm_s2_cutpoint_rendered =
            static_cast<uint32_t>(runtime->pcm_semantic_rendered_frames);
    }
    runtime->pcm_sink_permission.store(
        kPcmLifecycleScenario == kPcmLifecyclePostDoneConsumerFatal
            ? kPcmSinkPermissionFatal : kPcmSinkPermissionAccept,
        std::memory_order_release);
    runtime->pcm_post_done_permission_before_wake.store(
        1U, std::memory_order_release);
    notify_pcm_consumer(runtime);
}

enum np2_pcm_sink_result pcm_sink_finish(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr) return NP2_PCM_SINK_FATAL;
    ++runtime->pcm_s2_finish_calls;
    if (kPcmLifecycleScenario == kPcmLifecycleFinishFatal) {
        runtime->pcm_s2_finish_fatal_observed = 1U;
        return NP2_PCM_SINK_FATAL;
    }
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

#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
static_assert(kExpectedPcmSlots <= terminal_timing::kConsumerSequenceMask);
static_assert(static_cast<uint32_t>(terminal_timing::Phase::PcmFinish) <
              (1U << (14U - terminal_timing::kConsumerSequenceBits)));

uint64_t physical_diagnostic_now_us()
{
    const int64_t now_us = esp_timer_get_time();
    return now_us < 0 ? 0U : static_cast<uint64_t>(now_us);
}

uint32_t physical_diagnostic_elapsed_us(const uint64_t started_us,
                                        const uint64_t completed_us)
{
    if (completed_us < started_us) return 0U;
    const uint64_t elapsed = completed_us - started_us;
    return elapsed > UINT32_MAX ? UINT32_MAX
                                : static_cast<uint32_t>(elapsed);
}

uint32_t physical_diagnostic_relative_us(const Runtime *runtime,
                                         const uint64_t now_us)
{
    if (runtime == nullptr || !runtime->physical_diagnostic_origin_valid ||
        now_us < runtime->physical_diagnostic_origin_us)
        return 0U;
    const uint64_t relative = now_us - runtime->physical_diagnostic_origin_us;
    return relative > UINT32_MAX ? UINT32_MAX
                                 : static_cast<uint32_t>(relative);
}

uint32_t terminal_worker_relative_us(const Runtime *runtime)
{
    if (runtime == nullptr || !runtime->physical_diagnostic_origin_valid)
        return terminal_timing::kUnset;
    const uint64_t now_us = physical_diagnostic_now_us();
    if (now_us < runtime->physical_diagnostic_origin_us)
        return terminal_timing::kUnset;
    const uint64_t relative = now_us - runtime->physical_diagnostic_origin_us;
    return relative >= terminal_timing::kUnset
        ? terminal_timing::kUnset : static_cast<uint32_t>(relative);
}

uint32_t terminal_worker_service_sequence(const Runtime *runtime,
                                          const uint32_t consumer_sequence)
{
    const uint32_t phase = runtime->terminal_worker_timing.current_phase.load(
        std::memory_order_acquire);
    return terminal_timing::pack_service_sequence(
        consumer_sequence, static_cast<terminal_timing::Phase>(phase));
}

void publish_terminal_worker_phase(Runtime *runtime,
                                   const terminal_timing::Phase phase)
{
    terminal_timing::publish_phase(&runtime->terminal_worker_timing, phase);
    /* Both tasks are pinned to Core 0 and the consumer has the higher
     * priority.  While the worker is running, an occupied ring means the
     * consumer's last service is the retained tail; an empty ring means its
     * last service was the preceding sequence.  Reconstruct that existing
     * service context from the ring's callback-safe atomics so a worker-phase
     * publication does not replace it with producer progress. */
    const uint32_t head = runtime->pcm_ring.head.load(
        std::memory_order_acquire);
    const uint32_t tail = runtime->pcm_ring.tail.load(
        std::memory_order_acquire);
    const uint32_t current_sequence =
        head != tail || tail == 0U ? tail : tail - 1U;
    p4_nano_audio86_physical_sink_publish_consumer_progress(
        runtime->physical_sink, P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
        P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF,
        terminal_worker_service_sequence(runtime, current_sequence), tail, 0U);
}

void record_terminal_worker_point(Runtime *runtime,
                                  const terminal_timing::Point point)
{
    (void)terminal_timing::record_once(
        &runtime->terminal_worker_timing, point,
        terminal_worker_relative_us(runtime));
}

void freeze_first_qovf_worker_phase(
    Runtime *runtime, p4_nano_audio86_physical_telemetry *telemetry)
{
    if (runtime == nullptr || telemetry == nullptr ||
        telemetry->first_active_qovf_latched == 0U)
        return;
    const uint32_t packed_sequence = telemetry->first_qovf_current_sequence;
    const uint32_t phase = terminal_timing::unpack_service_phase(
        packed_sequence);
    (void)terminal_timing::freeze_first_qovf_phase(
        &runtime->terminal_worker_timing, phase);
    telemetry->first_qovf_current_sequence =
        terminal_timing::unpack_consumer_sequence(packed_sequence);
}

void publish_physical_diagnostic(
    Runtime *runtime, enum p4_nano_audio86_consumer_progress_point point,
    enum p4_nano_audio86_consumer_service_phase phase,
    const uint32_t current_sequence, const uint32_t published_sequence,
    const uint64_t now_us)
{
    p4_nano_audio86_physical_sink_publish_consumer_progress(
        runtime->physical_sink, point, phase,
        terminal_worker_service_sequence(runtime, current_sequence),
        published_sequence, physical_diagnostic_relative_us(runtime, now_us));
}

void observe_physical_qovf(Runtime *runtime, const uint64_t now_us)
{
    p4_nano_audio86_physical_sink_observe_first_qovf(
        runtime->physical_sink, physical_diagnostic_relative_us(runtime, now_us));
}

void publish_physical_ring_context(Runtime *runtime)
{
    p4_nano_audio86_physical_sink_publish_ring_context(
        runtime->physical_sink,
        runtime->physical_diagnostic_rendered_frames.load(
            std::memory_order_acquire),
        runtime->physical_diagnostic_next_published_sequence.load(
            std::memory_order_acquire),
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring),
        runtime->pcm_production_done.load(std::memory_order_acquire) != 0U);
}

void publish_physical_wait_enter(
    Runtime *runtime, const p4_nano_audio86_consumer_wait_reason reason,
    const uint32_t sequence)
{
    publish_physical_ring_context(runtime);
    p4_nano_audio86_physical_sink_publish_wait_enter(
        runtime->physical_sink, reason, sequence,
        physical_diagnostic_relative_us(runtime, physical_diagnostic_now_us()));
}

void publish_physical_wait_resume(
    Runtime *runtime, const p4_nano_audio86_consumer_wait_reason reason,
    const uint32_t sequence)
{
    p4_nano_audio86_physical_sink_publish_wait_resume(
        runtime->physical_sink, reason, sequence,
        physical_diagnostic_relative_us(runtime, physical_diagnostic_now_us()));
    publish_physical_ring_context(runtime);
}

void publish_physical_runnable(Runtime *runtime, const uint32_t sequence)
{
    p4_nano_audio86_physical_sink_publish_runnable(
        runtime->physical_sink, sequence);
}
#endif

enum np2_pcm_sink_result sustained_sink_start(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || runtime->sustained_downstream.start == nullptr)
        return NP2_PCM_SINK_FATAL;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    runtime->physical_diagnostic_origin_us = physical_diagnostic_now_us();
    runtime->physical_diagnostic_origin_valid = true;
    publish_physical_diagnostic(
        runtime, P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
        P4_NANO_AUDIO86_CONSUMER_PHASE_NONE, 0U, 0U,
        runtime->physical_diagnostic_origin_us);
#endif
    const enum np2_pcm_sink_result result =
        runtime->sustained_downstream.start(runtime->sustained_downstream.opaque);
#if !defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    if (result == NP2_PCM_SINK_ACCEPTED)
        np2audio86_sustained_stream_start(
            &runtime->sustained,
            static_cast<uint64_t>(esp_timer_get_time()) / 1000U);
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    observe_physical_qovf(runtime, physical_diagnostic_now_us());
#endif
    return result;
}

enum np2_pcm_sink_result sustained_sink_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || view == nullptr ||
        runtime->sustained_downstream.submit == nullptr)
        return NP2_PCM_SINK_FATAL;
    uint8_t running = 1U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    p4_nano_audio86_physical_telemetry before{};
    p4_nano_audio86_physical_sink_get_telemetry(runtime->physical_sink, &before);
    running = before.state == P4_NANO_AUDIO86_PHYSICAL_RUNNING ? 1U : 0U;
    const uint64_t downstream_started_us = physical_diagnostic_now_us();
    publish_physical_diagnostic(
        runtime, P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
        P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT, view->sequence,
        runtime->sustained.next_accepted_sequence, downstream_started_us);
#endif
    const enum np2_pcm_sink_result result = runtime->sustained_downstream.submit(
        runtime->sustained_downstream.opaque, view);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    const uint64_t downstream_returned_us = physical_diagnostic_now_us();
    if (running) {
        np2audio86_sustained_observe_downstream_submit(
            &runtime->sustained, view->sequence,
            physical_diagnostic_elapsed_us(downstream_started_us,
                                           downstream_returned_us));
    }
    publish_physical_diagnostic(
        runtime, P4_NANO_AUDIO86_PROGRESS_SUBMIT_RETURN,
        result == NP2_PCM_SINK_ACCEPTED
            ? P4_NANO_AUDIO86_CONSUMER_PHASE_POST_ACCEPT_EVIDENCE
            : P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF,
        view->sequence, runtime->sustained.next_accepted_sequence,
        downstream_returned_us);
    p4_nano_audio86_physical_telemetry after{};
    p4_nano_audio86_physical_sink_get_telemetry(runtime->physical_sink, &after);
    if (after.state == P4_NANO_AUDIO86_PHYSICAL_RUNNING &&
        !runtime->sustained.stream_started)
        np2audio86_sustained_stream_start(
            &runtime->sustained, downstream_returned_us / 1000U);
    observe_physical_qovf(runtime, downstream_returned_us);
#endif
    if (result == NP2_PCM_SINK_FATAL) return result;
    const enum np2audio86_sustained_submit_result evidence_result =
        result == NP2_PCM_SINK_ACCEPTED ? NP2_AUDIO86_SUSTAINED_ACCEPTED
                                       : NP2_AUDIO86_SUSTAINED_RETRY;
    const uint64_t evidence_started_us =
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        downstream_returned_us;
#else
        static_cast<uint64_t>(esp_timer_get_time());
#endif
    const int evidence_status = np2audio86_sustained_submit(
            &runtime->sustained, evidence_result, view->sequence,
            view->frame_offset, view->pcm, view->valid_frames, running,
            evidence_started_us / 1000U);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    if (evidence_status == 0 && result == NP2_PCM_SINK_ACCEPTED) {
        if (view->sequence == kExpectedPcmSlots - 2U)
            runtime->terminal_test_q398_accepted.store(
                1U, std::memory_order_release);
        if (view->sequence == kExpectedPcmSlots - 1U)
            runtime->terminal_test_q399_accepted.store(
                1U, std::memory_order_release);
    }
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    const uint64_t evidence_completed_us = physical_diagnostic_now_us();
    if (result == NP2_PCM_SINK_ACCEPTED && running) {
        np2audio86_sustained_observe_post_accept_evidence(
            &runtime->sustained, view->sequence,
            physical_diagnostic_elapsed_us(evidence_started_us,
                                           evidence_completed_us));
    }
    publish_physical_diagnostic(
        runtime,
        result == NP2_PCM_SINK_ACCEPTED && running && evidence_status == 0
            ? P4_NANO_AUDIO86_PROGRESS_RUNNING_ACCEPTED
            : P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
        P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF,
        view->sequence, runtime->sustained.next_accepted_sequence,
        evidence_completed_us);
    observe_physical_qovf(runtime, evidence_completed_us);
#endif
    if (evidence_status != 0)
        return NP2_PCM_SINK_FATAL;
    return result;
}

enum np2_pcm_sink_result sustained_sink_finish(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || runtime->sustained_downstream.finish == nullptr)
        return NP2_PCM_SINK_FATAL;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    publish_physical_diagnostic(
        runtime, P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
        P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH,
        runtime->sustained.next_accepted_sequence,
        runtime->sustained.next_accepted_sequence,
        physical_diagnostic_now_us());
#endif
    const enum np2_pcm_sink_result result = runtime->sustained_downstream.finish(
        runtime->sustained_downstream.opaque);
    if (result == NP2_PCM_SINK_ACCEPTED)
        np2audio86_sustained_drain_complete(
            &runtime->sustained,
            static_cast<uint64_t>(esp_timer_get_time()) / 1000U);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    observe_physical_qovf(runtime, physical_diagnostic_now_us());
#endif
    return result;
}

enum np2_pcm_sink_result sustained_sink_abort(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    if (runtime == nullptr || runtime->sustained_downstream.abort == nullptr)
        return NP2_PCM_SINK_FATAL;
    return runtime->sustained_downstream.abort(
        runtime->sustained_downstream.opaque);
}

const np2_pcm_sink kSustainedSink{
    &s_runtime, sustained_sink_start, sustained_sink_submit,
    sustained_sink_finish, sustained_sink_abort};
#endif

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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        runtime->physical_diagnostic_next_published_sequence.store(
            runtime->pcm_ring.next_sequence, std::memory_order_release);
        publish_physical_ring_context(runtime);
#endif
        appended += consumed;
        runtime->pcm_produced_frames += consumed;
        runtime->pcm_produced_bytes += consumed * 4U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        np2audio86_sustained_observe_ring(
            &runtime->sustained,
            np2opngen_pcm_ring_occupancy(&runtime->pcm_ring));
#endif
        notify_pcm_consumer(runtime);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        if (runtime->terminal_worker_timing.current_phase.load(
                std::memory_order_acquire) ==
                static_cast<uint32_t>(terminal_timing::Phase::Q399Publish) &&
            runtime->pcm_semantic_rendered_frames == kRenderFrames &&
            runtime->pcm_ring.next_sequence == kExpectedPcmSlots &&
            np2opngen_pcm_ring_producer_partial_valid_frames(
                &runtime->pcm_ring) == 0U) {
            record_terminal_worker_point(
                runtime, terminal_timing::Point::T9Q399Published);
            publish_terminal_worker_phase(runtime,
                                          terminal_timing::Phase::PcmFinish);
        }
#endif
        drive_pcm_s2_partial_controller(runtime);
        if (runtime->pcm_forced_abort_requested.load(
                std::memory_order_acquire) != 0U)
            return false;
        if (status == NP2_OPNGEN_PCM_RING_OK) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
            np2audio86_sustained_producer_full(&runtime->sustained, 0U);
#endif
            continue;
        }
        if (status != NP2_OPNGEN_PCM_RING_FULL) return false;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        np2audio86_sustained_producer_full(&runtime->sustained, 1U);
#endif
        runtime->pcm_worker_space_waiting.store(1U, std::memory_order_release);
        notify_pcm_consumer(runtime);
        drive_pcm_retry_controller(runtime);
        drive_pcm_s2_reset_controller(runtime, frames - appended);
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            runtime->physical_diagnostic_next_published_sequence.store(
                runtime->pcm_ring.next_sequence, std::memory_order_release);
            publish_physical_ring_context(runtime);
#endif
            resolve_post_done_retry(runtime);
            notify_pcm_consumer(runtime);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            if (runtime->terminal_worker_timing.current_phase.load(
                    std::memory_order_acquire) ==
                    static_cast<uint32_t>(terminal_timing::Phase::PcmFinish))
                record_terminal_worker_point(
                    runtime, terminal_timing::Point::T10PcmFinishComplete);
#endif
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
    if (runtime == nullptr) {
        vTaskSuspend(nullptr);
        return;
    }
    if (np2_pcm_output_start(&runtime->pcm_controller) != NP2_PCM_OUTPUT_OK) {
        /* A sink start FATAL is an output failure.  Publish the same forced
         * abort authority as a submit/finish failure, run the controller's
         * abort cleanup while FAILED, then use the established quiescent /
         * owner-reap protocol instead of self-deleting. */
        publish_pcm_forced_abort(runtime, kErrorWorker);
        if (np2_pcm_output_abort(&runtime->pcm_controller) != NP2_PCM_OUTPUT_OK)
            fail(runtime, kErrorWorker);
        runtime->pcm_consumer_terminal_ack.store(1U,
                                                 std::memory_order_release);
        runtime->pcm_consumer_quiescent.store(1U, std::memory_order_release);
        (void)xSemaphoreGive(runtime->pcm_ready);
        (void)xSemaphoreGive(runtime->pcm_done_semaphore);
        vTaskSuspend(nullptr);
        return;
    }
    runtime->pcm_consumer_ready.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->pcm_ready);
    bool released = false;
    for (;;) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        publish_physical_runnable(
            runtime, runtime->sustained.next_accepted_sequence);
#endif
        const uint32_t occupancy = np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
        const bool production_done =
            runtime->pcm_production_done.load(std::memory_order_acquire) != 0U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (occupancy == 0U)
            np2audio86_sustained_consumer_empty(
                &runtime->sustained, released ? 1U : 0U,
                production_done ? 1U : 0U);
#endif
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
                       kPcmPermissionLifecycle ||
                       kPcmLifecycleScenario ==
                           kPcmLifecyclePostDoneConsumerFatal ||
                       kPcmLifecycleScenario == kPcmLifecycleFinishFatal ||
                       (kPcmS2PartialLifecycle && production_done);
        if (!released && occupancy != 0U &&
            runtime->pcm_s2_consumer_fault_ready.load(
                std::memory_order_acquire) != 0U)
            released = true;
        if (runtime->pcm_forced_abort_requested.load(std::memory_order_acquire) != 0U) {
            if (np2_pcm_output_abort(&runtime->pcm_controller) != NP2_PCM_OUTPUT_OK)
                fail(runtime, kErrorWorker);
            runtime->pcm_consumer_terminal_ack.store(1U, std::memory_order_release);
            break;
        }
        if (released && occupancy != 0U) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            const uint32_t physical_step_sequence =
                runtime->sustained.next_accepted_sequence;
            publish_physical_diagnostic(
                runtime, P4_NANO_AUDIO86_PROGRESS_STEP_ENTER,
                P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT,
                physical_step_sequence,
                runtime->sustained.next_accepted_sequence,
                physical_diagnostic_now_us());
#endif
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
            const uint32_t physical_retry_snapshot =
                p4_nano_audio86_physical_sink_retry_snapshot(
                    runtime->physical_sink);
#endif
            const enum np2_pcm_output_status status =
                np2_pcm_output_step(&runtime->pcm_controller);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            const uint64_t physical_step_returned_us =
                physical_diagnostic_now_us();
            publish_physical_diagnostic(
                runtime, P4_NANO_AUDIO86_PROGRESS_STEP_EXIT,
                status == NP2_PCM_OUTPUT_RETRY
                    ? P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF
                    : P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF,
                physical_step_sequence,
                runtime->sustained.next_accepted_sequence,
                physical_step_returned_us);
            observe_physical_qovf(runtime, physical_step_returned_us);
#endif
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
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
                       !p4_nano_audio86_physical_sink_retry_ready(
                           runtime->physical_sink, physical_retry_snapshot)) {
#else
                       runtime->pcm_sink_permission.load(
                           std::memory_order_acquire) == kPcmSinkPermissionHold) {
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
                    publish_physical_wait_enter(
                        runtime, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
                        physical_step_sequence);
#endif
                    (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, portMAX_DELAY);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
                    publish_physical_wait_resume(
                        runtime, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
                        physical_step_sequence);
#endif
                    ++runtime->pcm_retry_wakes;
                }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
                observe_physical_qovf(runtime, physical_diagnostic_now_us());
#endif
                if (runtime->pcm_retry_wakes == 0U)
                    runtime->pcm_retry_wait_skipped_ready = 1U;
                continue;
            }
            if (status != NP2_PCM_OUTPUT_CONSUMED) {
                runtime->pcm_retry_tail_after =
                    runtime->pcm_ring.tail.load(std::memory_order_acquire);
                publish_pcm_forced_abort(runtime, kErrorWorker);
                if (kPcmLifecycleScenario ==
                    kPcmLifecyclePostDoneConsumerFatal)
                    runtime->pcm_post_done_retry_waiting.store(
                        0U, std::memory_order_release);
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            publish_physical_runnable(
                runtime, runtime->sustained.next_accepted_sequence);
            publish_physical_ring_context(runtime);
#endif
            notify_worker(runtime);
            continue;
        }
        if (production_done && occupancy == 0U) {
            runtime->pcm_retry_done_only_after_empty =
                runtime->pcm_retry_waiting.load(std::memory_order_acquire) == 0U
                    ? 1U : 0U;
            runtime->pcm_eos_after_done = 1U;
            runtime->pcm_finish_after_empty = 1U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            publish_physical_diagnostic(
                runtime, P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY,
                P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH,
                runtime->sustained.next_accepted_sequence,
                runtime->sustained.next_accepted_sequence,
                physical_diagnostic_now_us());
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            publish_physical_wait_enter(
                runtime,
                P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL,
                runtime->sustained.next_accepted_sequence);
#endif
            const enum np2_pcm_output_status finish_status =
                np2_pcm_output_finish(&runtime->pcm_controller);
            if (finish_status != NP2_PCM_OUTPUT_OK) {
                runtime->pcm_lifecycle_triggered.store(
                    1U, std::memory_order_release);
                runtime->pcm_s2_controller_driven.store(
                    1U, std::memory_order_release);
                runtime->pcm_s2_cutpoint_occupancy = occupancy;
                runtime->pcm_s2_cutpoint_partial =
                    np2opngen_pcm_ring_producer_partial_valid_frames(
                        &runtime->pcm_ring);
                runtime->pcm_s2_cutpoint_rendered = static_cast<uint32_t>(
                    runtime->pcm_semantic_rendered_frames);
                publish_pcm_forced_abort(runtime, kErrorWorker);
                if (np2_pcm_output_abort(&runtime->pcm_controller) !=
                    NP2_PCM_OUTPUT_OK)
                    fail(runtime, kErrorWorker);
                runtime->pcm_consumer_terminal_ack.store(
                    1U, std::memory_order_release);
            } else {
                runtime->pcm_ack_after_finish = runtime->pcm_sink_finished;
                runtime->pcm_s2_terminal_success = 1U;
                runtime->pcm_consumer_terminal_ack.store(1U, std::memory_order_release);
            }
            break;
        }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        const p4_nano_audio86_consumer_wait_reason wait_reason = released
            ? P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY
            : P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL;
        publish_physical_wait_enter(
            runtime, wait_reason, runtime->sustained.next_accepted_sequence);
#endif
        (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, portMAX_DELAY);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        publish_physical_wait_resume(
            runtime, wait_reason, runtime->sustained.next_accepted_sequence);
#endif
    }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    publish_physical_wait_enter(
        runtime, P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL,
        runtime->sustained.next_accepted_sequence);
#endif
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
                                             runtime->reset_ack_held_ordinal);
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        !np2audio86_runtime_semantic_event_permitted(
            &runtime->producer_clock) ||
#endif
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
    const bool reset = opcode == NP2_AUDIO86_EVENT_RESET_BARRIER;
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    const bool terminal_reset_test = reset && runtime->terminal_horizon_armed;
    if (terminal_reset_test) {
        /* Test-only rendezvous: park the real worker before publishing the
         * final event, so the producer can inspect both release boundaries
         * without consuming either production transport object. */
        runtime->terminal_test_worker_hold.store(1U,
                                                  std::memory_order_release);
        notify_worker(runtime);
        const TickType_t start = xTaskGetTickCount();
        while (runtime->terminal_test_worker_hold_ack.load(
                   std::memory_order_acquire) == 0U &&
               xTaskGetTickCount() - start < kTimeout)
            vTaskDelay(1U);
        if (runtime->terminal_test_worker_hold_ack.load(
                std::memory_order_acquire) == 0U) {
            fail(runtime, kErrorTransport);
            return;
        }
        runtime->terminal_test_notify_before_event =
            runtime->terminal_test_worker_notify_count.load(
                std::memory_order_acquire);
    }
#endif
    const int enqueue = reset
        ? np2audio86_reset_event_ring_enqueue(
              &runtime->events, &event, &runtime->reset_ordinal)
        : np2audio86_event_ring_enqueue(&runtime->events, &event);
    if (enqueue != NP2_AUDIO86_TRANSPORT_OK) {
        fail(runtime, kErrorTransport); return;
    }
    runtime->reserved_events = 0U;
    runtime->event_committed = true;
    ++runtime->next_sequence;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    const bool defer_worker_notify = reset && runtime->terminal_horizon_armed;
    if (defer_worker_notify) {
        /* np2audio86_reset_event_ring_enqueue() freezes the ordinal in the
         * event payload before publishing the ring head.  Preserve that same
         * transaction-owned identity for the following terminal mailbox. */
        runtime->terminal_reset_notify_deferred = true;
        runtime->terminal_reset_transaction_ordinal = runtime->reset_ordinal;
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
        const np2audio86_event *published_event = nullptr;
        runtime->terminal_test_phase.store(1U, std::memory_order_release);
        vTaskDelay(1U); /* force a scheduling opportunity in the R9 window */
        runtime->terminal_test_notify_after_event =
            runtime->terminal_test_worker_notify_count.load(
                std::memory_order_acquire);
        runtime->terminal_test_event_before_terminal =
            np2audio86_event_ring_peek(&runtime->events, &published_event) ==
                    NP2_AUDIO86_TRANSPORT_OK &&
                published_event != nullptr &&
                published_event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
                published_event->payload ==
                    runtime->terminal_reset_transaction_ordinal
                ? 1U
                : 0U;
        runtime->terminal_test_terminal_absent_before_release =
            !np2audio86_runtime_horizon_pending(&runtime->control) ? 1U : 0U;
#endif
    }
    if (!defer_worker_notify)
#endif
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
    const bool reset = runtime->transaction_kind == NP2AUDIO86_GUEST_TRANSACTION_RESET;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    const bool terminal = reset && runtime->terminal_horizon_armed;
    const uint32_t terminal_reset_ordinal =
        runtime->terminal_reset_transaction_ordinal;
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST == 2
    const int publish_status = terminal
        ? NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT
        : np2audio86_runtime_horizon_publish(
              &runtime->control, &runtime->producer_clock, frame);
#else
    const int publish_status = terminal
        ? ((frame == kSustainedTerminalResetFrame &&
            runtime->terminal_reset_notify_deferred &&
            terminal_reset_ordinal != 0U)
               ? np2audio86_runtime_terminal_horizon_publish(
                     &runtime->control, &runtime->producer_clock,
                     kRenderFrames, kRenderFrames, terminal_reset_ordinal)
               : NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT)
        : np2audio86_runtime_horizon_publish(
              &runtime->control, &runtime->producer_clock, frame);
#endif
#else
    const int publish_status = np2audio86_runtime_horizon_publish(
        &runtime->control, &runtime->producer_clock, frame);
#endif
    if (publish_status != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (terminal && runtime->terminal_reset_notify_deferred) {
            /* The RESET ring head cannot be rolled back.  Close producer-only
             * ownership, latch the failure, and issue a wake even if another
             * failure won the first-error race so visible work is never
             * stranded behind the deferred hint. */
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
            const np2audio86_event *published_event = nullptr;
            runtime->terminal_test_partial_failure_event_visible =
                np2audio86_event_ring_peek(&runtime->events,
                                            &published_event) ==
                        NP2_AUDIO86_TRANSPORT_OK &&
                    published_event != nullptr &&
                    published_event->opcode ==
                        NP2_AUDIO86_EVENT_RESET_BARRIER
                    ? 1U
                    : 0U;
            runtime->terminal_test_worker_hold.store(
                0U, std::memory_order_release);
#endif
            runtime->transaction_active = false;
            runtime->horizon_owned = false;
            runtime->transaction_kind = 0U;
            runtime->event_committed = false;
            runtime->run_committed = false;
            runtime->terminal_horizon_armed = false;
            runtime->terminal_reset_publication_failed_ordinal.store(
                terminal_reset_ordinal, std::memory_order_release);
            runtime->terminal_reset_notify_deferred = false;
            runtime->terminal_reset_transaction_ordinal = 0U;
            fail(runtime, kErrorTransport);
            notify_worker(runtime);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
            runtime->terminal_test_partial_failure_wake_issued = 1U;
            runtime->terminal_test_phase.store(4U,
                                                std::memory_order_release);
#endif
            return;
        }
#endif
        fail(runtime, kErrorTransport); return;
    }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    if (terminal) {
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
        const np2audio86_event *published_event = nullptr;
        const bool event_visible =
            np2audio86_event_ring_peek(&runtime->events, &published_event) ==
                NP2_AUDIO86_TRANSPORT_OK &&
            published_event != nullptr &&
            published_event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
            published_event->frame_timestamp == kSustainedTerminalResetFrame &&
            published_event->payload == terminal_reset_ordinal;
        const bool terminal_visible =
            runtime->control.horizon.horizon_state.load(
                std::memory_order_acquire) ==
                NP2_AUDIO86_RUNTIME_HORIZON_FULL &&
            runtime->control.horizon.horizon_frame_lo ==
                static_cast<uint32_t>(kRenderFrames) &&
            runtime->control.horizon.horizon_frame_hi == 0U &&
            runtime->control.horizon.horizon_flags ==
                NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL &&
            runtime->control.horizon.terminal_reset_ordinal ==
                terminal_reset_ordinal &&
            runtime->producer_clock.terminal_published_owner == 1U;
        runtime->terminal_test_pre_ack_state =
            event_visible && terminal_visible &&
                np2audio86_runtime_reset_ack(&runtime->control) == 0U &&
                runtime->producer_done.load(std::memory_order_acquire) == 0U
                ? 1U
                : 0U;
        runtime->terminal_test_phase.store(2U, std::memory_order_release);
        runtime->terminal_test_worker_hold.store(0U,
                                                  std::memory_order_release);
#endif
        runtime->terminal_horizon_armed = false;
        runtime->terminal_reset_notify_deferred = false;
        runtime->terminal_reset_transaction_ordinal = 0U;
        runtime->terminal_horizon_published.store(1U,
                                                   std::memory_order_release);
    }
#endif
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
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        if (kPcmS2ResetLifecycle)
            runtime->pcm_s2_reset_guest_linearized.store(
                1U, std::memory_order_release);
#endif
        const uint32_t ordinal = runtime->reset_ordinal;
        if (kPressureScenario == kPressureResetAck)
            pressure_capture_before(runtime);
        while (!failed(runtime) && np2audio86_runtime_reset_ack(&runtime->control) < ordinal) {
            runtime->producer_waiting.store(1U, std::memory_order_release);
            (void)p4_nano_audio86_notifications::wait_producer();
            runtime->producer_waiting.store(0U, std::memory_order_release);
        }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (terminal) {
            /* RESET ACK remains the first lifecycle boundary.  Terminal PCM
             * completion is a separate predicate: the worker can satisfy it
             * without guest producer_done, and the producer cannot race into
             * post-ACK snapshot/unbind work before q399 is durable. */
            while (!failed(runtime) &&
                   runtime->terminal_pcm_ready.load(
                       std::memory_order_acquire) == 0U) {
                runtime->producer_waiting.store(1U,
                                                std::memory_order_release);
                (void)p4_nano_audio86_notifications::wait_producer();
                runtime->producer_waiting.store(0U,
                                                std::memory_order_release);
            }
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
            runtime->terminal_test_q399_before_producer_continuation =
                runtime->terminal_pcm_ready.load(std::memory_order_acquire) !=
                        0U &&
                    runtime->producer_done.load(std::memory_order_acquire) ==
                        0U &&
                    runtime->rendered_frame_published.load(
                        std::memory_order_acquire) == kRenderFrames &&
                    runtime->terminal_test_q399_ring_visible.load(
                        std::memory_order_acquire) != 0U &&
                    runtime->pcm_production_done.load(
                        std::memory_order_acquire) != 0U
                    ? 1U
                    : 0U;
#endif
        }
#endif
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        if (runtime->terminal_worker_timing.current_phase.load(
                std::memory_order_acquire) ==
                static_cast<uint32_t>(terminal_timing::Phase::PostResetRender) &&
            target_frame == kRenderFrames &&
            runtime->rendered_frame + frames == kRenderFrames) {
            record_terminal_worker_point(
                runtime, terminal_timing::Point::T8PostResetSynthesisComplete);
            publish_terminal_worker_phase(runtime,
                                          terminal_timing::Phase::Q399Publish);
        }
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        runtime->pcm_semantic_rendered_frames += frames;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        runtime->physical_diagnostic_rendered_frames.store(
            static_cast<uint32_t>(runtime->pcm_semantic_rendered_frames),
            std::memory_order_release);
        publish_physical_ring_context(runtime);
#endif
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (np2audio86_sustained_generated(
                &runtime->sustained,
                runtime->sustained.next_generated_sequence,
                runtime->rendered_frame, runtime->canonical,
                static_cast<uint16_t>(frames)) != 0)
            return false;
#else
        const size_t offset = static_cast<size_t>(runtime->rendered_frame) * 4U;
        std::memcpy(runtime->full_pcm + offset, runtime->canonical, frames * 4U);
        if (!runtime->reset_seen)
            std::memcpy(runtime->pre_reset_pcm + offset, runtime->canonical, frames * 4U);
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        if (!append_pcm(runtime, runtime->canonical, frames, runtime->rendered_frame))
            return false;
#endif
        runtime->rendered_frame += frames;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        runtime->rendered_frame_published.store(runtime->rendered_frame,
                                                std::memory_order_release);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
        if (runtime->rendered_frame == kRenderFrames &&
            runtime->sustained.next_generated_sequence == kExpectedPcmSlots) {
            runtime->terminal_test_q399_ring_visible.store(
                1U, std::memory_order_release);
            if (runtime->terminal_test_q398_accepted.load(
                    std::memory_order_acquire) != 0U)
                runtime->terminal_test_deadline_virtual_gap_ms =
                    NP2_AUDIO86_SUSTAINED_QUANTUM_MS;
        }
#endif
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
        if (kPcmS2PartialLifecycle &&
            runtime->pcm_s2_controller_driven.load(
                std::memory_order_acquire) != 0U && failed(runtime))
            break;
#endif
    }
    return true;
}

#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
void sustained_trace_apply(Runtime *runtime, const ApplyRecord *record);
#endif

bool apply_event(Runtime *runtime, const np2audio86_event *event)
{
    if (event == nullptr
#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        || runtime->applied_count.load(std::memory_order_relaxed) >= 32U
#endif
        )
        return false;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    const bool s2_reset_event = kPcmS2ResetLifecycle &&
        event->frame_timestamp == kS2ResetFrameOffset + 13U &&
        !runtime->reset_seen;
    if (s2_reset_event)
        runtime->pcm_s2_reset_rendering.store(1U, std::memory_order_release);
#endif
    const bool rendered = render_until(runtime, event->frame_timestamp);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    if (s2_reset_event)
        runtime->pcm_s2_reset_rendering.store(0U, std::memory_order_release);
#endif
    if (!rendered) return false;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    const bool terminal_reset_timing_event =
        event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
        event->frame_timestamp == kSustainedTerminalResetFrame;
    if (terminal_reset_timing_event)
        record_terminal_worker_point(
            runtime, terminal_timing::Point::T1PreResetRenderComplete);
#endif
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
        action_payload = 0U;
        if (runtime->reset_seen || event->payload == 0U) return false;
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    if (terminal_reset_timing_event) {
        publish_terminal_worker_phase(runtime,
                                      terminal_timing::Phase::ResetApply);
        record_terminal_worker_point(
            runtime, terminal_timing::Point::T2ResetActionBegin);
    }
#endif
    const int result = np2audio86_guest_action_apply(&runtime->render, &guest_action, data,
                                                      byte_count, runtime->source,
                                                      sizeof(runtime->source));
    if (result != 0) return false;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    if (terminal_reset_timing_event) {
        record_terminal_worker_point(
            runtime, terminal_timing::Point::T3ResetActionComplete);
        publish_terminal_worker_phase(runtime,
                                      terminal_timing::Phase::ResetEvidence);
    }
#endif
    const uint32_t apply_count = runtime->applied_count.fetch_add(
        1U, std::memory_order_relaxed);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    const uint32_t apply_index = static_cast<uint32_t>(
        np2audio86_guest_trace_window_index(apply_count, 32U));
#else
    const uint32_t apply_index = apply_count;
#endif
    runtime->applied[apply_index] = {
        event->frame_timestamp, event->sequence, opcode, action,
        byte_offset, byte_count, action_payload};
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    sustained_trace_apply(runtime, &runtime->applied[apply_index]);
#endif
    const bool reset_event = event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    /* Copy producer-owned slot data before consume releases the slot. */
    const uint32_t reset_event_ordinal = reset_event ? event->payload : 0U;
#endif
    if (reset_event) {
        if (kPressureScenario == kPressureResetAck) {
            runtime->reset_ack_held_ordinal = event->payload;
            runtime->reset_ack_held.store(1U, std::memory_order_release);
            runtime->pressure_phase.store(3U, std::memory_order_release);
            notify_worker(runtime);
        } else {
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
            runtime->reset_ack_after_ring = runtime->reset_applied_after_ring;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
            if (event->sequence > UINT32_MAX) return false;
            np2audio86_sustained_freeze_reset(
                &runtime->sustained, event->frame_timestamp,
                static_cast<uint32_t>(event->sequence), event->payload,
                runtime->pcm_ring.next_frame_offset,
                runtime->reset_applied_after_ring ? 1U : 0U,
                runtime->reset_ack_after_ring ? 1U : 0U);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            if (terminal_reset_timing_event) {
                record_terminal_worker_point(
                    runtime, terminal_timing::Point::T4ResetEvidenceComplete);
                publish_terminal_worker_phase(runtime,
                                              terminal_timing::Phase::ResetAck);
            }
#endif
#endif
#endif
            np2audio86_runtime_reset_ack_publish(&runtime->control,
                                                 event->payload);
            notify_producer(runtime);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
            if (terminal_reset_timing_event) {
                record_terminal_worker_point(
                    runtime, terminal_timing::Point::T5ResetAckPublished);
                publish_terminal_worker_phase(
                    runtime, terminal_timing::Phase::ResetEventConsume);
            }
#endif
        }
    }
    if (np2audio86_event_ring_consume(&runtime->events) !=
        NP2_AUDIO86_TRANSPORT_OK)
        return false;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    if (reset_event) {
        runtime->terminal_reset_applied_ordinal = reset_event_ordinal;
        if (runtime->terminal_reset_publication_failed_ordinal.load(
                std::memory_order_acquire) == reset_event_ordinal)
            runtime->terminal_reset_publication_failed_ordinal.store(
                0U, std::memory_order_release);
    }
#endif
    return true;
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
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    /* S2 RESET profiles begin with exactly one full ring of durable pre-RESET
     * PCM.  The first real guest event is timestamped at the same frame, so
     * only the later 13-frame RESET boundary remains producer-local (R) when
     * the held first sink submission closes the ring. */
    if (kPcmS2ResetLifecycle &&
        !render_until(runtime, kS2ResetFrameOffset)) {
        fail(runtime, kErrorFinalRender);
    }
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    bool pcm_finished = false;
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    bool terminal_horizon_observed = false;
    uint64_t terminal_horizon = 0U;
    uint32_t terminal_reset_ordinal = 0U;
#endif
    for (;;) {
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
        if (runtime->terminal_test_worker_hold.load(
                std::memory_order_acquire) != 0U) {
            runtime->terminal_test_worker_hold_ack.store(
                1U, std::memory_order_release);
            (void)p4_nano_audio86_notifications::wait_worker();
            continue;
        }
#endif
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
                np2audio86_runtime_reset_ack_publish(
                    &runtime->control, runtime->reset_ack_held_ordinal);
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
        np2audio86_runtime_horizon_observation horizon_observation{};
        const int horizon = np2audio86_runtime_horizon_try_observe_detail(
            &runtime->control, &runtime->consumer_clock,
            &horizon_observation);
        const int peek = np2audio86_event_ring_peek(&runtime->events, &event);
        if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_OK) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
            if (horizon_observation.flags ==
                NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL) {
                if (terminal_horizon_observed ||
                    horizon_observation.frame != kRenderFrames ||
                    horizon_observation.terminal_reset_ordinal == 0U) {
                    fail(runtime, kErrorTransport);
                } else {
                    terminal_horizon_observed = true;
                    terminal_horizon = horizon_observation.frame;
                    terminal_reset_ordinal =
                        horizon_observation.terminal_reset_ordinal;
                    runtime->terminal_horizon_observed.store(
                        1U, std::memory_order_release);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
                    runtime->terminal_test_worker_observed_pair =
                        peek == NP2_AUDIO86_TRANSPORT_OK && event != nullptr &&
                                event->opcode ==
                                    NP2_AUDIO86_EVENT_RESET_BARRIER &&
                                event->payload == terminal_reset_ordinal
                            ? 1U
                            : 0U;
#endif
                }
            } else if (horizon_observation.flags !=
                           NP2_AUDIO86_RUNTIME_HORIZON_FLAG_NONE ||
                       terminal_horizon_observed) {
                fail(runtime, kErrorTransport);
            }
#else
            if (horizon_observation.flags !=
                NP2_AUDIO86_RUNTIME_HORIZON_FLAG_NONE)
                fail(runtime, kErrorTransport);
#endif
            notify_producer(runtime);
        }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
        if (terminal_horizon_observed &&
            runtime->terminal_worker_timing.timestamps[
                terminal_timing::point_index(
                    terminal_timing::Point::T0TerminalPairObserved)] ==
                terminal_timing::kUnset &&
            terminal_horizon == kRenderFrames &&
            terminal_reset_ordinal != 0U &&
            peek == NP2_AUDIO86_TRANSPORT_OK && event != nullptr &&
            event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
            event->frame_timestamp == kSustainedTerminalResetFrame &&
            event->payload == terminal_reset_ordinal) {
            publish_terminal_worker_phase(
                runtime, terminal_timing::Phase::TerminalObserved);
            record_terminal_worker_point(
                runtime, terminal_timing::Point::T0TerminalPairObserved);
            publish_terminal_worker_phase(
                runtime, terminal_timing::Phase::PreResetRender);
        }
#endif
        const bool terminal_reset_failure_recovery =
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
            failed(runtime) && peek == NP2_AUDIO86_TRANSPORT_OK &&
            event != nullptr &&
            event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
            event->payload != 0U &&
            event->payload ==
                runtime->terminal_reset_publication_failed_ordinal.load(
                    std::memory_order_acquire);
#else
            false;
#endif
        if (peek == NP2_AUDIO86_TRANSPORT_OK && event != nullptr &&
            (event->frame_timestamp <=
                 runtime->consumer_clock.committed_frame_reconstructed ||
             terminal_reset_failure_recovery)) {
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
            /* The register write immediately preceding RESET shares its
             * timestamp.  Do not enter the full-ring cutpoint until the real
             * RESET transaction has committed and guest semantics have
             * linearized; commit_horizon supplies the level plus wake. */
            if (kPcmS2ResetLifecycle && !runtime->reset_seen &&
                event->frame_timestamp == kS2ResetFrameOffset + 13U &&
                runtime->pcm_s2_reset_guest_linearized.load(
                    std::memory_order_acquire) == 0U) {
                (void)p4_nano_audio86_notifications::wait_worker();
                continue;
            }
#endif
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
            const bool terminal_reset_event =
                event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
                terminal_horizon_observed;
            const uint64_t before_terminal_reset_frame =
                runtime->rendered_frame;
#endif
            if (!apply_event(runtime, event)) {
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                if (kPcmS2ResetLifecycle &&
                    event->frame_timestamp == kS2ResetFrameOffset + 13U &&
                    runtime->pcm_forced_abort_requested.load(
                        std::memory_order_acquire) != 0U) {
                    runtime->pcm_s2_reset_event_residual_before_cleanup =
                        np2audio86_event_ring_occupancy(&runtime->events);
                    runtime->pcm_s2_reset_horizon_residual_before_cleanup =
                        np2audio86_runtime_horizon_pending(&runtime->control)
                            ? 1U : 0U;
                    runtime->pcm_s2_reset_abandoned.store(
                        1U, std::memory_order_release);
                    runtime->event_lease.store(0U, std::memory_order_release);
                    runtime->byte_lease.store(0U, std::memory_order_release);
                    runtime->horizon_lease.store(0U, std::memory_order_release);
                    runtime->reset_ack_held.store(0U,
                                                  std::memory_order_release);
                    while (np2audio86_event_ring_occupancy(&runtime->events) !=
                           0U)
                        (void)np2audio86_event_ring_consume(&runtime->events);
                    runtime->failure_reset_closed.store(
                        1U, std::memory_order_release);
                    runtime->pcm_s2_reset_transport_residual_after_cleanup =
                        np2audio86_event_ring_occupancy(&runtime->events) +
                        np2audio86_byte_ring_occupancy(&runtime->bytes) +
                        (np2audio86_runtime_horizon_pending(&runtime->control)
                             ? 1U : 0U) +
                        runtime->event_lease.load(std::memory_order_acquire) +
                        runtime->byte_lease.load(std::memory_order_acquire) +
                        runtime->horizon_lease.load(std::memory_order_acquire) +
                        runtime->reset_ack_held.load(std::memory_order_acquire);
                } else
#endif
                {
                    fail(runtime, kErrorEventApply);
                }
                break;
            }
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
            if (terminal_reset_event)
                runtime->terminal_test_reset_before_remainder =
                    before_terminal_reset_frame <=
                                kSustainedTerminalResetFrame &&
                            runtime->rendered_frame ==
                                kSustainedTerminalResetFrame &&
                            runtime->terminal_reset_applied_ordinal ==
                                terminal_reset_ordinal
                        ? 1U
                        : 0U;
#endif
            notify_producer(runtime);
            continue;
        }
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (terminal_horizon_observed && !pcm_finished) {
            if (peek == NP2_AUDIO86_TRANSPORT_OK && event != nullptr &&
                event->frame_timestamp > terminal_horizon) {
                fail(runtime, kErrorTransport);
                break;
            }
            const bool transport_empty =
                np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
                np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
                !np2audio86_runtime_horizon_pending(&runtime->control);
            if (transport_empty &&
                runtime->terminal_reset_applied_ordinal ==
                    terminal_reset_ordinal) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
                record_terminal_worker_point(
                    runtime, terminal_timing::Point::T6TerminalPredicateReady);
                publish_terminal_worker_phase(
                    runtime, terminal_timing::Phase::PostResetRender);
                record_terminal_worker_point(
                    runtime, terminal_timing::Point::T7PostResetRenderBegin);
#endif
                if (!render_until(runtime, terminal_horizon)) {
                    fail(runtime, kErrorFinalRender);
                    break;
                }
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                if (!finish_pcm(runtime)) {
                    fail(runtime, kErrorFinalRender);
                    break;
                }
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                pcm_finished = true;
#endif
                runtime->terminal_pcm_before_guest_done.store(
                    runtime->producer_done.load(std::memory_order_acquire) == 0U
                        ? 1U : 0U,
                    std::memory_order_release);
                runtime->terminal_pcm_ready.store(1U,
                                                   std::memory_order_release);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
                runtime->terminal_test_retained_until_pcm_done =
                    terminal_horizon_observed &&
                            terminal_reset_ordinal ==
                                runtime->terminal_reset_applied_ordinal &&
                            runtime->rendered_frame == terminal_horizon &&
                            runtime->terminal_test_q399_ring_visible.load(
                                std::memory_order_acquire) != 0U &&
                            runtime->pcm_production_done.load(
                                std::memory_order_acquire) != 0U
                        ? 1U
                        : 0U;
                runtime->terminal_test_phase.store(3U,
                                                    std::memory_order_release);
#endif
                notify_producer(runtime);
                continue;
            }
            if (np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
                (runtime->terminal_reset_applied_ordinal !=
                     terminal_reset_ordinal ||
                 np2audio86_byte_ring_occupancy(&runtime->bytes) != 0U)) {
                /* Acquiring the terminal mailbox also acquires all prior
                 * producer publications.  An empty event ring here therefore
                 * proves that the declared RESET is absent, not merely late. */
                fail(runtime, kErrorTransport);
                break;
            }
            if (runtime->producer_done.load(std::memory_order_acquire) != 0U) {
                fail(runtime, kErrorTransport);
                break;
            }
        }
#endif
        if (runtime->producer_done.load(std::memory_order_acquire) != 0U &&
            np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
            np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
            !np2audio86_runtime_horizon_pending(&runtime->control)) {
            if (!failed(runtime)) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
                if (!terminal_horizon_observed || !pcm_finished) {
                    fail(runtime, kErrorTransport);
                }
#else
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                if (kPcmS2PartialLifecycle)
                    runtime->pcm_s2_final_rendering.store(
                        1U, std::memory_order_release);
#endif
                const bool final_rendered = render_until(runtime, kRenderFrames);
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
                if (kPcmS2PartialLifecycle)
                    runtime->pcm_s2_final_rendering.store(
                        0U, std::memory_order_release);
#endif
                if (!final_rendered) fail(runtime, kErrorFinalRender);
#endif
            }
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
    if (!pcm_finished && !finish_pcm(runtime)) fail(runtime, kErrorFinalRender);
#endif
    runtime->worker_quiescent.store(1U, std::memory_order_release);
    (void)xSemaphoreGive(runtime->done);
    vTaskSuspend(nullptr);
}

#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
void sustained_trace_apply(Runtime *runtime, const ApplyRecord *record)
{
    uint8_t canonical[kApplyRecordBytes]{};
    const auto le32 = [](uint8_t *out, const uint32_t value) {
        for (size_t i = 0U; i < 4U; ++i)
            out[i] = static_cast<uint8_t>(value >> (i * 8U));
    };
    const auto le64 = [](uint8_t *out, const uint64_t value) {
        for (size_t i = 0U; i < 8U; ++i)
            out[i] = static_cast<uint8_t>(value >> (i * 8U));
    };
    le64(canonical, record->frame);
    le64(canonical + 8U, record->sequence);
    le32(canonical + 16U, record->opcode);
    le32(canonical + 20U, record->action);
    le64(canonical + 24U, record->byte_offset);
    le32(canonical + 32U, record->byte_count);
    le32(canonical + 36U, record->payload);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_APPLY,
        canonical, sizeof(canonical));
}

void sustained_trace_event(void *opaque,
                           const np2audio86_guest_event_t *record)
{
    uint8_t canonical[24U]{};
    auto *runtime = static_cast<Runtime *>(opaque);
    const size_t bytes = np2audio86_guest_evidence_serialize_event_record(
        record, canonical);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_EVENT,
        canonical, bytes);
}

void sustained_trace_run(void *opaque,
                         const np2audio86_guest_data_run_t *record)
{
    uint8_t canonical[32U]{};
    auto *runtime = static_cast<Runtime *>(opaque);
    const size_t bytes = np2audio86_guest_evidence_serialize_run_record(
        record, canonical);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_RUN,
        canonical, bytes);
}

void sustained_trace_pcm_byte(void *opaque, const uint8_t value)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_PCM_BYTES,
        &value, 1U);
}

void sustained_trace_timer(void *opaque,
                           const np2audio86_guest_timer_trace_t *record)
{
    uint8_t canonical[28U]{};
    auto *runtime = static_cast<Runtime *>(opaque);
    const size_t bytes = np2audio86_guest_evidence_serialize_timer_record(
        record, canonical);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_TIMER,
        canonical, bytes);
}

void sustained_trace_io(void *opaque,
                        const np2audio86_guest_io_trace_t *record)
{
    uint8_t canonical[24U]{};
    auto *runtime = static_cast<Runtime *>(opaque);
    const size_t bytes = np2audio86_guest_evidence_serialize_io_record(
        record, canonical);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_IO,
        canonical, bytes);
}
#endif

#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
uint64_t sustained_guest_monotonic_us(void *)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us < 0 ? 0U : static_cast<uint64_t>(now_us);
}

void sustained_guest_delay_one_tick(void *)
{
    /* A blocking one-tick delay, rather than taskYIELD(), gives lower-priority
     * CPU1 system work a bounded opportunity to run. */
    vTaskDelay(1U);
}
#endif

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
    runtime->trace = {};
    runtime->trace.events = runtime->trace_events;
    runtime->trace.event_capacity = 64U;
    runtime->trace.data_runs = runtime->trace_runs;
    runtime->trace.data_run_capacity = 8U;
    runtime->trace.pcm_bytes = runtime->trace_bytes;
    runtime->trace.pcm_capacity = sizeof(runtime->trace_bytes);
    runtime->trace.timers = runtime->trace_timers;
    runtime->trace.timer_capacity = 64U;
    runtime->trace.io = runtime->trace_io;
    runtime->trace.io_capacity = 128U;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    runtime->trace.bounded_windows = 1U;
    runtime->trace.observer = {
        runtime, sustained_trace_event, sustained_trace_run,
        sustained_trace_pcm_byte, sustained_trace_timer, sustained_trace_io};
#endif
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
#if P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO >= 9 && \
    P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO <= 11
    np2audio86_guest_host_test_seed(kS2ResetFrameOffset, 0U);
#else
    np2audio86_guest_host_test_seed(0U, 0U);
#endif
    np2audio86_guest_host_trace_attach(&runtime->trace);
    np2audio86_guest_sink_bind(&kSink);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    const size_t program_size = np2audio86_guest_program_build_sustained_2s(
        mem, 0x90000U);
    if (program_size != 5498U) return false;
#else
    const size_t program_size = np2audio86_guest_program_build(mem, 0x90000U);
    if (program_size != 4971U) return false;
#endif
    i286c_initialize();
    i286c_reset();
    i286core.s.r.w.cs = 0U; i286core.s.cs_base = 0U;
    i286core.s.r.w.ds = 0U; i286core.s.ds_base = 0U;
    i286core.s.r.w.ss = 0U; i286core.s.ss_base = 0U;
    i286core.s.r.w.ip = 0U; i286core.s.r.w.flag = I_FLAG;
    i286core.s.adrsmask = 0xfffffU;
    nevent_get1stevent();
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    np2audio86_sustained_cooperative_scheduler cooperative{};
    if (np2audio86_sustained_cooperative_scheduler_init(
            &cooperative, sustained_guest_monotonic_us,
            sustained_guest_delay_one_tick, nullptr) != 0)
        return false;
#endif
    while (!failed(runtime)) {
        if (mem[i286core.s.r.w.ip] == 0xf4U) break;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        if (np2audio86_sustained_cooperative_checkpoint(&cooperative) < 0)
            return false;
#endif
        if (i286core.s.remainclock <= 0) { nevent_progress(); continue; }
        i286c_step();
        if (CPU_CLOCK > 100000000U) return false;
    }
    if (failed(runtime)) return false;
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    /* HLT is the producer's irrevocable terminal guest boundary.  Arming is
     * owner-local; the following RESET transaction publishes the declaration
     * only after its event slot has been released. */
    runtime->terminal_horizon_armed = true;
#endif
    board86_reset(&np2cfg, FALSE);
#if defined(P4_NANO_AUDIO86_TERMINAL_POST_PCM_FAILURE_TEST)
    if (runtime->terminal_pcm_ready.load(std::memory_order_acquire) == 0U ||
        runtime->producer_done.load(std::memory_order_acquire) != 0U)
        return false;
    if (runtime->lifecycle_runtime != nullptr)
        (void)runtime->lifecycle_runtime->mark_failure();
    fail(runtime, kErrorInjectedFatal);
    std::printf("P4_AUDIO86_TERMINAL_POST_PCM_PRODUCER_FAILURE "
                "pcm_done=1 guest_done=0 first_error=%" PRIu32
                " overall=FAIL\n",
                runtime->first_error.load(std::memory_order_acquire));
    return false;
#endif
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    np2audio86_guest_host_snapshot(&runtime->final_state);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    uint8_t canonical_state[128U]{};
    const size_t state_bytes = np2audio86_guest_evidence_serialize_state(
        &runtime->final_state, canonical_state);
    np2audio86_sustained_trace_record(
        &runtime->sustained, NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE,
        canonical_state, state_bytes);
#endif
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

#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
void capture_physical_snapshot(Runtime *runtime)
{
    PhysicalSnapshot snapshot{};
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    snapshot.semantic_frames = runtime->sustained.next_generated_frame_offset;
    snapshot.semantic_bytes = runtime->sustained.generated.bytes;
    np2audio86_sustained_digest_snapshot(
        &runtime->sustained.generated, &snapshot.semantic_crc32,
        snapshot.semantic_sha256);
#else
#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)
    const uint8_t *const semantic_pcm = runtime->pre_reset_pcm;
    const uint64_t semantic_frames = runtime->pre_reset_frame;
#else
    const uint8_t *const semantic_pcm = runtime->full_pcm;
    const uint64_t semantic_frames = kRenderFrames;
#endif
    const size_t semantic_bytes =
        static_cast<size_t>(semantic_frames) *
        P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
    snapshot.semantic_frames = semantic_frames;
    snapshot.semantic_bytes = semantic_bytes;
    snapshot.semantic_crc32 = np2_crc32_iso_hdlc(
        semantic_pcm, semantic_bytes);
    np2_sha256_context sha{};
    np2_sha256_init(&sha);
    np2_sha256_update(&sha, semantic_pcm, semantic_bytes);
    np2_sha256_final(&sha, snapshot.semantic_sha256);
#endif
    p4_nano_audio86_physical_sink_get_telemetry(runtime->physical_sink,
                                                 &snapshot.sink);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    freeze_first_qovf_worker_phase(runtime, &snapshot.sink);
#endif
    snapshot.controller_accepted_frames =
        runtime->pcm_controller.accepted_frames;
    snapshot.controller_accepted_bytes =
        runtime->pcm_controller.accepted_bytes;
    snapshot.controller_state = runtime->pcm_controller.state;
    snapshot.first_error = runtime->first_error.load(std::memory_order_acquire);
    snapshot.forced_abort = runtime->pcm_forced_abort_requested.load(
        std::memory_order_acquire);
    snapshot.produced_frames = runtime->pcm_produced_frames;
    snapshot.produced_bytes = runtime->pcm_produced_bytes;
    snapshot.produced_slots = runtime->pcm_produced_slots;
    snapshot.final_ring_occupancy =
        np2opngen_pcm_ring_occupancy(&runtime->pcm_ring);
    snapshot.final_ring_partial =
        np2opngen_pcm_ring_producer_partial_valid_frames(&runtime->pcm_ring);
    snapshot.drops = runtime->pcm_drops;
    snapshot.overwrites = runtime->pcm_overwrites;
    snapshot.abandoned_published_frames =
        runtime->pcm_abandoned_published_frames;
    snapshot.abandoned_partial_frames = runtime->pcm_abandoned_partial_frames;
    snapshot.abandoned_rendered_frames = runtime->pcm_abandoned_rendered_frames;
    snapshot.captured = true;
    runtime->physical = snapshot;
}

void record_physical_destroy(Runtime *runtime, const bool destroyed)
{
    if (!runtime->physical.captured) return;
    runtime->physical.sink_destroyed = destroyed ? 1U : 0U;
    runtime->physical.first_error = runtime->first_error.load(
        std::memory_order_acquire);
}
#endif

#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
bool cleanup_pcm_start_failure(Runtime *runtime)
{
    const bool terminal = runtime->pcm_done_semaphore != nullptr &&
        xSemaphoreTake(runtime->pcm_done_semaphore, kTimeout) == pdTRUE;
    const bool quiescent =
        runtime->pcm_consumer_quiescent.load(std::memory_order_acquire) != 0U;
    const bool ack = runtime->pcm_consumer_terminal_ack.load(
                         std::memory_order_acquire) != 0U;
    const bool suspended = runtime->pcm_consumer != nullptr && terminal &&
        quiescent && ack && wait_task_suspended(runtime->pcm_consumer);
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
    runtime->test_terminal_wait = terminal ? 1U : 0U;
    runtime->test_quiescent_observed = quiescent ? 1U : 0U;
    runtime->test_ack_observed = ack ? 1U : 0U;
    runtime->test_suspended_observed = suspended ? 1U : 0U;
#endif
    runtime->pcm_consumer_suspended_observed.store(suspended ? 1U : 0U,
                                                   std::memory_order_release);
    runtime->pcm_join_timeout = suspended ? 0U : 1U;
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    /* PHYSICAL_TELEMETRY_SNAPSHOT_POINT=
     * AFTER_DONE_SEMAPHORE_ACK_QUIESCENT_AND_ESUSPENDED_
     * BEFORE_TASK_DELETE_AND_SINK_DESTROY */
    const bool physical_snapshot_ready = terminal && quiescent && ack &&
        suspended;
    if (physical_snapshot_ready && runtime->physical_sink != nullptr)
        capture_physical_snapshot(runtime);
#endif
    if (suspended) {
        vTaskDelete(runtime->pcm_consumer);
        runtime->pcm_consumer = nullptr;
        runtime->pcm_consumer_deleted_after_suspended.store(
            1U, std::memory_order_release);
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
        runtime->test_delete_performed = 1U;
#endif
    }
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    if (suspended && runtime->physical_sink != nullptr) {
        const bool destroyed = p4_nano_audio86_physical_sink_destroy(
                                   runtime->physical_sink) == 0;
        record_physical_destroy(runtime, destroyed);
        if (!destroyed)
            return false;
        runtime->physical_sink = nullptr;
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
        runtime->test_sink_destroy_performed = 1U;
#endif
    }
#endif
    return terminal && quiescent && ack && suspended;
}
#endif

#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
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
#endif

#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
void print_sustained_digest(const char *name,
                            const np2audio86_sustained_digest *digest)
{
    uint32_t crc32 = 0U;
    uint8_t sha256[NP2_SHA256_DIGEST_SIZE]{};
    np2audio86_sustained_digest_snapshot(digest, &crc32, sha256);
    std::printf("%s_SERIALIZED_BYTES=%" PRIu64 "\n%s_CRC32=%08" PRIx32
                "\n%s_SHA256=", name, digest->bytes, name, crc32, name);
    for (const uint8_t byte : sha256) std::printf("%02x", byte);
    std::printf("\n");
}
#endif

#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
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
#endif

#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
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
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE) && \
    !defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
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
#if defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
    std::printf("REAL_P4_AUDIO_TIMING=VALIDATED_BY_5D2_S2_PHYSICAL_EXEC\n");
#else
    std::printf("REAL_P4_AUDIO_TIMING=NOT_VALIDATED\n");
#endif
}
#else
void emit_sustained_evidence(const Runtime *runtime)
{
    const auto &e = runtime->sustained;
    std::printf("WORKLOAD_ID=FULL_REPLAY_PCM_SUSTAINED_2S_V1\n");
    std::printf("SEMANTIC_DURATION_MS=2000\nSUSTAINED_Q240_UNITS=400\n");
    print_sustained_digest("GUEST_IO",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO]);
    std::printf("GUEST_IO_RECORDS=%" PRIu64 "\n",
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].records);
    print_sustained_digest("AUDIO_EVENTS",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT]);
    std::printf("AUDIO_EVENTS_RECORDS=%" PRIu64 "\n",
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT].records);
    print_sustained_digest("PCM86_DATA_RUNS",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_RUN]);
    std::printf("PCM86_DATA_RUNS_RECORDS=%" PRIu64 "\n",
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_RUN].records);
    print_sustained_digest("PCM86_BYTES",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_PCM_BYTES]);
    print_sustained_digest("TIMER_PIC",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER]);
    std::printf("TIMER_PIC_RECORDS=%" PRIu64 "\n",
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER].records);
    print_sustained_digest("WORKER_APPLY_TRACE",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY]);
    std::printf("WORKER_APPLY_TRACE_RECORDS=%" PRIu64 "\n",
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY].records);
    print_sustained_digest("FINAL_G_STATE",
        &e.trace[NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE]);
    print_sustained_digest("FULL_PCM", &e.generated);
    std::printf("FULL_PCM_FRAMES=%" PRIu64 "\n",
                e.next_generated_frame_offset);
    print_sustained_digest("ACCEPTED_PCM", &e.accepted);
    std::printf("ACCEPTED_PCM_FRAMES=%" PRIu64 "\n",
                e.next_accepted_frame_offset);
    std::printf("PRE_RESET_PCM_SERIALIZED_BYTES=%" PRIu64
                "\nPRE_RESET_PCM_CRC32=%08" PRIx32 "\nPRE_RESET_PCM_SHA256=",
                e.reset.bytes, e.reset.crc32);
    for (const uint8_t byte : e.reset.sha256) std::printf("%02x", byte);
    std::printf("\nPRE_RESET_PCM_FRAMES=%" PRIu64 "\n", e.reset.frames);
    std::printf("P4_AUDIO86_SUSTAINED_SLOT first_sequence=%" PRIu32
                " first_offset=%" PRIu64 " first_crc32=%08" PRIx32
                " final_sequence=%" PRIu32 " final_offset=%" PRIu64
                " final_crc32=%08" PRIx32 " storage=BOUNDED\n",
                e.first_accepted.sequence, e.first_accepted.frame_offset,
                e.first_accepted.crc32, e.final_accepted.sequence,
                e.final_accepted.frame_offset, e.final_accepted.crc32);
    std::printf("P4_AUDIO86_SUSTAINED_RESET frame=%" PRIu64
                " sequence=%" PRIu32 " ordinal=%" PRIu32
                " ring_next_frame=%" PRIu64 " applied_after_ring=%u"
                " ack_after_apply=%u\n",
                e.reset.reset_event_frame, e.reset.reset_event_sequence,
                e.reset.reset_ordinal, e.reset.ring_next_frame_offset,
                e.reset.applied_after_ring, e.reset.ack_after_apply);
    std::printf("P4_AUDIO86_SUSTAINED_TRACE io=%" PRIu64 "/128"
                " events=%" PRIu64 "/64 runs=%" PRIu64 "/8"
                " timers=%" PRIu64 "/64 applied=%" PRIu64 "/32"
                " model=FIRST_HALF_LAST_HALF_ALL_DIGESTED\n",
                static_cast<uint64_t>(runtime->trace.io_count),
                static_cast<uint64_t>(runtime->trace.event_count),
                static_cast<uint64_t>(runtime->trace.data_run_count),
                static_cast<uint64_t>(runtime->trace.timer_count),
                static_cast<uint64_t>(runtime->applied_count.load()));
    std::printf("P4_AUDIO86_SUSTAINED_RING max_occupancy=%" PRIu32
                " producer_full_wait_count=%" PRIu32
                " consumer_premature_empty_count=%" PRIu32 "\n",
                e.pcm_ring_max_occupancy, e.pcm_producer_full_wait_count,
                e.pcm_consumer_premature_empty_count);
    std::printf("P4_AUDIO86_SUSTAINED_TIMING stream_started_ms=%" PRIu64
                " drain_completed_ms=%" PRIu64 " stream_wall_ms=%" PRIu64
                " max_running_accept_gap_ms=%" PRIu64
                " progress_bound_ms=%u authority=HOST_ONLY\n",
                e.stream_started_ms, e.drain_completed_ms,
                np2audio86_sustained_stream_wall_ms(&e),
                e.max_running_accept_gap_ms,
                NP2_AUDIO86_SUSTAINED_PROGRESS_BOUND_MS);
    std::printf("P4_AUDIO86_SUSTAINED_MEMORY evidence_fixed_bytes=%zu"
                " duration_dependent_pcm_bytes=0 ring_bytes=%zu\n",
                sizeof(e), sizeof(runtime->pcm_ring));
}
#endif

#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE) || \
    defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE) || \
    defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
const char *physical_controller_state_name(
    const enum np2_pcm_output_state state)
{
    switch (state) {
    case NP2_PCM_OUTPUT_INITIAL: return "INITIAL";
    case NP2_PCM_OUTPUT_STARTED: return "STARTED";
    case NP2_PCM_OUTPUT_FAILED: return "FAILED";
    case NP2_PCM_OUTPUT_FINISHED: return "FINISHED";
    case NP2_PCM_OUTPUT_ABORTED: return "ABORTED";
    }
    return "UNKNOWN";
}

const char *physical_sink_state_name(
    const enum p4_nano_audio86_physical_state state)
{
    switch (state) {
    case P4_NANO_AUDIO86_PHYSICAL_INITIAL: return "INITIAL";
    case P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING:
        return "PREPARED_ACCEPTING";
    case P4_NANO_AUDIO86_PHYSICAL_STARTING: return "STARTING";
    case P4_NANO_AUDIO86_PHYSICAL_RUNNING: return "RUNNING";
    case P4_NANO_AUDIO86_PHYSICAL_DRAINING: return "DRAINING";
    case P4_NANO_AUDIO86_PHYSICAL_FAILED: return "FAILED";
    case P4_NANO_AUDIO86_PHYSICAL_ABORTING: return "ABORTING";
    case P4_NANO_AUDIO86_PHYSICAL_QUIESCENT: return "QUIESCENT";
    }
    return "UNKNOWN";
}
#endif

#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)
void emit_physical_s1_evidence(const Runtime *runtime)
{
    const PhysicalSnapshot &snapshot = runtime->physical;
    if (!snapshot.captured) return;
    const p4_nano_audio86_physical_telemetry &sink = snapshot.sink;
    const uint32_t drain_post_snapshot_eofs =
        sink.drain_completion_epoch - sink.drain_snapshot_epoch;
    const uint32_t quiescent_post_snapshot_eofs =
        sink.quiescent_eof_epoch - sink.drain_snapshot_epoch;
    const uint64_t padding_bytes = sink.physical_padding_frames *
        P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
    std::printf("5D2_S1_IDENTITY schema=2 evidence_class=PHYSICAL_EXEC"
                " source_git_sha=%s"
                " profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S_SHORT"
                " board=P4_NANO_P4_V1X backend=IDF_I2S0_ES8311"
                " stimulus=PRE_RESET_PCM display=DISABLED\n",
                P4_AUDIO86_GIT_SHA);
    std::printf("5D2_S1_START schema=2 evidence_class=PHYSICAL_EXEC"
                " rate_hz=48000 channels=2 sample_bits=16 encoding=S16LE"
                " i2s_format=PHILIPS clock_source=APLL mclk_multiple=256"
                " mclk_hz=12288000 q_frames=%u bytes_per_frame=%u"
                " physical_unit_bytes=%u dma_desc=%u dma_frames=%u"
                " prepare_completed=%u pa_initial_low=%u"
                " codec_initialized_muted=%u i2s_initialized=%u"
                " muted_warmup_completed=%u callbacks_registered=%u"
                " stream_started=%u codec_unmute_completed=%u\n",
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME,
                P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES,
                P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS,
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                sink.prepare_completed ? 1U : 0U,
                sink.pa_initial_low ? 1U : 0U,
                sink.codec_initialized_muted ? 1U : 0U,
                sink.i2s_initialized ? 1U : 0U,
                sink.muted_warmup_completed ? 1U : 0U,
                sink.callbacks_registered ? 1U : 0U,
                sink.stream_started ? 1U : 0U,
                sink.codec_unmute_completed ? 1U : 0U);
    std::printf("5D2_S1_PCM schema=2 evidence_class=PHYSICAL_EXEC"
                " semantic_frames=%" PRIu64 " semantic_bytes=%" PRIu64
                " semantic_crc32=%08" PRIx32 " semantic_sha256=",
                snapshot.semantic_frames, snapshot.semantic_bytes,
                snapshot.semantic_crc32);
    for (const uint8_t byte : snapshot.semantic_sha256)
        std::printf("%02x", byte);
    std::printf(" controller_accepted_frames=%" PRIu64
                " controller_accepted_bytes=%" PRIu64
                " sink_accepted_frames=%" PRIu64
                " sink_accepted_bytes=%" PRIu64
                " full_units=%" PRIu32 " final_partial_units=%" PRIu32
                " final_valid_frames=%" PRIu32
                " physical_units=%" PRIu64 " physical_bytes=%" PRIu64
                " padding_frames=%" PRIu64 " padding_bytes=%" PRIu64
                " submit_attempts=%" PRIu64 " retry_count=%" PRIu64 "\n",
                snapshot.controller_accepted_frames,
                snapshot.controller_accepted_bytes,
                sink.semantic_accepted_frames, sink.semantic_accepted_bytes,
                sink.full_units, sink.final_partial_units,
                sink.final_valid_frames, sink.physical_units_copied,
                sink.physical_bytes_copied, sink.physical_padding_frames,
                padding_bytes, sink.submit_attempts, sink.retry_count);
    std::printf("5D2_S1_FINISH schema=2 evidence_class=PHYSICAL_EXEC"
                " controller_state=%s sink_state=%s"
                " final_copy_eof_epoch=%" PRIu32
                " drain_completion_eof_epoch=%" PRIu32
                " quiescent_eof_epoch=%" PRIu32
                " drain_post_snapshot_eofs=%" PRIu32
                " quiescent_post_snapshot_eofs=%" PRIu32
                " drain_duration_ms=%" PRIu64 " finish_completed=%u"
                " pending_frames=%" PRIu64 " drained_frames=%" PRIu64
                " discarded_frames=%" PRIu64
                " running_q_ovf=%" PRIu32 " draining_q_ovf=%" PRIu32
                " sticky_error=%u registered_generation=%" PRIu32
                " terminal_generation=%" PRIu32
                " stale_callbacks=%" PRIu32
                " callback_in_flight=%" PRIu32 " callbacks_active=%u"
                " codec_final_muted=%u pa_final_low=%u"
                " i2s_enabled=%u i2s_created=%u"
                " first_error=%" PRIu32 " forced_abort=%" PRIu32
                " sink_destroyed=%" PRIu32 "\n",
                physical_controller_state_name(snapshot.controller_state),
                physical_sink_state_name(sink.state),
                sink.drain_snapshot_epoch, sink.drain_completion_epoch,
                sink.quiescent_eof_epoch, drain_post_snapshot_eofs,
                quiescent_post_snapshot_eofs, sink.drain_duration_ms,
                sink.finish_completed ? 1U : 0U,
                sink.accepted_pending_drain_frames,
                sink.physically_drained_frames,
                sink.physically_discarded_accepted_frames,
                sink.running_queue_overflow_count,
                sink.draining_queue_overflow_count,
                sink.sticky_error ? 1U : 0U,
                sink.registered_generation, sink.generation,
                sink.stale_callback_count, sink.callback_refcount,
                sink.callbacks_active ? 1U : 0U,
                sink.codec_final_muted ? 1U : 0U,
                sink.pa_final_low ? 1U : 0U,
                sink.i2s_enabled ? 1U : 0U,
                sink.i2s_created ? 1U : 0U,
                snapshot.first_error, snapshot.forced_abort,
                snapshot.sink_destroyed);
}

bool physical_s1_snapshot_healthy(const Runtime *runtime)
{
    return p4_nano_audio86_terminal_predicate::physical_s1_snapshot_healthy(
        runtime->physical, kRenderFrames);
}
#endif

#if defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
void emit_physical_s2_evidence(const Runtime *runtime)
{
    const PhysicalSnapshot &snapshot = runtime->physical;
    if (!snapshot.captured) return;
    const p4_nano_audio86_physical_telemetry &sink = snapshot.sink;
    const uint32_t drain_post_snapshot_eofs =
        sink.drain_completion_epoch - sink.drain_snapshot_epoch;
    const uint32_t quiescent_post_snapshot_eofs =
        sink.quiescent_eof_epoch - sink.drain_snapshot_epoch;
    const uint64_t padding_bytes = sink.physical_padding_frames *
        P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
    const uint64_t running_units = sink.physical_units_copied >=
            sink.preloaded_units
        ? sink.physical_units_copied - sink.preloaded_units
        : 0U;
    constexpr uint64_t kSemanticDurationMs =
        kRenderFrames * 1000U / 48000U;
    std::printf("5D2_S2_IDENTITY schema=1 evidence_class=PHYSICAL_EXEC"
                " source_git_sha=%s"
                " profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S"
                " board=P4_NANO_P4_V1X backend=IDF_I2S0_ES8311"
                " stimulus=FULL_REPLAY_PCM display=DISABLED\n",
                P4_AUDIO86_GIT_SHA);
    std::printf("5D2_S2_START schema=1 evidence_class=PHYSICAL_EXEC"
                " rate_hz=48000 channels=2 sample_bits=16 encoding=S16LE"
                " i2s_format=PHILIPS clock_source=APLL mclk_multiple=256"
                " mclk_hz=12288000 q_frames=%u bytes_per_frame=%u"
                " physical_unit_bytes=%u dma_desc=%u dma_frames=%u"
                " ring_capacity=%u prefill=%" PRIu32
                " prepare_completed=%u pa_initial_low=%u"
                " codec_initialized_muted=%u i2s_initialized=%u"
                " muted_warmup_completed=%u callbacks_registered=%u"
                " stream_started=%u codec_unmute_completed=%u\n",
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME,
                P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES,
                P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS,
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                NP2_OPNGEN_PCM_RING_CAPACITY, kPcmPrefillSlots,
                sink.prepare_completed ? 1U : 0U,
                sink.pa_initial_low ? 1U : 0U,
                sink.codec_initialized_muted ? 1U : 0U,
                sink.i2s_initialized ? 1U : 0U,
                sink.muted_warmup_completed ? 1U : 0U,
                sink.callbacks_registered ? 1U : 0U,
                sink.stream_started ? 1U : 0U,
                sink.codec_unmute_completed ? 1U : 0U);
    std::printf("5D2_S2_STREAM schema=1 evidence_class=PHYSICAL_EXEC"
                " semantic_frames=%" PRIu64 " semantic_bytes=%" PRIu64
                " semantic_crc32=%08" PRIx32 " semantic_sha256=",
                snapshot.semantic_frames, snapshot.semantic_bytes,
                snapshot.semantic_crc32);
    for (const uint8_t byte : snapshot.semantic_sha256)
        std::printf("%02x", byte);
    std::printf(" produced_frames=%" PRIu64
                " produced_bytes=%" PRIu64 " produced_slots=%" PRIu32
                " controller_accepted_frames=%" PRIu64
                " controller_accepted_bytes=%" PRIu64
                " sink_accepted_frames=%" PRIu64
                " sink_accepted_bytes=%" PRIu64
                " physical_units=%" PRIu64 " full_units=%" PRIu32
                " final_partial_units=%" PRIu32
                " final_valid_frames=%" PRIu32
                " padding_frames=%" PRIu64 " padding_bytes=%" PRIu64
                " preloaded_units=%" PRIu32 " running_units=%" PRIu64
                " submit_attempts=%" PRIu64 " retry_count=%" PRIu64
                " running_q_ovf=%" PRIu32
                " final_ring_occupancy=%" PRIu32
                " final_ring_partial=%" PRIu32
                " drops=%" PRIu32 " overwrite=%" PRIu32
                " abandoned_published=%" PRIu64
                " abandoned_partial=%" PRIu64
                " abandoned_rendered=%" PRIu64
                " semantic_duration_ms=%" PRIu64 "\n",
                snapshot.produced_frames, snapshot.produced_bytes,
                snapshot.produced_slots, snapshot.controller_accepted_frames,
                snapshot.controller_accepted_bytes,
                sink.semantic_accepted_frames, sink.semantic_accepted_bytes,
                sink.physical_units_copied, sink.full_units,
                sink.final_partial_units, sink.final_valid_frames,
                sink.physical_padding_frames, padding_bytes,
                sink.preloaded_units, running_units, sink.submit_attempts,
                sink.retry_count, sink.running_queue_overflow_count,
                snapshot.final_ring_occupancy, snapshot.final_ring_partial,
                snapshot.drops, snapshot.overwrites,
                snapshot.abandoned_published_frames,
                snapshot.abandoned_partial_frames,
                snapshot.abandoned_rendered_frames, kSemanticDurationMs);
    std::printf("5D2_S2_FINISH schema=1 evidence_class=PHYSICAL_EXEC"
                " controller_state=%s sink_state=%s"
                " final_copy_eof_epoch=%" PRIu32
                " drain_completion_eof_epoch=%" PRIu32
                " quiescent_eof_epoch=%" PRIu32
                " drain_post_snapshot_eofs=%" PRIu32
                " quiescent_post_snapshot_eofs=%" PRIu32
                " drain_duration_ms=%" PRIu64 " finish_completed=%u"
                " pending_frames=%" PRIu64 " drained_frames=%" PRIu64
                " discarded_frames=%" PRIu64
                " draining_q_ovf=%" PRIu32
                " sticky_error=%u registered_generation=%" PRIu32
                " terminal_generation=%" PRIu32
                " stale_callbacks=%" PRIu32
                " callback_in_flight=%" PRIu32 " callbacks_active=%u"
                " codec_final_muted=%u pa_final_low=%u"
                " i2s_enabled=%u i2s_created=%u"
                " first_error=%" PRIu32 " forced_abort=%" PRIu32
                " sink_destroyed=%" PRIu32 "\n",
                physical_controller_state_name(snapshot.controller_state),
                physical_sink_state_name(sink.state),
                sink.drain_snapshot_epoch, sink.drain_completion_epoch,
                sink.quiescent_eof_epoch, drain_post_snapshot_eofs,
                quiescent_post_snapshot_eofs, sink.drain_duration_ms,
                sink.finish_completed ? 1U : 0U,
                sink.accepted_pending_drain_frames,
                sink.physically_drained_frames,
                sink.physically_discarded_accepted_frames,
                sink.draining_queue_overflow_count,
                sink.sticky_error ? 1U : 0U,
                sink.registered_generation, sink.generation,
                sink.stale_callback_count, sink.callback_refcount,
                sink.callbacks_active ? 1U : 0U,
                sink.codec_final_muted ? 1U : 0U,
                sink.pa_final_low ? 1U : 0U,
                sink.i2s_enabled ? 1U : 0U,
                sink.i2s_created ? 1U : 0U,
                snapshot.first_error, snapshot.forced_abort,
                snapshot.sink_destroyed);
}

bool physical_s2_snapshot_healthy(const Runtime *runtime)
{
    constexpr uint64_t kExpectedUnits =
        kRenderFrames / P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT;
    return p4_nano_audio86_terminal_predicate::physical_s2_snapshot_healthy(
        runtime->physical, kRenderFrames, kExpectedUnits,
        P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS);
}
#endif

#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
const char *consumer_service_phase_name(const uint32_t phase)
{
    switch (phase) {
    case P4_NANO_AUDIO86_CONSUMER_PHASE_NONE: return "NONE";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_START_ENABLE: return "START_ENABLE";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_CODEC_UNMUTE: return "CODEC_UNMUTE";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT:
        return "DOWNSTREAM_SUBMIT";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_POST_ACCEPT_EVIDENCE:
        return "POST_ACCEPT_EVIDENCE";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF: return "WAIT_EOF";
    case P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH: return "FINISH";
    }
    return "UNKNOWN";
}

const char *consumer_wait_reason_name(const uint32_t reason)
{
    switch (reason) {
    case P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE: return "RUNNABLE";
    case P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF: return "RETRY_EOF_WAIT";
    case P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY:
        return "PCM_RING_EMPTY_WAIT";
    case P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL: return "PCM_PREFILL_WAIT";
    case P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL:
        return "FINISH_OR_TERMINAL_WAIT";
    }
    return "UNKNOWN";
}

const char *first_qovf_state_name(const uint32_t state)
{
    if (state == 0U) return "NONE";
    if (state == P4_NANO_AUDIO86_PHYSICAL_STARTING) return "STARTING";
    if (state == P4_NANO_AUDIO86_PHYSICAL_RUNNING) return "RUNNING";
    return "UNKNOWN";
}

const char *terminal_worker_phase_name(const uint32_t phase)
{
    switch (static_cast<terminal_timing::Phase>(phase)) {
    case terminal_timing::Phase::None: return "NONE";
    case terminal_timing::Phase::TerminalObserved: return "TERMINAL_OBSERVED";
    case terminal_timing::Phase::PreResetRender: return "PRE_RESET_RENDER";
    case terminal_timing::Phase::ResetApply: return "RESET_APPLY";
    case terminal_timing::Phase::ResetEvidence: return "RESET_EVIDENCE";
    case terminal_timing::Phase::ResetAck: return "RESET_ACK";
    case terminal_timing::Phase::ResetEventConsume:
        return "RESET_EVENT_CONSUME";
    case terminal_timing::Phase::PostResetRender: return "POST_RESET_RENDER";
    case terminal_timing::Phase::Q399Publish: return "Q399_PUBLISH";
    case terminal_timing::Phase::PcmFinish: return "PCM_FINISH";
    }
    return "UNKNOWN";
}

void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    for (size_t i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i)
        std::printf("%02x", digest[i]);
}

void snapshot_digest(const np2audio86_sustained_digest &digest,
                     uint32_t *crc32,
                     uint8_t sha256[NP2_SHA256_DIGEST_SIZE])
{
    np2audio86_sustained_digest_snapshot(&digest, crc32, sha256);
}

void emit_physical_5d3_s1_evidence(const Runtime *runtime)
{
    const PhysicalSnapshot &snapshot = runtime->physical;
    if (!snapshot.captured) return;
    const auto &e = runtime->sustained;
    const auto &sink = snapshot.sink;
    uint32_t generated_crc = 0U, accepted_crc = 0U;
    uint32_t io_crc = 0U, event_crc = 0U, timer_crc = 0U;
    uint32_t action_crc = 0U, final_state_crc = 0U;
    uint8_t generated_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t accepted_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t io_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t event_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t timer_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t action_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t final_state_sha[NP2_SHA256_DIGEST_SIZE]{};
    snapshot_digest(e.generated, &generated_crc, generated_sha);
    snapshot_digest(e.accepted, &accepted_crc, accepted_sha);
    snapshot_digest(e.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO], &io_crc, io_sha);
    snapshot_digest(e.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT],
                    &event_crc, event_sha);
    snapshot_digest(e.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER],
                    &timer_crc, timer_sha);
    snapshot_digest(e.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY],
                    &action_crc, action_sha);
    snapshot_digest(e.trace[NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE],
                    &final_state_crc, final_state_sha);
    const uint32_t drain_delta =
        sink.drain_completion_epoch - sink.drain_snapshot_epoch;
    const uint32_t quiescent_delta =
        sink.quiescent_eof_epoch - sink.drain_snapshot_epoch;
    const uint64_t padding_bytes = sink.physical_padding_frames *
        P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
    const uint64_t running_units = sink.physical_units_copied >=
            sink.preloaded_units
        ? sink.physical_units_copied - sink.preloaded_units : 0U;

    std::printf("5D3_S1_IDENTITY schema=4 evidence_class=PHYSICAL_EXEC"
                " source_git_sha=%s"
                " profile=AUDIO86_REAL_GUEST_SUSTAINED_2S_PHYSICAL_I2S"
                " workload_id=FULL_REPLAY_PCM_SUSTAINED_2S_V1"
                " board=P4_NANO_P4_V1X backend=IDF_I2S0_ES8311"
                " display=DISABLED guest_program_bytes=5498"
                " guest_program_crc32=e577580a"
                " guest_program_sha256=56443e5c4e524a34e046387e83ef7f89b647d60bc0b3c2ff7c84abe5084a6ce7\n",
                P4_AUDIO86_GIT_SHA);
    std::printf("5D3_S1_START schema=4 evidence_class=PHYSICAL_EXEC"
                " rate_hz=48000 channels=2 sample_bits=16 encoding=S16LE"
                " i2s_format=PHILIPS clock_source=APLL mclk_multiple=256"
                " mclk_hz=12288000 q_frames=%u bytes_per_frame=%u"
                " physical_unit_bytes=%u dma_desc=%u dma_frames=%u"
                " ring_capacity=%u prefill=%" PRIu32
                " semantic_duration_ms=2000"
                " expected_units=400 prepare_completed=%u pa_initial_low=%u"
                " codec_initialized_muted=%u i2s_initialized=%u"
                " muted_warmup_completed=%u callbacks_registered=%u"
                " stream_started=%u codec_unmute_completed=%u"
                " startup_durations_valid=%" PRIu32
                " enable_stream_duration_us=%" PRIu32
                " codec_unmute_duration_us=%" PRIu32 "\n",
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME,
                P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES,
                P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS,
                P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT,
                NP2_OPNGEN_PCM_RING_CAPACITY, kPcmPrefillSlots,
                sink.prepare_completed ? 1U : 0U, sink.pa_initial_low ? 1U : 0U,
                sink.codec_initialized_muted ? 1U : 0U,
                sink.i2s_initialized ? 1U : 0U,
                sink.muted_warmup_completed ? 1U : 0U,
                sink.callbacks_registered ? 1U : 0U,
                sink.stream_started ? 1U : 0U,
                sink.codec_unmute_completed ? 1U : 0U,
                sink.startup_durations_valid,
                sink.enable_stream_duration_us,
                sink.codec_unmute_duration_us);
    std::printf("5D3_S1_STREAM schema=4 evidence_class=PHYSICAL_EXEC"
                " generated_frames=%" PRIu64 " generated_bytes=%" PRIu64
                " generated_crc32=%08" PRIx32 " generated_sha256=",
                e.next_generated_frame_offset, e.generated.bytes, generated_crc);
    print_sha256(generated_sha);
    std::printf(" accepted_frames=%" PRIu64 " accepted_bytes=%" PRIu64
                " accepted_crc32=%08" PRIx32 " accepted_sha256=",
                e.next_accepted_frame_offset, e.accepted.bytes, accepted_crc);
    print_sha256(accepted_sha);
    std::printf(" generated_units=%" PRIu32 " accepted_units=%" PRIu32
                " next_generated_sequence=%" PRIu32
                " next_accepted_sequence=%" PRIu32
                " next_generated_frame_offset=%" PRIu64
                " next_accepted_frame_offset=%" PRIu64
                " generated_slot_fill_frames=%u"
                " first_sequence=%" PRIu32 " first_offset=%" PRIu64
                " first_valid_frames=%u first_crc32=%08" PRIx32
                " final_sequence=%" PRIu32 " final_offset=%" PRIu64
                " final_slot_valid_frames=%u final_crc32=%08" PRIx32
                " pre_reset_frames=%" PRIu64 " pre_reset_bytes=%" PRIu64
                " pre_reset_crc32=%08" PRIx32 " pre_reset_sha256=",
                e.next_generated_sequence, e.next_accepted_sequence,
                e.next_generated_sequence, e.next_accepted_sequence,
                e.next_generated_frame_offset, e.next_accepted_frame_offset,
                e.generated_slot_fill_frames,
                e.first_accepted.sequence, e.first_accepted.frame_offset,
                e.first_accepted.valid_frames, e.first_accepted.crc32,
                e.final_accepted.sequence, e.final_accepted.frame_offset,
                e.final_accepted.valid_frames, e.final_accepted.crc32,
                e.reset.frames, e.reset.bytes, e.reset.crc32);
    print_sha256(e.reset.sha256);
    std::printf(" reset_frame=%" PRIu64 " reset_sequence=%" PRIu32
                " reset_ordinal=%" PRIu32 " reset_opcode=2147483648"
                " io_count=%" PRIu64 " io_crc32=%08" PRIx32 " io_sha256=",
                e.reset.reset_event_frame, e.reset.reset_event_sequence,
                e.reset.reset_ordinal,
                e.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].records, io_crc);
    print_sha256(io_sha);
    std::printf(" event_count=%" PRIu64 " event_crc32=%08" PRIx32
                " event_sha256=",
                e.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT].records, event_crc);
    print_sha256(event_sha);
    std::printf(" timer_count=%" PRIu64 " timer_crc32=%08" PRIx32
                " timer_sha256=",
                e.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER].records, timer_crc);
    print_sha256(timer_sha);
    std::printf(" action_count=%" PRIu64 " action_crc32=%08" PRIx32
                " action_sha256=",
                e.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY].records, action_crc);
    print_sha256(action_sha);
    std::printf(" final_state_count=%" PRIu64
                " final_state_crc32=%08" PRIx32 " final_state_sha256=",
                e.trace[NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE].records,
                final_state_crc);
    print_sha256(final_state_sha);
    std::printf(" controller_accepted_frames=%" PRIu64
                " controller_accepted_bytes=%" PRIu64
                " sink_accepted_frames=%" PRIu64
                " sink_accepted_bytes=%" PRIu64
                " physical_units=%" PRIu64 " full_units=%" PRIu32
                " final_partial_units=%" PRIu32
                " final_valid_frames=%" PRIu32
                " padding_frames=%" PRIu64 " padding_bytes=%" PRIu64
                " submit_attempts=%" PRIu64 " retry_count=%" PRIu64
                " retry_identity_failures=%" PRIu32
                " retry_episode_units=%" PRIu32
                " direct_running_accept_units=%" PRIu32
                " running_q_ovf=%" PRIu32
                " final_ring_occupancy=%" PRIu32
                " final_ring_partial=%" PRIu32
                " drops=%" PRIu32 " overwrite=%" PRIu32
                " abandoned_published=%" PRIu64
                " abandoned_partial=%" PRIu64
                " abandoned_rendered=%" PRIu64 "\n",
                snapshot.controller_accepted_frames,
                snapshot.controller_accepted_bytes,
                sink.semantic_accepted_frames, sink.semantic_accepted_bytes,
                sink.physical_units_copied, sink.full_units,
                sink.final_partial_units, sink.final_valid_frames,
                sink.physical_padding_frames, padding_bytes,
                sink.submit_attempts, sink.retry_count,
                e.retry_identity_failures, e.retry_episode_units,
                e.direct_running_accept_units,
                sink.running_queue_overflow_count,
                snapshot.final_ring_occupancy, snapshot.final_ring_partial,
                snapshot.drops, snapshot.overwrites,
                snapshot.abandoned_published_frames,
                snapshot.abandoned_partial_frames,
                snapshot.abandoned_rendered_frames);
    std::printf("5D3_S1_PROGRESS schema=4 evidence_class=PHYSICAL_EXEC"
                " pcm_ring_max_occupancy=%" PRIu32
                " pcm_producer_full_wait_count=%" PRIu32
                " pcm_consumer_empty_after_release_before_done_count=%" PRIu32
                " max_running_accept_gap_ms=%" PRIu64
                " stream_started_ms=%" PRIu64
                " drain_completed_ms=%" PRIu64
                " stream_wall_ms=%" PRIu64
                " preloaded_units=%" PRIu32
                " running_accepted_units=%" PRIu64
                " max_gap_initial=%u"
                " max_gap_previous_sequence_valid=%u"
                " max_gap_previous_sequence=%" PRIu32
                " max_gap_next_sequence=%" PRIu32
                " max_gap_previous_relative_ms=%" PRIu64
                " max_gap_next_relative_ms=%" PRIu64
                " max_downstream_submit_us=%" PRIu32
                " max_downstream_submit_sequence=%" PRIu32
                " max_post_accept_evidence_us=%" PRIu32
                " max_post_accept_evidence_sequence=%" PRIu32
                " timing_authority=HOST_ONLY\n",
                e.pcm_ring_max_occupancy, e.pcm_producer_full_wait_count,
                e.pcm_consumer_premature_empty_count,
                e.max_running_accept_gap_ms, e.stream_started_ms,
                e.drain_completed_ms, np2audio86_sustained_stream_wall_ms(&e),
                sink.preloaded_units, running_units,
                e.max_running_gap_initial,
                e.max_running_gap_previous_sequence_valid,
                e.max_running_gap_previous_sequence,
                e.max_running_gap_next_sequence,
                e.max_running_gap_previous_relative_ms,
                e.max_running_gap_next_relative_ms,
                e.max_downstream_submit_us,
                e.max_downstream_submit_sequence,
                e.max_post_accept_evidence_us,
                e.max_post_accept_evidence_sequence);
    const auto &terminal = runtime->terminal_worker_timing;
    const auto &t = terminal.timestamps;
    const uint32_t q399_final_published =
        runtime->physical_diagnostic_next_published_sequence.load(
            std::memory_order_acquire) == kExpectedPcmSlots
        ? 1U : 0U;
    const uint32_t q399_final_rendered =
        runtime->physical_diagnostic_rendered_frames.load(
            std::memory_order_acquire);
    std::printf("5D3_S1_TERMINAL_TIMING schema=4"
                " evidence_class=PHYSICAL_EXEC"
                " clock=TASK_CONTEXT_RELATIVE_US unset=UINT32_MAX points=11"
                " phase_enum=NONE:0,TERMINAL_OBSERVED:1,PRE_RESET_RENDER:2,RESET_APPLY:3,RESET_EVIDENCE:4,RESET_ACK:5,RESET_EVENT_CONSUME:6,POST_RESET_RENDER:7,Q399_PUBLISH:8,PCM_FINISH:9"
                " current_phase=%s first_qovf_worker_phase=%s"
                " t0=%" PRIu32 " t1=%" PRIu32 " t2=%" PRIu32
                " t3=%" PRIu32 " t4=%" PRIu32 " t5=%" PRIu32
                " t6=%" PRIu32 " t7=%" PRIu32 " t8=%" PRIu32
                " t9=%" PRIu32 " t10=%" PRIu32
                " terminal_to_pre_reset_done_us=%" PRIu32
                " reset_action_us=%" PRIu32
                " reset_observability_us=%" PRIu32
                " ack_to_terminal_ready_us=%" PRIu32
                " post_reset_synthesis_us=%" PRIu32
                " post_reset_evidence_and_publish_us=%" PRIu32
                " pcm_finish_us=%" PRIu32
                " terminal_to_q399_publish_us=%" PRIu32
                " q399_published=%" PRIu32
                " q399_rendered_frames=%" PRIu32
                " q399_valid_frames=%u pcm_production_done=%" PRIu32
                " storage_logical_bytes=%u storage_actual_bytes=%u\n",
                terminal_worker_phase_name(terminal.current_phase.load(
                    std::memory_order_acquire)),
                terminal_worker_phase_name(terminal.first_qovf_phase.load(
                    std::memory_order_acquire)),
                t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9],
                t[10], terminal_timing::duration(t[0], t[1]),
                terminal_timing::duration(t[2], t[3]),
                terminal_timing::duration(t[3], t[4]),
                terminal_timing::duration(t[5], t[6]),
                terminal_timing::duration(t[7], t[8]),
                terminal_timing::duration(t[8], t[9]),
                terminal_timing::duration(t[9], t[10]),
                terminal_timing::duration(t[0], t[9]), q399_final_published,
                q399_final_rendered,
                q399_final_published != 0U ? NP2_AUDIO86_QUANTUM_FRAMES : 0U,
                runtime->pcm_production_done.load(std::memory_order_acquire),
                static_cast<unsigned>(terminal_timing::kLogicalPayloadBytes),
                static_cast<unsigned>(sizeof(terminal_timing::Snapshot)));
    const uint32_t q399_rendered =
        sink.first_qovf_rendered_frames >= kRenderFrames ? 1U : 0U;
    const uint32_t q399_published =
        sink.first_qovf_next_published_sequence >= kExpectedPcmSlots ? 1U : 0U;
    const uint32_t q399_available = q399_published != 0U &&
            sink.first_qovf_consumer_next_sequence < kExpectedPcmSlots &&
            sink.first_qovf_ring_occupancy != 0U
        ? 1U : 0U;
    std::printf("5D3_S1_FINISH schema=4 evidence_class=PHYSICAL_EXEC"
                " controller_state=%s sink_state=%s"
                " final_copy_eof_epoch=%" PRIu32
                " drain_completion_eof_epoch=%" PRIu32
                " quiescent_eof_epoch=%" PRIu32
                " drain_post_snapshot_eofs=%" PRIu32
                " quiescent_post_snapshot_eofs=%" PRIu32
                " drain_duration_ms=%" PRIu64 " finish_completed=%u"
                " pending_frames=%" PRIu64 " drained_frames=%" PRIu64
                " discarded_frames=%" PRIu64
                " draining_q_ovf=%" PRIu32 " sticky_error=%u"
                " first_active_qovf_latched=%" PRIu32
                " first_qovf_state=%s"
                " first_qovf_eof_epoch=%" PRIu32
                " first_qovf_phase=%s"
                " first_qovf_current_sequence=%" PRIu32
                " first_qovf_published_sequence=%" PRIu32
                " first_qovf_last_step_enter_us=%" PRIu32
                " first_qovf_last_submit_return_us=%" PRIu32
                " first_qovf_last_step_exit_us=%" PRIu32
                " first_qovf_last_running_accepted_us=%" PRIu32
                " first_qovf_wait_reason=%s"
                " first_qovf_consumer_next_sequence=%" PRIu32
                " first_qovf_next_published_sequence=%" PRIu32
                " first_qovf_q399_rendered=%" PRIu32
                " first_qovf_q399_published=%" PRIu32
                " first_qovf_q399_available=%" PRIu32
                " first_qovf_ring_occupancy=%" PRIu32
                " first_qovf_production_done=%" PRIu32
                " first_qovf_rendered_frames=%" PRIu32
                " first_qovf_eof_notify_count=%" PRIu32
                " first_qovf_hpwoken_true_count=%" PRIu32
                " first_qovf_retry_wait_enter_count=%" PRIu32
                " first_qovf_retry_wait_resume_count=%" PRIu32
                " first_qovf_ring_wait_enter_count=%" PRIu32
                " first_qovf_ring_wait_resume_count=%" PRIu32
                " first_qovf_last_wait_enter_us=%" PRIu32
                " first_qovf_last_wait_resume_us=%" PRIu32
                " first_qovf_last_resume_reason=%s"
                " first_qovf_last_resume_sequence=%" PRIu32
                " notification_state_model=PROJECT_WAIT_REASON_AND_COUNTERS"
                " cpu0_task_identity=UNAVAILABLE_SAFELY"
                " service_phase_semantics=LAST_SERVICE_NOT_WAIT_REASON"
                " first_qovf_observed=%" PRIu32
                " first_qovf_observed_us=%" PRIu32
                " qovf_time_semantics=TASK_PUBLISHED_RELATIVE_US_NO_ISR_TIMER"
                " registered_generation=%" PRIu32
                " terminal_generation=%" PRIu32
                " stale_callbacks=%" PRIu32
                " callback_in_flight=%" PRIu32 " callbacks_active=%u"
                " codec_final_muted=%u pa_final_low=%u"
                " i2s_enabled=%u i2s_created=%u"
                " first_error=%" PRIu32 " forced_abort=%" PRIu32
                " sink_destroyed=%" PRIu32 "\n",
                physical_controller_state_name(snapshot.controller_state),
                physical_sink_state_name(sink.state), sink.drain_snapshot_epoch,
                sink.drain_completion_epoch, sink.quiescent_eof_epoch,
                drain_delta, quiescent_delta, sink.drain_duration_ms,
                sink.finish_completed ? 1U : 0U,
                sink.accepted_pending_drain_frames,
                sink.physically_drained_frames,
                sink.physically_discarded_accepted_frames,
                sink.draining_queue_overflow_count,
                sink.sticky_error ? 1U : 0U,
                sink.first_active_qovf_latched,
                first_qovf_state_name(sink.first_qovf_state),
                sink.first_qovf_eof_epoch,
                consumer_service_phase_name(sink.first_qovf_phase),
                sink.first_qovf_current_sequence,
                sink.first_qovf_published_sequence,
                sink.first_qovf_last_step_enter_us,
                sink.first_qovf_last_submit_return_us,
                sink.first_qovf_last_step_exit_us,
                sink.first_qovf_last_running_accepted_us,
                consumer_wait_reason_name(sink.first_qovf_wait_reason),
                sink.first_qovf_consumer_next_sequence,
                sink.first_qovf_next_published_sequence,
                q399_rendered, q399_published, q399_available,
                sink.first_qovf_ring_occupancy,
                sink.first_qovf_production_done,
                sink.first_qovf_rendered_frames,
                sink.first_qovf_eof_notify_count,
                sink.first_qovf_hpwoken_true_count,
                sink.first_qovf_retry_wait_enter_count,
                sink.first_qovf_retry_wait_resume_count,
                sink.first_qovf_ring_wait_enter_count,
                sink.first_qovf_ring_wait_resume_count,
                sink.first_qovf_last_wait_enter_us,
                sink.first_qovf_last_wait_resume_us,
                consumer_wait_reason_name(sink.first_qovf_last_resume_reason),
                sink.first_qovf_last_resume_sequence,
                sink.first_qovf_observed,
                sink.first_qovf_observed_us,
                sink.registered_generation, sink.generation,
                sink.stale_callback_count, sink.callback_refcount,
                sink.callbacks_active ? 1U : 0U,
                sink.codec_final_muted ? 1U : 0U,
                sink.pa_final_low ? 1U : 0U,
                sink.i2s_enabled ? 1U : 0U, sink.i2s_created ? 1U : 0U,
                snapshot.first_error, snapshot.forced_abort,
                snapshot.sink_destroyed);
}

p4_nano_audio86_terminal_predicate::SustainedPhysicalLocalHealth
sustained_physical_local_health(const Runtime *runtime)
{
    const auto &e = runtime->sustained;
    uint32_t generated_crc = 0U, accepted_crc = 0U;
    uint8_t generated_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t accepted_sha[NP2_SHA256_DIGEST_SIZE]{};
    snapshot_digest(e.generated, &generated_crc, generated_sha);
    snapshot_digest(e.accepted, &accepted_crc, accepted_sha);
    p4_nano_audio86_terminal_predicate::SustainedPhysicalLocalHealth local{};
    local.generated_frames = e.next_generated_frame_offset;
    local.generated_bytes = e.generated.bytes;
    local.accepted_frames = e.next_accepted_frame_offset;
    local.accepted_bytes = e.accepted.bytes;
    local.generated_units = e.next_generated_sequence;
    local.accepted_units = e.next_accepted_sequence;
    local.next_generated_sequence = e.next_generated_sequence;
    local.next_accepted_sequence = e.next_accepted_sequence;
    local.next_generated_frame_offset = e.next_generated_frame_offset;
    local.next_accepted_frame_offset = e.next_accepted_frame_offset;
    local.generated_slot_fill_frames = e.generated_slot_fill_frames;
    local.retry_pending = e.retry_pending;
    local.retry_identity_failures = e.retry_identity_failures;
    local.generated_digest_expected =
        generated_crc == kSustainedExpectedPcmCrc32 &&
        std::memcmp(generated_sha, kSustainedExpectedPcmSha256,
                    sizeof(generated_sha)) == 0;
    local.accepted_digest_matches_generated =
        accepted_crc == generated_crc &&
        std::memcmp(accepted_sha, generated_sha, sizeof(generated_sha)) == 0;
    local.reset_identity_expected = e.reset.frozen &&
        e.reset.frames == 95761U && e.reset.bytes == 383044U &&
        e.reset.crc32 == 0xc65c7a5dU &&
        std::memcmp(e.reset.sha256, kSustainedExpectedPreResetPcmSha256,
                    sizeof(e.reset.sha256)) == 0 &&
        e.reset.reset_event_frame == 95761U &&
        e.reset.reset_event_sequence == 18U && e.reset.reset_ordinal == 1U &&
        e.reset.ring_next_frame_offset == 95761U &&
        e.reset.applied_after_ring == 1U && e.reset.ack_after_apply == 1U;
    local.trace_shape_expected =
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].records == 246U &&
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT].records == 18U &&
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_RUN].records == 1U &&
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER].records == 20U &&
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY].records == 19U &&
        e.trace[NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE].records == 1U;
    return local;
}

bool physical_5d3_s1_snapshot_healthy(const Runtime *runtime)
{
    return p4_nano_audio86_terminal_predicate::
        sustained_physical_snapshot_healthy(
            runtime->physical, sustained_physical_local_health(runtime),
            kRenderFrames, kExpectedPcmSlots, kPcmPrefillSlots);
}
#endif

void emit_summary(const Runtime *runtime, const bool ok)
{
    std::printf("P4_AUDIO86_REAL_GUEST profile=1 producer=p4_nano_pc98 producer_core=1 producer_priority=3 terminal_index=0 worker_core=0 worker_priority=6 producer_index=1 worker_index=0\n");
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    std::printf("P4_AUDIO86_REAL_GUEST_FIXTURE bytes=5498 crc32=e577580a\n");
#else
    std::printf("P4_AUDIO86_REAL_GUEST_FIXTURE bytes=4971 crc32=544b2e8c\n");
#endif
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
    if (exact_evidence) {
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        emit_sustained_evidence(runtime);
#else
        emit_exact_evidence(runtime);
#endif
    }
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
            ? "RETRY_CONSUMER_FIRST" :
        kPcmLifecycleScenario == kPcmLifecycleResetStop ? "RESET_FULL_STOP" :
        kPcmLifecycleScenario == kPcmLifecycleResetFatal ? "RESET_FULL_FATAL" :
        kPcmLifecycleScenario == kPcmLifecycleResetConsumerFatal
            ? "RESET_FULL_CONSUMER_FATAL" :
        kPcmLifecycleScenario == kPcmLifecyclePartialStop ? "PARTIAL_STOP" :
        kPcmLifecycleScenario == kPcmLifecyclePartialFatal ? "PARTIAL_FATAL" :
        kPcmLifecycleScenario == kPcmLifecyclePartialConsumerFatal
            ? "PARTIAL_CONSUMER_FATAL" :
        kPcmLifecycleScenario == kPcmLifecyclePostDoneConsumerFatal
            ? "POST_DONE_CONSUMER_FATAL" :
        kPcmLifecycleScenario == kPcmLifecycleFinishFatal
            ? "FINISH_FATAL" : "NONE";
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
    if (kPcmS2Lifecycle) {
        const uint64_t accounted = runtime->pcm_controller.accepted_frames +
            runtime->pcm_abandoned_published_frames +
            runtime->pcm_abandoned_partial_frames +
            runtime->pcm_abandoned_rendered_frames;
        std::printf("P4_AUDIO86_PCM_S2_CUTPOINT scenario=%s occupancy=%" PRIu32
                    " partial=%u semantic_rendered=%" PRIu32
                    " unappended=%" PRIu32 " pcm_done=%" PRIu32
                    " ring_finished=%" PRIu32 " sink_finished=%" PRIu32
                    " terminal_success=%" PRIu32 "\n",
                    pcm_scenario, runtime->pcm_s2_cutpoint_occupancy,
                    runtime->pcm_s2_cutpoint_partial,
                    runtime->pcm_s2_cutpoint_rendered,
                    runtime->pcm_s2_cutpoint_unappended,
                    runtime->pcm_production_done.load(),
                    runtime->pcm_ring_finished.load(),
                    runtime->pcm_sink_finished,
                    runtime->pcm_s2_terminal_success);
        std::printf("P4_AUDIO86_PCM_ACCOUNTING semantic_rendered=%" PRIu64
                    " accepted=%" PRIu64 " abandoned_published=%" PRIu64
                    " abandoned_partial=%" PRIu64
                    " abandoned_rendered=%" PRIu64
                    " accounted=%" PRIu64 " semantic_bytes=%" PRIu64
                    " accounted_bytes=%" PRIu64 " identity=%u\n",
                    runtime->pcm_semantic_rendered_frames,
                    runtime->pcm_controller.accepted_frames,
                    runtime->pcm_abandoned_published_frames,
                    runtime->pcm_abandoned_partial_frames,
                    runtime->pcm_abandoned_rendered_frames, accounted,
                    runtime->pcm_semantic_rendered_frames * 4U,
                    accounted * 4U,
                    accounted == runtime->pcm_semantic_rendered_frames
                        ? 1U : 0U);
        std::printf("P4_AUDIO86_PCM_RESET_TERMINAL guest_linearized=%" PRIu32
                    " worker_applied=%" PRIu32 " ack_published=%u"
                    " abandoned=%" PRIu32 " event_before_cleanup=%" PRIu32
                    " horizon_before_cleanup=%" PRIu32
                    " transport_after_cleanup=%" PRIu32 "\n",
                    runtime->pcm_s2_reset_guest_linearized.load(),
                    runtime->reset_applied_after_ring,
                    np2audio86_runtime_reset_ack(&runtime->control) != 0U
                        ? 1U : 0U,
                    runtime->pcm_s2_reset_abandoned.load(),
                    runtime->pcm_s2_reset_event_residual_before_cleanup,
                    runtime->pcm_s2_reset_horizon_residual_before_cleanup,
                    runtime->pcm_s2_reset_transport_residual_after_cleanup);
        std::printf("P4_AUDIO86_PCM_FINISH_TERMINAL calls=%" PRIu32
                    " fatal=%" PRIu32 " sink_finished=%" PRIu32
                    " success_ack=%" PRIu32 " terminal_ack=%" PRIu32
                    " forced_abort=%" PRIu32 " abort_calls=%" PRIu32
                    " controller_state=%u\n",
                    runtime->pcm_s2_finish_calls,
                    runtime->pcm_s2_finish_fatal_observed,
                    runtime->pcm_sink_finished,
                    runtime->pcm_ack_after_finish,
                    runtime->pcm_consumer_terminal_ack.load(),
                    runtime->pcm_forced_abort,
                    runtime->pcm_sink_abort_calls,
                    static_cast<unsigned>(runtime->pcm_controller.state));
        std::printf("5C3_I2S_ACTIVE=NO\n");
    }
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
#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)
    emit_physical_s1_evidence(runtime);
#elif defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
    emit_physical_s2_evidence(runtime);
#elif defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    emit_physical_5d3_s1_evidence(runtime);
#endif
    std::printf("P4_AUDIO86_REAL_GUEST_RESULT=%s\n", ok ? "PASS" : "FAIL");
#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)
    std::printf("P4_AUDIO86_PHYSICAL_S1_TERMINAL=%s\n",
                ok ? "COMPLETE" : "FAILED");
#elif defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
    std::printf("P4_AUDIO86_PHYSICAL_S2_TERMINAL=%s\n",
                ok ? "COMPLETE" : "FAILED");
#elif defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    std::printf("P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=%s\n",
                ok ? "COMPLETE" : "FAILED");
#endif
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
    runtime->reset_ack_held_ordinal = 0U;
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
    runtime->producer_clock = {};
    runtime->consumer_clock = {};
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    runtime->terminal_horizon_armed = false;
    runtime->terminal_reset_notify_deferred = false;
    runtime->terminal_reset_transaction_ordinal = 0U;
    runtime->terminal_reset_publication_failed_ordinal.store(
        0U, std::memory_order_relaxed);
    runtime->terminal_reset_applied_ordinal = 0U;
    runtime->terminal_horizon_published.store(0U,
                                               std::memory_order_relaxed);
    runtime->terminal_horizon_observed.store(0U,
                                              std::memory_order_relaxed);
    runtime->terminal_pcm_ready.store(0U, std::memory_order_relaxed);
    runtime->terminal_pcm_before_guest_done.store(
        0U, std::memory_order_relaxed);
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    runtime->terminal_test_worker_hold.store(0U,
                                              std::memory_order_relaxed);
    runtime->terminal_test_worker_hold_ack.store(0U,
                                                  std::memory_order_relaxed);
    runtime->terminal_test_phase.store(0U, std::memory_order_relaxed);
    runtime->terminal_test_worker_notify_count.store(
        0U, std::memory_order_relaxed);
    runtime->terminal_test_q398_accepted.store(0U,
                                               std::memory_order_relaxed);
    runtime->terminal_test_q399_ring_visible.store(
        0U, std::memory_order_relaxed);
    runtime->terminal_test_q399_accepted.store(0U,
                                               std::memory_order_relaxed);
    runtime->terminal_test_notify_before_event = 0U;
    runtime->terminal_test_notify_after_event = 0U;
    runtime->terminal_test_event_before_terminal = 0U;
    runtime->terminal_test_terminal_absent_before_release = 0U;
    runtime->terminal_test_pre_ack_state = 0U;
    runtime->terminal_test_worker_observed_pair = 0U;
    runtime->terminal_test_reset_before_remainder = 0U;
    runtime->terminal_test_retained_until_pcm_done = 0U;
    runtime->terminal_test_q399_before_producer_continuation = 0U;
    runtime->terminal_test_partial_failure_event_visible = 0U;
    runtime->terminal_test_partial_failure_wake_issued = 0U;
    runtime->terminal_test_deadline_virtual_gap_ms = UINT32_MAX;
#endif
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    np2opngen_pcm_ring_init(&runtime->pcm_ring);
#if !defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    std::memset(runtime->ring_pcm, 0, sizeof(runtime->ring_pcm));
#else
    np2audio86_sustained_evidence_init(&runtime->sustained);
    runtime->rendered_frame_published.store(0U, std::memory_order_relaxed);
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    runtime->physical_diagnostic_rendered_frames.store(
        0U, std::memory_order_relaxed);
    runtime->physical_diagnostic_next_published_sequence.store(
        0U, std::memory_order_relaxed);
    runtime->physical_diagnostic_origin_us = 0U;
    runtime->physical_diagnostic_origin_valid = false;
    terminal_timing::initialize(&runtime->terminal_worker_timing);
#endif
#endif
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    runtime->physical = {};
#endif
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
        kPcmPermissionLifecycle ? kPcmSinkPermissionHold
                                : kPcmSinkPermissionAccept,
        std::memory_order_relaxed);
    runtime->pcm_retry_waiting.store(0U, std::memory_order_relaxed);
    runtime->pcm_retry_controller_driven.store(0U, std::memory_order_relaxed);
    runtime->pcm_retry_permission_before_wake.store(0U,
                                                    std::memory_order_relaxed);
    runtime->pcm_post_done_retry_waiting.store(0U,
                                               std::memory_order_relaxed);
    runtime->pcm_post_done_permission_before_wake.store(
        0U, std::memory_order_relaxed);
    runtime->pcm_s2_controller_driven.store(0U, std::memory_order_relaxed);
    runtime->pcm_s2_reset_rendering.store(0U, std::memory_order_relaxed);
    runtime->pcm_s2_final_rendering.store(0U, std::memory_order_relaxed);
    runtime->pcm_s2_consumer_fault_ready.store(0U,
                                               std::memory_order_relaxed);
    runtime->pcm_s2_reset_guest_linearized.store(
        0U, std::memory_order_relaxed);
    runtime->pcm_s2_reset_abandoned.store(0U, std::memory_order_relaxed);
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
    runtime->pcm_semantic_rendered_frames = 0U;
    runtime->pcm_s2_cutpoint_occupancy = 0U;
    runtime->pcm_s2_cutpoint_partial = 0U;
    runtime->pcm_s2_cutpoint_rendered = 0U;
    runtime->pcm_s2_cutpoint_unappended = 0U;
    runtime->pcm_s2_reset_event_residual_before_cleanup = 0U;
    runtime->pcm_s2_reset_horizon_residual_before_cleanup = 0U;
    runtime->pcm_s2_reset_transport_residual_after_cleanup = 0U;
    runtime->pcm_s2_finish_calls = 0U;
    runtime->pcm_s2_finish_fatal_observed = 0U;
    runtime->pcm_s2_terminal_success = 0U;
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
    np2_pcm_sink selected_sink = kPcmSink;
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
    if (p4_nano_audio86_physical::create_lifecycle_test(
            &runtime->physical_sink, &runtime->pcm_consumer) != ESP_OK)
        return ESP_ERR_NO_MEM;
#else
    if (p4_nano_audio86_physical::create_idf(
            &runtime->physical_sink, &runtime->pcm_consumer) != ESP_OK)
        return ESP_ERR_NO_MEM;
#endif
    selected_sink = p4_nano_audio86_physical_sink_interface(
        runtime->physical_sink);
#endif
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    /* Evidence decorates the selected sink so ACCEPTED/RETRY accounting stays
     * identical for the virtual F2 gate and a later physical profile. */
    runtime->sustained_downstream = selected_sink;
    selected_sink = kSustainedSink;
#endif
    if (np2_pcm_output_controller_init(&runtime->pcm_controller,
                                       &runtime->pcm_ring, &selected_sink) != 0) {
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
        (void)p4_nano_audio86_physical_sink_destroy(runtime->physical_sink);
        runtime->physical_sink = nullptr;
#endif
        return ESP_FAIL;
    }
    runtime->pcm_ready = xSemaphoreCreateBinaryStatic(&runtime->pcm_ready_storage);
    runtime->pcm_done_semaphore =
        xSemaphoreCreateBinaryStatic(&runtime->pcm_done_storage);
    if (runtime->pcm_ready == nullptr || runtime->pcm_done_semaphore == nullptr) {
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
        (void)p4_nano_audio86_physical_sink_destroy(runtime->physical_sink);
        runtime->physical_sink = nullptr;
#endif
        return ESP_ERR_NO_MEM;
    }
    runtime->pcm_consumer = xTaskCreateStaticPinnedToCore(
        pcm_consumer_task, "audio86_pcm_out",
        kPcmConsumerStackBytes / sizeof(StackType_t), runtime,
        kPcmConsumerPriority, runtime->pcm_consumer_stack,
        &runtime->pcm_consumer_tcb, kPcmConsumerCore);
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
    const BaseType_t pcm_ready_wait = runtime->pcm_consumer == nullptr
        ? pdFALSE : xSemaphoreTake(runtime->pcm_ready, kTimeout);
    runtime->test_ready_wait = pcm_ready_wait == pdTRUE ? 1U : 0U;
    if (runtime->pcm_consumer == nullptr || pcm_ready_wait != pdTRUE ||
        runtime->pcm_consumer_ready.load(std::memory_order_acquire) == 0U) {
#else
    if (runtime->pcm_consumer == nullptr ||
        xSemaphoreTake(runtime->pcm_ready, kTimeout) != pdTRUE ||
        runtime->pcm_consumer_ready.load(std::memory_order_acquire) == 0U) {
#endif
        publish_pcm_forced_abort(runtime, kErrorWorker);
        const bool cleaned = runtime->pcm_consumer != nullptr &&
            cleanup_pcm_start_failure(runtime);
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
        if (runtime->pcm_consumer == nullptr && runtime->physical_sink != nullptr) {
            if (p4_nano_audio86_physical_sink_destroy(runtime->physical_sink) == 0)
                runtime->physical_sink = nullptr;
        }
#endif
        if (!cleaned) fail(runtime, kErrorWorker);
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
        record_physical_destroy(runtime, runtime->physical_sink == nullptr);
#endif
        emit_summary(runtime, false);
#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
        p4_nano_audio86_physical::emit_lifecycle_test_backend_evidence();
        const bool evidence_ok = cleaned &&
            runtime->test_ready_wait == 1U &&
            runtime->pcm_forced_abort_requested.load(
                std::memory_order_acquire) == 1U &&
            runtime->first_error.load(std::memory_order_acquire) ==
                kErrorWorker &&
            runtime->test_terminal_wait == 1U &&
            runtime->test_quiescent_observed == 1U &&
            runtime->test_ack_observed == 1U &&
            runtime->test_suspended_observed == 1U &&
            runtime->test_delete_performed == 1U &&
            runtime->test_sink_destroy_performed == 1U &&
            runtime->pcm_join_timeout == 0U &&
            p4_nano_audio86_physical::lifecycle_test_evidence_valid();
        std::printf("5D1_EVIDENCE schema=2 evidence_class=ESP_EMU_EXEC scenario=start_fatal_%u start_fatal=1 ready_wait=%" PRIu32 " forced_abort=%" PRIu32 " first_error=%" PRIu32 " terminal_ack=%" PRIu32 " consumer_quiescent=%" PRIu32 " terminal_wait=%" PRIu32 " owner_suspended=%" PRIu32 " delete_performed=%" PRIu32 " sink_destroy_performed=%" PRIu32 " callback_residual=0 resource_residual=0 pa_high=0 i2c_residual=0 result=%u\n",
                    P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE,
                    runtime->test_ready_wait,
                    runtime->pcm_forced_abort_requested.load(),
                    runtime->first_error.load(), runtime->test_ack_observed,
                    runtime->test_quiescent_observed,
                    runtime->test_terminal_wait,
                    runtime->test_suspended_observed,
                    runtime->test_delete_performed,
                    runtime->test_sink_destroy_performed,
                    evidence_ok ? 1U : 0U);
        std::printf("5D1_ESP_EMU_LIFECYCLE_RESULT=%s\n",
                    evidence_ok ? "PASS" : "FAIL");
        return evidence_ok ? ESP_OK : ESP_FAIL;
#endif
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
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    const uint64_t rendered_before_join = runtime->rendered_frame_published.load(
        std::memory_order_acquire);
    const uint64_t remaining_frames = rendered_before_join < kRenderFrames
        ? kRenderFrames - rendered_before_join : 0U;
    const uint32_t worker_wait_ms = np2audio86_sustained_worker_wait_ms(
        remaining_frames);
    const TickType_t worker_wait = pdMS_TO_TICKS(worker_wait_ms) == 0U
        ? 1U : pdMS_TO_TICKS(worker_wait_ms);
#else
    const TickType_t worker_wait = kTimeout;
#endif
    const bool worker_terminal =
        xSemaphoreTake(runtime->done, worker_wait) == pdTRUE;
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
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    /* PHYSICAL_TELEMETRY_SNAPSHOT_POINT=
     * AFTER_DONE_SEMAPHORE_ACK_QUIESCENT_AND_ESUSPENDED_
     * BEFORE_TASK_DELETE_AND_SINK_DESTROY */
    const bool physical_snapshot_ready = pcm_terminal && pcm_quiescent &&
        pcm_ack && pcm_suspended;
    if (physical_snapshot_ready && runtime->physical_sink != nullptr)
        capture_physical_snapshot(runtime);
#endif

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
#if defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    if (pcm_joined && runtime->physical_sink != nullptr) {
        const bool destroyed = p4_nano_audio86_physical_sink_destroy(
                                   runtime->physical_sink) == 0;
        if (!destroyed)
            fail(runtime, kErrorWorker);
        else
            runtime->physical_sink = nullptr;
        record_physical_destroy(runtime, destroyed);
    }
#endif
#endif
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST == 1
    const bool terminal_publication_test_ok =
        runtime->terminal_test_notify_before_event ==
            runtime->terminal_test_notify_after_event &&
        runtime->terminal_test_event_before_terminal == 1U &&
        runtime->terminal_test_terminal_absent_before_release == 1U &&
        runtime->terminal_test_pre_ack_state == 1U &&
        runtime->terminal_test_worker_observed_pair == 1U &&
        runtime->terminal_test_reset_before_remainder == 1U &&
        runtime->terminal_test_retained_until_pcm_done == 1U &&
        runtime->terminal_test_q399_before_producer_continuation == 1U &&
        runtime->terminal_test_q398_accepted.load(
            std::memory_order_acquire) == 1U &&
        runtime->terminal_test_q399_ring_visible.load(
            std::memory_order_acquire) == 1U &&
        runtime->terminal_test_q399_accepted.load(
            std::memory_order_acquire) == 1U &&
        runtime->terminal_test_deadline_virtual_gap_ms < 20U &&
        runtime->terminal_test_phase.load(std::memory_order_acquire) == 3U;
#else
    const bool terminal_publication_test_ok = true;
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
    const bool common_ok = guest_ok && joined && pcm_joined && pressure_ok &&
                    terminal_publication_test_ok && !failed(runtime) &&
                    np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
                    np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
                    !np2audio86_runtime_horizon_pending(&runtime->control)
#if defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
                    && runtime->producer_clock.terminal_published_owner == 1U
                    && runtime->terminal_horizon_published.load(
                           std::memory_order_acquire) == 1U
                    && runtime->terminal_horizon_observed.load(
                           std::memory_order_acquire) == 1U
                    && runtime->terminal_pcm_ready.load(
                           std::memory_order_acquire) == 1U
                    && runtime->terminal_pcm_before_guest_done.load(
                           std::memory_order_acquire) == 1U
#endif
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
                    && runtime->pcm_ring_before_done == 1U
                    && runtime->pcm_eos_after_done == 1U
                    && runtime->pcm_finish_after_empty == 1U
                    && runtime->reset_ring_owned_frames >= 13U
                    && runtime->reset_applied_after_ring == 1U
                    && runtime->reset_ack_after_ring == 1U
#endif
                    ;
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)
    const bool physical_sink_ok = physical_s1_snapshot_healthy(runtime);
    const bool sink_profile_ok = physical_sink_ok;
#elif defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)
    const bool physical_sink_ok = physical_s2_snapshot_healthy(runtime);
    const bool sink_profile_ok = physical_sink_ok;
#elif defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    const bool physical_sink_ok = physical_5d3_s1_snapshot_healthy(runtime);
    const bool sink_profile_ok = physical_sink_ok;
#elif defined(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    uint32_t generated_crc = 0U;
    uint32_t accepted_crc = 0U;
    uint8_t generated_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t accepted_sha[NP2_SHA256_DIGEST_SIZE]{};
    np2audio86_sustained_digest_snapshot(
        &runtime->sustained.generated, &generated_crc, generated_sha);
    np2audio86_sustained_digest_snapshot(
        &runtime->sustained.accepted, &accepted_crc, accepted_sha);
    const bool scalable_identity_ok =
        runtime->sustained.generated.bytes == kRenderFrames * 4U &&
        runtime->sustained.accepted.bytes == kRenderFrames * 4U &&
        runtime->sustained.next_generated_sequence == kExpectedPcmSlots &&
        runtime->sustained.next_accepted_sequence == kExpectedPcmSlots &&
        runtime->sustained.next_generated_frame_offset == kRenderFrames &&
        runtime->sustained.next_accepted_frame_offset == kRenderFrames &&
        runtime->sustained.generated_slot_fill_frames == 0U &&
        runtime->sustained.retry_pending == 0U &&
        runtime->sustained.retry_identity_failures == 0U &&
        runtime->sustained.reset.frozen &&
        runtime->sustained.reset.frames == 95761U &&
        runtime->sustained.reset.reset_event_frame == 95761U &&
        runtime->sustained.reset.reset_event_sequence == 18U &&
        runtime->sustained.reset.applied_after_ring == 1U &&
        runtime->sustained.reset.ack_after_apply == 1U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].records == 246U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_EVENT].records == 18U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_RUN].records == 1U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_TIMER].records == 20U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_APPLY].records == 19U &&
        runtime->sustained.trace[NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE].records == 1U &&
        generated_crc == accepted_crc &&
        std::memcmp(generated_sha, accepted_sha, sizeof(generated_sha)) == 0;
    const bool virtual_sink_ok = scalable_identity_ok &&
        p4_nano_audio86_terminal_predicate::virtual_sink_scalable_observer_healthy(
            *runtime, kExpectedPcmSlots, kExpectedPartialSlots,
            kPcmPrefillSlots);
    const bool sink_profile_ok = virtual_sink_ok;
#else
    const bool virtual_sink_ok =
        p4_nano_audio86_terminal_predicate::virtual_sink_observer_healthy(
            *runtime, kExpectedPcmSlots, kExpectedPartialSlots,
            kRenderFrames < NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES
                ? 1U : kPcmPrefillSlots);
    const bool sink_profile_ok = virtual_sink_ok;
#endif
#else
    const bool sink_profile_ok = true;
#endif
    const bool normal_ok =
        p4_nano_audio86_terminal_predicate::normal_terminal_healthy(
            common_ok, sink_profile_ok);
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
    const uint64_t pcm_s2_accounted_frames =
        runtime->pcm_controller.accepted_frames +
        runtime->pcm_abandoned_published_frames +
        runtime->pcm_abandoned_partial_frames +
        runtime->pcm_abandoned_rendered_frames;
    const bool pcm_s2_common_ok = !kPcmS2Lifecycle ||
        (joined && pcm_joined &&
         runtime->pcm_lifecycle_triggered.load(std::memory_order_acquire) == 1U &&
         runtime->pcm_s2_controller_driven.load(std::memory_order_acquire) == 1U &&
         runtime->pcm_semantic_rendered_frames == pcm_s2_accounted_frames &&
         runtime->pcm_produced_bytes == runtime->pcm_produced_frames * 4U &&
         runtime->pcm_controller.accepted_bytes ==
             runtime->pcm_controller.accepted_frames * 4U &&
         np2opngen_pcm_ring_occupancy(&runtime->pcm_ring) == 0U &&
         np2opngen_pcm_ring_producer_partial_valid_frames(
             &runtime->pcm_ring) == 0U &&
         runtime->pcm_consumer_terminal_ack.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_consumer_suspended_observed.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_worker_suspended_observed.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_consumer_deleted_after_suspended.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_worker_deleted_after_suspended.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_join_timeout == 0U &&
         runtime->pcm_worker_join_timeout == 0U &&
         runtime->event_lease.load(std::memory_order_acquire) == 0U &&
         runtime->byte_lease.load(std::memory_order_acquire) == 0U &&
         runtime->horizon_lease.load(std::memory_order_acquire) == 0U &&
         runtime->reset_ack_held.load(std::memory_order_acquire) == 0U &&
         np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
         np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
         !np2audio86_runtime_horizon_pending(&runtime->control));
    const bool pcm_s2_healthy_common = pcm_s2_common_ok &&
        runtime->pcm_forced_abort_requested.load(
            std::memory_order_acquire) == 0U &&
        runtime->pcm_forced_abort == 0U &&
        runtime->pcm_sink_abort_calls == 0U &&
        runtime->pcm_ring_finished.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_production_done.load(std::memory_order_acquire) == 1U &&
        runtime->pcm_sink_finished == 1U &&
        runtime->pcm_s2_terminal_success == 1U &&
        runtime->pcm_abandoned_published_frames == 0U &&
        runtime->pcm_abandoned_partial_frames == 0U &&
        runtime->pcm_abandoned_rendered_frames == 0U &&
        runtime->pcm_semantic_rendered_frames ==
            runtime->pcm_controller.accepted_frames;
    const bool pcm_s2_forced_common = pcm_s2_common_ok &&
        runtime->pcm_forced_abort_requested.load(
            std::memory_order_acquire) == 1U &&
        runtime->pcm_forced_abort_published_before_wake.load(
            std::memory_order_acquire) == 1U &&
        runtime->pcm_forced_abort == 1U &&
        runtime->pcm_sink_abort_calls == 1U &&
        runtime->pcm_sink_finished == 0U &&
        runtime->pcm_s2_terminal_success == 0U &&
        runtime->first_error.load(std::memory_order_acquire) == kErrorWorker &&
        lifecycle_runtime != nullptr &&
        lifecycle_runtime->state() == np2runtime::State::Failed;
    const bool pcm_s2_reset_cutpoint =
        runtime->pcm_s2_cutpoint_occupancy == NP2_OPNGEN_PCM_RING_CAPACITY &&
        runtime->pcm_s2_cutpoint_partial == 0U &&
        runtime->pcm_s2_cutpoint_rendered == 1933U &&
        runtime->pcm_s2_cutpoint_unappended == 13U &&
        runtime->pcm_s2_reset_guest_linearized.load(
            std::memory_order_acquire) == 1U;
    const bool pcm_s2_partial_cutpoint =
        runtime->pcm_s2_cutpoint_occupancy == 1U &&
        runtime->pcm_s2_cutpoint_partial == 13U &&
        runtime->pcm_s2_cutpoint_rendered == 253U &&
        runtime->pcm_s2_cutpoint_unappended == 0U;
    const bool pcm_s2_ok = !kPcmS2Lifecycle ||
        ((kPcmLifecycleScenario == kPcmLifecycleResetStop ||
          kPcmLifecycleScenario == kPcmLifecycleResetFatal) &&
         pcm_s2_healthy_common && pcm_s2_reset_cutpoint &&
         runtime->pcm_semantic_rendered_frames == 1933U &&
         runtime->reset_ring_owned_frames == 1933U &&
         runtime->reset_applied_after_ring == 1U &&
         runtime->reset_ack_after_ring == 1U &&
         runtime->pcm_s2_reset_abandoned.load(
             std::memory_order_acquire) == 0U &&
         ((kPcmLifecycleScenario == kPcmLifecycleResetStop &&
           runtime->first_error.load(std::memory_order_acquire) == 0U &&
           lifecycle_runtime != nullptr &&
           lifecycle_runtime->state() == np2runtime::State::Stopped) ||
          (kPcmLifecycleScenario == kPcmLifecycleResetFatal &&
           runtime->first_error.load(std::memory_order_acquire) ==
               kErrorInjectedFatal && lifecycle_runtime != nullptr &&
           lifecycle_runtime->state() == np2runtime::State::Failed))) ||
        (kPcmLifecycleScenario == kPcmLifecycleResetConsumerFatal &&
         pcm_s2_forced_common && pcm_s2_reset_cutpoint &&
         runtime->pcm_semantic_rendered_frames == 1933U &&
         runtime->pcm_controller.accepted_frames == 0U &&
         runtime->pcm_abandoned_published_frames == 1920U &&
         runtime->pcm_abandoned_partial_frames == 0U &&
         runtime->pcm_abandoned_rendered_frames == 13U &&
         runtime->reset_applied_after_ring == 0U &&
         runtime->reset_ack_after_ring == 0U &&
         np2audio86_runtime_reset_ack(&runtime->control) == 0U &&
         runtime->pcm_s2_reset_abandoned.load(
             std::memory_order_acquire) == 1U &&
         runtime->pcm_s2_reset_event_residual_before_cleanup == 2U &&
         runtime->pcm_s2_reset_horizon_residual_before_cleanup == 0U &&
         runtime->pcm_s2_reset_transport_residual_after_cleanup == 0U) ||
        ((kPcmLifecycleScenario == kPcmLifecyclePartialStop ||
          kPcmLifecycleScenario == kPcmLifecyclePartialFatal) &&
         pcm_s2_healthy_common && pcm_s2_partial_cutpoint &&
         runtime->pcm_semantic_rendered_frames == 253U &&
         runtime->pcm_controller.accepted_frames == 253U &&
         ((kPcmLifecycleScenario == kPcmLifecyclePartialStop &&
           runtime->first_error.load(std::memory_order_acquire) == 0U &&
           lifecycle_runtime != nullptr &&
           lifecycle_runtime->state() == np2runtime::State::Stopped) ||
          (kPcmLifecycleScenario == kPcmLifecyclePartialFatal &&
           runtime->first_error.load(std::memory_order_acquire) ==
               kErrorInjectedFatal && lifecycle_runtime != nullptr &&
           lifecycle_runtime->state() == np2runtime::State::Failed))) ||
        (kPcmLifecycleScenario == kPcmLifecyclePartialConsumerFatal &&
         pcm_s2_forced_common && pcm_s2_partial_cutpoint &&
         runtime->pcm_semantic_rendered_frames == 253U &&
         runtime->pcm_controller.accepted_frames == 0U &&
         runtime->pcm_abandoned_published_frames == 240U &&
         runtime->pcm_abandoned_partial_frames == 13U &&
         runtime->pcm_abandoned_rendered_frames == 0U) ||
        (kPcmLifecycleScenario == kPcmLifecyclePostDoneConsumerFatal &&
         pcm_s2_forced_common &&
         runtime->pcm_s2_cutpoint_occupancy == 1U &&
         runtime->pcm_s2_cutpoint_partial == 0U &&
         runtime->pcm_s2_cutpoint_rendered == 2400U &&
         runtime->pcm_ring_finished.load(std::memory_order_acquire) == 1U &&
         runtime->pcm_production_done.load(std::memory_order_acquire) == 1U &&
         runtime->pcm_controller.accepted_frames == 2160U &&
         runtime->pcm_abandoned_published_frames == 240U &&
         runtime->pcm_abandoned_partial_frames == 0U &&
         runtime->pcm_abandoned_rendered_frames == 0U) ||
        (kPcmLifecycleScenario == kPcmLifecycleFinishFatal &&
         pcm_s2_forced_common &&
         runtime->pcm_s2_cutpoint_occupancy == 0U &&
         runtime->pcm_s2_cutpoint_partial == 0U &&
         runtime->pcm_s2_cutpoint_rendered == 2400U &&
         runtime->pcm_controller.accepted_frames == 2400U &&
         runtime->pcm_abandoned_published_frames == 0U &&
         runtime->pcm_abandoned_partial_frames == 0U &&
         runtime->pcm_abandoned_rendered_frames == 0U &&
         runtime->pcm_s2_finish_calls == 1U &&
         runtime->pcm_s2_finish_fatal_observed == 1U &&
         runtime->pcm_ack_after_finish == 0U &&
         runtime->pcm_controller.state == NP2_PCM_OUTPUT_ABORTED);
    const bool pcm_lifecycle_failure =
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureFull ||
        kPcmLifecycleScenario == kPcmLifecycleConsumerFailureEmpty ||
        kPcmLifecycleScenario == kPcmLifecycleRetryPrimaryFirst ||
        kPcmLifecycleScenario == kPcmLifecycleRetryConsumerFirst;
#else
    const bool retry_healthy_ok = true;
#endif
#if defined(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    const bool ok = kPcmS2Lifecycle ? pcm_s2_ok :
        (pcm_lifecycle_failure ? forced_abort_ok :
        (kFailureKind == kFailureNone ? normal_ok :
         (failure_ok && byte_extend_failure_ok && retry_healthy_ok)));
#else
    const bool ok = kFailureKind == kFailureNone ? normal_ok :
        (failure_ok && byte_extend_failure_ok && retry_healthy_ok);
#endif
    if (kPressureScenario != kPressureNone && pressure_ok)
        runtime->pressure_phase.store(6U, std::memory_order_release); /* COMPLETE */
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST != 0
    const bool terminal_test_success =
#if P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST == 1
        ok && terminal_publication_test_ok;
#else
        !ok &&
        runtime->terminal_test_partial_failure_event_visible == 1U &&
        runtime->terminal_test_partial_failure_wake_issued == 1U &&
        runtime->terminal_test_phase.load(std::memory_order_acquire) == 4U &&
        runtime->first_error.load(std::memory_order_acquire) ==
            kErrorTransport &&
        !runtime->transaction_active && !runtime->horizon_owned &&
        !runtime->terminal_horizon_armed &&
        !runtime->terminal_reset_notify_deferred &&
        runtime->terminal_reset_transaction_ordinal == 0U &&
        runtime->terminal_reset_publication_failed_ordinal.load(
            std::memory_order_acquire) == 0U &&
        np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
        np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
        !np2audio86_runtime_horizon_pending(&runtime->control) &&
        runtime->producer_done.load(std::memory_order_acquire) == 1U;
#endif
    std::printf(
        "P4_AUDIO86_TERMINAL_PUBLICATION_TEST mode=%u"
        " actual_path=HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER"
        " hold_ack=%" PRIu32 " event_visible=%" PRIu32
        " terminal_absent_before_release=%" PRIu32
        " notify_before_event=%" PRIu32 " notify_after_event=%" PRIu32
        " pre_ack_pair=%" PRIu32 " worker_pair=%" PRIu32
        " reset_before_remainder=%" PRIu32 " retained=%" PRIu32
        " q399_before_continuation=%" PRIu32
        " q398_accepted=%" PRIu32 " q399_visible=%" PRIu32
        " q399_accepted=%" PRIu32 " virtual_gap_ms=%" PRIu32
        " service_horizon_ms=20 partial_event=%" PRIu32
        " partial_wake=%" PRIu32 " producer_done=%" PRIu32
        " transport_residual=%" PRIu32 " first_error=%" PRIu32
        " result=%s\n",
        P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST,
        runtime->terminal_test_worker_hold_ack.load(),
        runtime->terminal_test_event_before_terminal,
        runtime->terminal_test_terminal_absent_before_release,
        runtime->terminal_test_notify_before_event,
        runtime->terminal_test_notify_after_event,
        runtime->terminal_test_pre_ack_state,
        runtime->terminal_test_worker_observed_pair,
        runtime->terminal_test_reset_before_remainder,
        runtime->terminal_test_retained_until_pcm_done,
        runtime->terminal_test_q399_before_producer_continuation,
        runtime->terminal_test_q398_accepted.load(),
        runtime->terminal_test_q399_ring_visible.load(),
        runtime->terminal_test_q399_accepted.load(),
        runtime->terminal_test_deadline_virtual_gap_ms,
        runtime->terminal_test_partial_failure_event_visible,
        runtime->terminal_test_partial_failure_wake_issued,
        runtime->producer_done.load(),
        np2audio86_event_ring_occupancy(&runtime->events) +
            np2audio86_byte_ring_occupancy(&runtime->bytes) +
            (np2audio86_runtime_horizon_pending(&runtime->control) ? 1U : 0U),
        runtime->first_error.load(),
        terminal_test_success ? "PASS" : "FAIL");
#endif
    emit_summary(runtime, ok);
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio86_guest_binding
