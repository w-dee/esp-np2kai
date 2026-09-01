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
#include "np2runtime/np2runtime.hpp"
#include "p4_nano_audio86_notifications/task_notification.hpp"

namespace p4_nano_audio86_guest_binding {
namespace {

constexpr BaseType_t kWorkerCore = 0;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 6U;
constexpr uint32_t kWorkerStackBytes = 8192U;
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
constexpr size_t kRenderFrames = 2400U;
constexpr size_t kApplyRecordBytes = 40U;
#ifndef P4_NANO_AUDIO86_PRESSURE_SCENARIO
#define P4_NANO_AUDIO86_PRESSURE_SCENARIO 0
#endif
#ifndef P4_NANO_AUDIO86_FAILURE_KIND
#define P4_NANO_AUDIO86_FAILURE_KIND 0
#endif
enum : uint32_t { kPressureNone = 0U, kPressureEvent = 1U,
                  kPressureByte = 2U, kPressureHorizon = 3U,
                  kPressureResetAck = 4U };
constexpr uint32_t kPressureScenario = P4_NANO_AUDIO86_PRESSURE_SCENARIO;
enum : uint32_t { kFailureNone = 0U, kFailureStop = 1U, kFailureFatal = 2U };
constexpr uint32_t kFailureKind = P4_NANO_AUDIO86_FAILURE_KIND;
constexpr uint32_t kErrorInjectedFatal = 86U;

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

int reserve_checked(void *opaque, const uint32_t kind, const size_t bytes,
                    np2audio86_guest_transaction_t *token)
{
    auto *runtime = static_cast<Runtime *>(opaque);
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
    if (runtime == nullptr || !token_matches(runtime, token,
                                               NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        bytes != 1U || runtime->run_committed) {
        if (runtime != nullptr) fail(runtime, kErrorTransport);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    for (;;) {
        if (failed(runtime)) return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
        if (np2audio86_byte_ring_occupancy(&runtime->bytes) + runtime->reserved_bytes <
            NP2_AUDIO86_ASYNC_BYTE_CAPACITY) break;
        runtime->producer_waiting.store(1U, std::memory_order_release);
        (void)p4_nano_audio86_notifications::wait_producer();
        runtime->producer_waiting.store(0U, std::memory_order_release);
    }
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
            } else if (runtime->pressure_phase.load(std::memory_order_acquire) == 3U) {
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
    if (ok && kFailureKind == kFailureNone) emit_exact_evidence(runtime);
    if (kPressureScenario != kPressureNone && kFailureKind == kFailureNone) {
        const char *const name = kPressureScenario == kPressureEvent ? "EVENT" :
            kPressureScenario == kPressureByte ? "BYTE" :
            kPressureScenario == kPressureHorizon ? "HORIZON" : "RESET_ACK";
        const char *const cause = kPressureScenario == kPressureEvent ? "EVENT_CAPACITY_ONLY" :
            kPressureScenario == kPressureByte ? "BYTE_CAPACITY_ONLY" :
            kPressureScenario == kPressureHorizon ? "HORIZON_ONLY" : "POSTCOMMIT_ACK";
        const char *const target = kPressureScenario == kPressureEvent ? "EVENT_SEQUENCE_0" :
            kPressureScenario == kPressureByte ? "DATA_RUN_SEQUENCE_16" :
            kPressureScenario == kPressureHorizon ? "HORIZON_EVENT_SEQUENCE_0" : "RESET_ORDINAL_1";
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
            kPressureScenario == kPressureHorizon ? "HORIZON" : "RESET_ACK";
        const np2runtime::State state = runtime->lifecycle_runtime == nullptr
            ? np2runtime::State::Created : runtime->lifecycle_runtime->state();
        const char *const final_state = state == np2runtime::State::Stopped ? "Stopped" :
            state == np2runtime::State::Failed ? "Failed" : "NOT_TERMINAL";
        std::printf("P4_AUDIO86_FAILURE kind=%s wait=%s reason=%" PRIu32
                    " producer_waiting=1 predicate_published=%" PRIu32
                    " producer_wake_index=%" PRIu32 " worker_wake_index=%" PRIu32
                    " order=%" PRIu32 " lifecycle=%s first_error=%" PRIu32
                    " later_guest_instructions=%" PRIu32 "\n",
                    kind, wait, kFailureKind == kFailureFatal ? kErrorInjectedFatal : 0U,
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
    }
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
    runtime->pressure_ip_before = runtime->pressure_ip_after = 0U;
    runtime->pressure_position_before = runtime->pressure_position_after = 0U;
    runtime->pressure_snapshot_before = runtime->pressure_snapshot_after = 0U;
    runtime->pressure_phase.store(kPressureScenario == kPressureNone ? 0U : 1U,
                                  std::memory_order_release); /* ARMED */
    runtime->generation++;
    np2audio86_event_ring_init(&runtime->events);
    np2audio86_byte_ring_init(&runtime->bytes);
    np2audio86_runtime_control_init(&runtime->control);
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
    np2audio86_guest_sink_unbind();
    np2audio86_guest_host_trace_detach();
    board86_unbind();
    runtime->producer_done.store(1U, std::memory_order_release);
    np2audio86_runtime_producer_done_publish(&runtime->control);
    notify_worker(runtime);
    const bool joined = xSemaphoreTake(runtime->done, kTimeout) == pdTRUE &&
                        runtime->worker_quiescent.load(std::memory_order_acquire) != 0U;
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
    const bool normal_ok = guest_ok && joined && pressure_ok && !failed(runtime) &&
                    np2audio86_event_ring_occupancy(&runtime->events) == 0U &&
                    np2audio86_byte_ring_occupancy(&runtime->bytes) == 0U &&
                    !np2audio86_runtime_horizon_pending(&runtime->control);
    if (kFailureKind != kFailureNone && lifecycle_runtime != nullptr)
        (void)lifecycle_runtime->run();
    runtime->failure_first_error_after_cleanup.store(
        runtime->first_error.load(std::memory_order_acquire), std::memory_order_release);
    const bool failure_ok = kFailureKind != kFailureNone && joined &&
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
        ((kFailureKind == kFailureStop &&
          lifecycle_runtime->state() == np2runtime::State::Stopped &&
          runtime->first_error.load(std::memory_order_acquire) == 0U) ||
         (kFailureKind == kFailureFatal &&
          lifecycle_runtime->state() == np2runtime::State::Failed &&
          runtime->first_error.load(std::memory_order_acquire) == kErrorInjectedFatal &&
          runtime->failure_first_error_after_cleanup.load(std::memory_order_acquire) == kErrorInjectedFatal));
    const bool ok = kFailureKind == kFailureNone ? normal_ok : failure_ok;
    if (kPressureScenario != kPressureNone && pressure_ok)
        runtime->pressure_phase.store(6U, std::memory_order_release); /* COMPLETE */
    emit_summary(runtime, ok);
    if (joined && runtime->worker != nullptr) { vTaskDelete(runtime->worker); runtime->worker = nullptr; }
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio86_guest_binding
