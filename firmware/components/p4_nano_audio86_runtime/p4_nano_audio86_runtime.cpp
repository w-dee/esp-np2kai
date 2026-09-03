#include "p4_nano_audio86_runtime/p4_nano_audio86_runtime.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "np2_sha256.h"
#include "np2audio86_fixture.h"
#include "np2audio86_runtime_transport.h"
#include "p4_nano_audio86_notifications/task_notification.hpp"

namespace p4_nano_audio86_runtime {
namespace {

constexpr BaseType_t kProducerCore = 1;
constexpr BaseType_t kWorkerCore = 0;
constexpr UBaseType_t kProducerPriority = tskIDLE_PRIORITY + 3U;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 6U;
constexpr uint32_t kProducerStackBytes = 6144U;
constexpr uint32_t kWorkerStackBytes = 8192U;
constexpr TickType_t kTimeout = pdMS_TO_TICKS(5000U);
constexpr uint32_t kErrorProducer = 1U;
constexpr uint32_t kErrorWorker = 2U;
constexpr uint32_t kErrorHorizon = 3U;
constexpr uint8_t kDataRun[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
constexpr uint8_t kDataRunSha256[NP2_SHA256_DIGEST_SIZE] = {
    0xbe, 0x45, 0xcb, 0x26, 0x05, 0xbf, 0x36, 0xbe,
    0xbd, 0xe6, 0x84, 0x84, 0x1a, 0x28, 0xf0, 0xfd,
    0x43, 0xc6, 0x98, 0x50, 0xa3, 0xdc, 0xe5, 0xfe,
    0xdb, 0xa6, 0x99, 0x28, 0xee, 0x3a, 0x89, 0x91,
};

enum class Scenario : uint32_t {
    Normal,
    FatalWorkerWait,
    FatalProducerWait,
    StopEventWait,
    EventWaitNormal,
    StopResetWait,
    FatalResetWait,
    StopWorkerWait,
    CrossIndexIsolation,
    ByteWakeBeforeRecheck,
    ByteSpuriousThenNormal,
    ByteWaitStop,
    ByteWaitFatal,
    HorizonWakeBeforeRecheck,
    HorizonSpuriousThenNormal,
    HorizonWaitStop,
    HorizonWaitFatal,
    TransportBeforeHorizon,
    WorkerOnlyPartialCreate,
    ProducerOnlyPartialCreate,
    ProducerReadyTimeout,
    WorkerReadyTimeout,
    CompletionRecheckEventOnly,
    CompletionRecheckHorizonOnly,
    CompletionRecheckCombined,
    CompletionRecheckEmpty,
    CompletionRecheckStop,
    CompletionRecheckProducerFatal,
    CompletionRecheckWorkerFatal,
    CompletionRecheckReset,
};

struct Runtime {
    struct np2audio86_event_ring events{};
    struct np2audio86_byte_ring bytes{};
    struct np2audio86_runtime_control control{};
    struct np2audio86_runtime_producer_clock producer_clock{};
    struct np2audio86_runtime_consumer_clock consumer_clock{};
    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN]{};
    StaticTask_t producer_tcb{};
    StaticTask_t worker_tcb{};
    StackType_t producer_stack[kProducerStackBytes / sizeof(StackType_t)]{};
    StackType_t worker_stack[kWorkerStackBytes / sizeof(StackType_t)]{};
    StaticSemaphore_t ready_storage{};
    StaticSemaphore_t start_storage{};
    StaticSemaphore_t terminal_storage{};
    StaticSemaphore_t injection_storage{};
    StaticSemaphore_t resume_storage{};
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t start = nullptr;
    SemaphoreHandle_t terminal = nullptr;
    SemaphoreHandle_t injection = nullptr;
    SemaphoreHandle_t resume = nullptr;
    TaskHandle_t producer_task = nullptr;
    TaskHandle_t worker_task = nullptr;
    std::atomic<uint32_t> producer_waiting{0U};
    std::atomic<uint32_t> worker_waiting{0U};
    std::atomic<uint32_t> producer_core{UINT32_MAX};
    std::atomic<uint32_t> worker_core{UINT32_MAX};
    std::atomic<uint32_t> worker_priority{0U};
    std::atomic<uint32_t> byte_wait_attempts{0U};
    std::atomic<uint32_t> horizon_wait_attempts{0U};
    std::atomic<uint32_t> producer_quiescent{0U};
    std::atomic<uint32_t> worker_quiescent{0U};
    std::atomic<uint32_t> transport_pause_verified{0U};
    uint32_t generation = 0U;
    Scenario scenario = Scenario::Normal;
    bool consumer_has_horizon = false;
    uint64_t rendered_frame = 0U;
    uint64_t next_sequence = 0U;
    uint32_t action_count = 0U;
    uint32_t data_bytes = 0U;
    uint32_t reset_count = 0U;
    uint32_t last_reset_ordinal = 0U;
    uint32_t producer_wakes = 0U;
    uint32_t worker_wakes = 0U;
    uint8_t data_sha[NP2_SHA256_DIGEST_SIZE]{};
    np2_sha256_context data_sha_context{};
};

static_assert(sizeof(struct np2audio86_event) == 24U);
static_assert(sizeof(struct np2audio86_event_ring) == 3080U);
static_assert(sizeof(struct np2audio86_byte_ring) == 65544U);
static_assert(sizeof(struct np2audio86_runtime_control) == 36U);
static_assert(sizeof(struct np2audio86_runtime_horizon_mailbox) == 20U);
static_assert(alignof(struct np2audio86_runtime_control) >= 4U);
static_assert(sizeof(((Runtime *)nullptr)->producer_stack) ==
              kProducerStackBytes);
static_assert(sizeof(((Runtime *)nullptr)->worker_stack) == kWorkerStackBytes);
DRAM_ATTR Runtime s_runtime{};
static_assert(sizeof(StaticTask_t) == 344U);
static_assert(sizeof(Runtime) == 117168U);
uint32_t s_next_generation = 0U;
uint32_t s_last_deleted_generation = 0U;
bool s_storage_reusable = true;
bool s_stale_audio_index1_seeded = false;
bool s_recreated_audio_notification_clean = true;

void notify_producer(Runtime *runtime)
{
    if (runtime->producer_task != nullptr) {
        (void)p4_nano_audio86_notifications::notify_producer(
            runtime->producer_task);
    }
}

void notify_worker(Runtime *runtime)
{
    if (runtime->worker_task != nullptr) {
        (void)p4_nano_audio86_notifications::notify_worker(
            runtime->worker_task);
    }
}

void wake_all(Runtime *runtime)
{
    notify_producer(runtime);
    notify_worker(runtime);
}

bool publish_error(Runtime *runtime, uint32_t error)
{
    const bool first = np2audio86_runtime_first_error_publish(
        &runtime->control, error);
    if (first) {
        wake_all(runtime);
    }
    return first;
}

void publish_stop(Runtime *runtime)
{
    np2audio86_runtime_stop_publish(&runtime->control);
    wake_all(runtime);
}

bool abort_requested(Runtime *runtime)
{
    return np2audio86_runtime_stop_requested(&runtime->control) ||
           np2audio86_runtime_first_error(&runtime->control) != 0U;
}

void task_quiescent_epilogue(Runtime *runtime,
                             std::atomic<uint32_t> *quiescent)
{
    /* After this release the task touches only its terminal semaphore and
     * scheduler state, then self-suspends. */
    quiescent->store(runtime->generation, std::memory_order_release);
    xSemaphoreGive(runtime->terminal);
    vTaskSuspend(nullptr);
}

bool publish_horizon_wait(Runtime *runtime, uint64_t frame)
{
    uint32_t attempt = 0U;
    for (;;) {
        if (abort_requested(runtime)) {
            return false;
        }
        const int result = np2audio86_runtime_horizon_publish(
            &runtime->control, &runtime->producer_clock, frame);
        if (result == NP2_AUDIO86_RUNTIME_HORIZON_OK) {
            notify_worker(runtime);
            return true;
        }
        if (result != NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
            publish_error(runtime, kErrorHorizon);
            return false;
        }
        ++attempt;
        runtime->horizon_wait_attempts.store(attempt,
                                             std::memory_order_release);
        runtime->producer_waiting.store(1U, std::memory_order_release);
        if ((runtime->scenario == Scenario::HorizonWakeBeforeRecheck ||
             runtime->scenario == Scenario::HorizonSpuriousThenNormal) &&
            attempt == 1U) {
            xSemaphoreGive(runtime->injection);
            xSemaphoreTake(runtime->resume, portMAX_DELAY);
        } else if (runtime->scenario == Scenario::HorizonSpuriousThenNormal &&
                   attempt == 2U) {
            xSemaphoreGive(runtime->injection);
        } else if ((runtime->scenario == Scenario::HorizonWaitStop ||
                    runtime->scenario == Scenario::HorizonWaitFatal) &&
                   attempt == 1U) {
            xSemaphoreGive(runtime->injection);
        }
        if (!np2audio86_runtime_horizon_pending(&runtime->control)) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            continue;
        }
        if (abort_requested(runtime)) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            return false;
        }
        p4_nano_audio86_notifications::wait_producer();
        ++runtime->producer_wakes;
        runtime->producer_waiting.store(0U, std::memory_order_release);
    }
}

bool enqueue_event_wait(Runtime *runtime,
                        const struct np2audio86_event *event)
{
    for (;;) {
        if (abort_requested(runtime)) {
            return false;
        }
        const int result = np2audio86_event_ring_enqueue(&runtime->events,
                                                         event);
        if (result == NP2_AUDIO86_TRANSPORT_OK) {
            notify_worker(runtime);
            return true;
        }
        if (result != NP2_AUDIO86_TRANSPORT_FULL) {
            publish_error(runtime, kErrorProducer);
            return false;
        }
        runtime->producer_waiting.store(1U, std::memory_order_release);
        if (np2audio86_event_ring_occupancy(&runtime->events) <
            NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            continue;
        }
        if (runtime->scenario == Scenario::FatalProducerWait ||
            runtime->scenario == Scenario::StopEventWait ||
            runtime->scenario == Scenario::EventWaitNormal) {
            xSemaphoreGive(runtime->injection);
        }
        p4_nano_audio86_notifications::wait_producer();
        ++runtime->producer_wakes;
        runtime->producer_waiting.store(0U, std::memory_order_release);
    }
}

bool byte_capacity_available(Runtime *runtime, size_t count, bool *available)
{
    const uint32_t occupancy =
        np2audio86_byte_ring_occupancy(&runtime->bytes);
    if (available == nullptr || count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY ||
        occupancy > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        publish_error(runtime, kErrorProducer);
        return false;
    }
    *available = count <= NP2_AUDIO86_ASYNC_BYTE_CAPACITY - occupancy;
    return true;
}

bool enqueue_bytes_wait(Runtime *runtime, const uint8_t *bytes, size_t count)
{
    uint32_t attempt = 0U;
    for (;;) {
        bool available = false;
        if (abort_requested(runtime)) {
            return false;
        }
        const int result = np2audio86_byte_ring_push(&runtime->bytes, bytes,
                                                     count);
        if (result == NP2_AUDIO86_TRANSPORT_OK) {
            return true;
        }
        if (result != NP2_AUDIO86_TRANSPORT_FULL) {
            publish_error(runtime, kErrorProducer);
            return false;
        }
        ++attempt;
        runtime->byte_wait_attempts.store(attempt, std::memory_order_release);
        runtime->producer_waiting.store(1U, std::memory_order_release);

        if ((runtime->scenario == Scenario::ByteWakeBeforeRecheck ||
             runtime->scenario == Scenario::ByteSpuriousThenNormal) &&
            attempt == 1U) {
            xSemaphoreGive(runtime->injection);
            xSemaphoreTake(runtime->resume, portMAX_DELAY);
        } else if (runtime->scenario == Scenario::ByteSpuriousThenNormal &&
                   attempt == 2U) {
            xSemaphoreGive(runtime->injection);
        } else if ((runtime->scenario == Scenario::ByteWaitStop ||
                    runtime->scenario == Scenario::ByteWaitFatal) &&
                   attempt == 1U) {
            xSemaphoreGive(runtime->injection);
        }

        if (!byte_capacity_available(runtime, count, &available)) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            return false;
        }
        if (available) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            continue;
        }
        if (abort_requested(runtime)) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            return false;
        }
        p4_nano_audio86_notifications::wait_producer();
        ++runtime->producer_wakes;
        runtime->producer_waiting.store(0U, std::memory_order_release);
    }
}

bool wait_reset_ack(Runtime *runtime, uint32_t ordinal)
{
    for (;;) {
        if (np2audio86_runtime_reset_ack(&runtime->control) == ordinal) {
            return true;
        }
        if (abort_requested(runtime)) {
            return false;
        }
        runtime->producer_waiting.store(1U, std::memory_order_release);
        if (np2audio86_runtime_reset_ack(&runtime->control) == ordinal) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            return true;
        }
        if (abort_requested(runtime)) {
            runtime->producer_waiting.store(0U, std::memory_order_release);
            return false;
        }
        if (runtime->scenario == Scenario::StopResetWait ||
            runtime->scenario == Scenario::FatalResetWait) {
            xSemaphoreGive(runtime->injection);
        }
        p4_nano_audio86_notifications::wait_producer();
        ++runtime->producer_wakes;
        runtime->producer_waiting.store(0U, std::memory_order_release);
    }
}

bool publish_event(Runtime *runtime, uint64_t frame, uint32_t opcode,
                   uint32_t payload)
{
    struct np2audio86_event event{
        frame, runtime->next_sequence, opcode, payload};
    if (!enqueue_event_wait(runtime, &event)) {
        return false;
    }
    ++runtime->next_sequence;
    return true;
}

bool publish_normal_stream(Runtime *runtime)
{
    if (!publish_event(runtime, 0U, NP2_AUDIO86_EVENT_FM_KEY, 0x28U)) {
        return false;
    }
    if (!enqueue_bytes_wait(runtime, kDataRun, sizeof(kDataRun))) {
        return false;
    }
    if (!publish_event(runtime, 8U, NP2_AUDIO86_EVENT_PCM86_DATA_RUN,
                       sizeof(kDataRun)) ||
        !publish_event(runtime, 16U, NP2_AUDIO86_EVENT_RESET_BARRIER, 1U)) {
        return false;
    }
    /* All slot and DATA_RUN release publications precede this release store. */
    if (!publish_horizon_wait(runtime, 16U)) {
        return false;
    }
    if (!wait_reset_ack(runtime, 1U)) {
        return false;
    }
    if (!publish_event(runtime, 16U, NP2_AUDIO86_EVENT_PSG_REGISTER, 7U) ||
        !publish_horizon_wait(runtime, 16U)) {
        publish_error(runtime, kErrorProducer);
        return false;
    }
    return true;
}

bool fill_byte_ring(Runtime *runtime)
{
    std::memset(runtime->worker_run, 0xa5, sizeof(runtime->worker_run));
    return np2audio86_byte_ring_push(&runtime->bytes, runtime->worker_run,
                                     sizeof(runtime->worker_run)) ==
               NP2_AUDIO86_TRANSPORT_OK &&
           np2audio86_byte_ring_push(&runtime->bytes, runtime->worker_run,
                                     sizeof(runtime->worker_run)) ==
               NP2_AUDIO86_TRANSPORT_OK;
}

void publish_event_pressure(Runtime *runtime)
{
    for (uint32_t index = 0U;
         index < NP2_AUDIO86_ASYNC_EVENT_CAPACITY + 1U; ++index) {
        if (!publish_event(runtime, 0U, NP2_AUDIO86_EVENT_FM_KEY, index)) {
            break;
        }
    }
}

bool is_completion_recheck_scenario(Scenario scenario)
{
    return scenario == Scenario::CompletionRecheckEventOnly ||
           scenario == Scenario::CompletionRecheckHorizonOnly ||
           scenario == Scenario::CompletionRecheckCombined ||
           scenario == Scenario::CompletionRecheckEmpty ||
           scenario == Scenario::CompletionRecheckStop ||
           scenario == Scenario::CompletionRecheckProducerFatal ||
           scenario == Scenario::CompletionRecheckWorkerFatal ||
           scenario == Scenario::CompletionRecheckReset;
}

bool completion_publish_direct(Runtime *runtime, bool event, bool horizon,
                               bool reset)
{
    const uint64_t frame = (reset || !horizon) ? 0U : 8U;
    if (event) {
        const struct np2audio86_event final_event{
            frame, runtime->next_sequence,
            reset ? NP2_AUDIO86_EVENT_RESET_BARRIER
                  : NP2_AUDIO86_EVENT_PSG_REGISTER,
            reset ? 1U : 7U};
        if (np2audio86_event_ring_enqueue(&runtime->events, &final_event) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            return false;
        }
        ++runtime->next_sequence;
    }
    if (horizon && np2audio86_runtime_horizon_publish(
                       &runtime->control, &runtime->producer_clock, frame) !=
                       NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        return false;
    }
    return true;
}

void producer_completion_recheck(Runtime *runtime)
{
    xSemaphoreTake(runtime->injection, portMAX_DELAY);
    const bool event =
        runtime->scenario != Scenario::CompletionRecheckHorizonOnly &&
        runtime->scenario != Scenario::CompletionRecheckEmpty;
    const bool horizon =
        runtime->scenario != Scenario::CompletionRecheckEventOnly &&
        runtime->scenario != Scenario::CompletionRecheckEmpty;
    const bool reset = runtime->scenario == Scenario::CompletionRecheckReset;
    if (!completion_publish_direct(runtime, event, horizon, reset)) {
        publish_error(runtime, kErrorProducer);
    }
    /* No task notification follows this release.  The test-only semaphore
     * rendezvous resumes the worker, which must converge from level state. */
    np2audio86_runtime_producer_done_publish(&runtime->control);
    if (runtime->scenario == Scenario::CompletionRecheckStop) {
        publish_stop(runtime);
    } else if (runtime->scenario ==
               Scenario::CompletionRecheckProducerFatal) {
        publish_error(runtime, kErrorProducer);
    }
    xSemaphoreGive(runtime->resume);
}

void producer_task(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    runtime->producer_core.store(static_cast<uint32_t>(xPortGetCoreID()),
                                 std::memory_order_release);
    if (runtime->scenario != Scenario::ProducerReadyTimeout) {
        xSemaphoreGive(runtime->ready);
    }
    xSemaphoreTake(runtime->start, portMAX_DELAY);

    if (abort_requested(runtime)) {
        task_quiescent_epilogue(runtime, &runtime->producer_quiescent);
        return;
    }

    switch (runtime->scenario) {
    case Scenario::Normal:
        if (s_stale_audio_index1_seeded &&
            p4_nano_audio86_notifications::wait_producer(0U) != 0U) {
            s_recreated_audio_notification_clean = false;
            publish_error(runtime, kErrorProducer);
            break;
        }
        (void)publish_normal_stream(runtime);
        np2audio86_runtime_producer_done_publish(&runtime->control);
        notify_worker(runtime);
        break;
    case Scenario::FatalWorkerWait:
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorProducer);
        break;
    case Scenario::FatalProducerWait: {
        publish_event_pressure(runtime);
        /* Exercise first-wins after the worker's injected fatal publication. */
        (void)publish_error(runtime, kErrorProducer);
        break;
    }
    case Scenario::StopEventWait:
    case Scenario::EventWaitNormal:
        publish_event_pressure(runtime);
        break;
    case Scenario::StopResetWait:
    case Scenario::FatalResetWait:
        if (publish_event(runtime, 0U, NP2_AUDIO86_EVENT_RESET_BARRIER, 1U) &&
            publish_horizon_wait(runtime, 0U)) {
            (void)wait_reset_ack(runtime, 1U);
        }
        break;
    case Scenario::StopWorkerWait:
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
        break;
    case Scenario::CrossIndexIsolation:
        runtime->producer_waiting.store(1U, std::memory_order_release);
        xSemaphoreGive(runtime->injection);
        xSemaphoreTake(runtime->resume, portMAX_DELAY);
        (void)p4_nano_audio86_notifications::wait_producer();
        ++runtime->producer_wakes;
        runtime->producer_waiting.store(0U, std::memory_order_release);
        xSemaphoreGive(runtime->injection);
        np2audio86_runtime_producer_done_publish(&runtime->control);
        notify_worker(runtime);
        break;
    case Scenario::ByteWakeBeforeRecheck:
    case Scenario::ByteSpuriousThenNormal:
    case Scenario::ByteWaitStop:
    case Scenario::ByteWaitFatal:
        if (!fill_byte_ring(runtime)) {
            publish_error(runtime, kErrorProducer);
        } else {
            (void)enqueue_bytes_wait(runtime, kDataRun, sizeof(kDataRun));
        }
        break;
    case Scenario::HorizonWakeBeforeRecheck:
    case Scenario::HorizonSpuriousThenNormal:
    case Scenario::HorizonWaitStop:
    case Scenario::HorizonWaitFatal:
        if (np2audio86_runtime_horizon_publish(
                &runtime->control, &runtime->producer_clock, 0U) !=
                NP2_AUDIO86_RUNTIME_HORIZON_OK ||
            !publish_horizon_wait(runtime, 0U)) {
            if (!abort_requested(runtime)) {
                publish_error(runtime, kErrorHorizon);
            }
        } else {
            np2audio86_runtime_producer_done_publish(&runtime->control);
            notify_worker(runtime);
        }
        break;
    case Scenario::TransportBeforeHorizon:
        if (publish_event(runtime, 0U, NP2_AUDIO86_EVENT_FM_KEY, 0x28U)) {
            xSemaphoreGive(runtime->injection);
            xSemaphoreTake(runtime->resume, portMAX_DELAY);
            if (publish_horizon_wait(runtime, 0U)) {
                np2audio86_runtime_producer_done_publish(&runtime->control);
                notify_worker(runtime);
            }
        }
        break;
    case Scenario::CompletionRecheckEventOnly:
    case Scenario::CompletionRecheckHorizonOnly:
    case Scenario::CompletionRecheckCombined:
    case Scenario::CompletionRecheckEmpty:
    case Scenario::CompletionRecheckStop:
    case Scenario::CompletionRecheckProducerFatal:
    case Scenario::CompletionRecheckWorkerFatal:
    case Scenario::CompletionRecheckReset:
        producer_completion_recheck(runtime);
        break;
    case Scenario::WorkerOnlyPartialCreate:
    case Scenario::ProducerOnlyPartialCreate:
    case Scenario::ProducerReadyTimeout:
    case Scenario::WorkerReadyTimeout:
        break;
    }
    task_quiescent_epilogue(runtime, &runtime->producer_quiescent);
}

enum class WorkerDecision { Work, Wait, Finish, Abort };

WorkerDecision worker_after_done(Runtime *runtime,
                                 const struct np2audio86_event **event)
{
    /* producer_done was acquire-observed by the caller.  Do not reuse any
     * publication state sampled before that acquire. */
    if (abort_requested(runtime)) {
        return WorkerDecision::Abort;
    }

    const int peek = np2audio86_event_ring_peek(&runtime->events, event);
    const int horizon = np2audio86_runtime_horizon_try_observe(
        &runtime->control, &runtime->consumer_clock);
    if (horizon != NP2_AUDIO86_RUNTIME_HORIZON_OK &&
        horizon != NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
        publish_error(runtime, kErrorHorizon);
        return WorkerDecision::Abort;
    }
    if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        runtime->consumer_has_horizon = true;
        notify_producer(runtime);
    }
    if (peek == NP2_AUDIO86_TRANSPORT_OK) {
        if ((*event)->frame_timestamp <=
            runtime->consumer_clock.committed_frame_reconstructed) {
            return WorkerDecision::Work;
        }
        /* done orders all horizon publication before this fresh acquire, so
         * a still-uncommitted event is a terminal protocol violation. */
        publish_error(runtime, kErrorWorker);
        return WorkerDecision::Abort;
    }
    if (peek != NP2_AUDIO86_TRANSPORT_EMPTY) {
        publish_error(runtime, kErrorWorker);
        return WorkerDecision::Abort;
    }
    if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_OK ||
        runtime->rendered_frame <
            runtime->consumer_clock.committed_frame_reconstructed) {
        runtime->rendered_frame =
            runtime->consumer_clock.committed_frame_reconstructed;
        return WorkerDecision::Wait;
    }

    const bool reset_ack_complete =
        runtime->reset_count == 0U ||
        np2audio86_runtime_reset_ack(&runtime->control) ==
            runtime->last_reset_ordinal;
    if (abort_requested(runtime) ||
        np2audio86_byte_ring_occupancy(&runtime->bytes) != 0U ||
        np2audio86_runtime_horizon_pending(&runtime->control) ||
        !reset_ack_complete) {
        publish_error(runtime, kErrorWorker);
        return WorkerDecision::Abort;
    }
    return WorkerDecision::Finish;
}

WorkerDecision worker_decision(Runtime *runtime,
                               const struct np2audio86_event **event)
{
    if (abort_requested(runtime)) {
        return WorkerDecision::Abort;
    }
    if (np2audio86_runtime_producer_done(&runtime->control)) {
        return worker_after_done(runtime, event);
    }
    const int horizon = np2audio86_runtime_horizon_try_observe(
        &runtime->control, &runtime->consumer_clock);
    if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
        if (!runtime->consumer_has_horizon) {
            return WorkerDecision::Wait;
        }
    } else if (horizon != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        publish_error(runtime, kErrorHorizon);
        return WorkerDecision::Abort;
    } else {
        runtime->consumer_has_horizon = true;
        notify_producer(runtime);
    }
    const int peek = np2audio86_event_ring_peek(&runtime->events, event);
    if (peek == NP2_AUDIO86_TRANSPORT_OK) {
        if ((*event)->frame_timestamp <=
            runtime->consumer_clock.committed_frame_reconstructed) {
            return WorkerDecision::Work;
        }
        if (np2audio86_runtime_producer_done(&runtime->control)) {
            return worker_after_done(runtime, event);
        }
        return WorkerDecision::Wait;
    }
    if (peek != NP2_AUDIO86_TRANSPORT_EMPTY) {
        publish_error(runtime, kErrorWorker);
        return WorkerDecision::Abort;
    }
    if (np2audio86_runtime_producer_done(&runtime->control)) {
        return worker_after_done(runtime, event);
    }
    return WorkerDecision::Wait;
}

bool apply_event(Runtime *runtime, const struct np2audio86_event *event)
{
    if (event == nullptr || event->sequence != runtime->action_count ||
        event->frame_timestamp < runtime->rendered_frame ||
        event->frame_timestamp >
            runtime->consumer_clock.committed_frame_reconstructed) {
        publish_error(runtime, kErrorWorker);
        return false;
    }
    runtime->rendered_frame = event->frame_timestamp;
    if (event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
        if (event->payload == 0U ||
            event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            np2audio86_byte_ring_pop(&runtime->bytes, runtime->worker_run,
                                     event->payload) !=
                NP2_AUDIO86_TRANSPORT_OK) {
            publish_error(runtime, kErrorWorker);
            return false;
        }
        np2_sha256_update(&runtime->data_sha_context, runtime->worker_run,
                          event->payload);
        runtime->data_bytes += event->payload;
        notify_producer(runtime);
    } else if (event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER) {
        ++runtime->reset_count;
    } else if (event->opcode != NP2_AUDIO86_EVENT_FM_KEY &&
               event->opcode != NP2_AUDIO86_EVENT_PSG_REGISTER) {
        publish_error(runtime, kErrorWorker);
        return false;
    }
    const uint32_t reset_ordinal = event->payload;
    const bool reset = event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER;
    if (np2audio86_event_ring_consume(&runtime->events) !=
        NP2_AUDIO86_TRANSPORT_OK) {
        publish_error(runtime, kErrorWorker);
        return false;
    }
    ++runtime->action_count;
    notify_producer(runtime);
    if (reset) {
        runtime->last_reset_ordinal = reset_ordinal;
        np2audio86_runtime_reset_ack_publish(&runtime->control,
                                             reset_ordinal);
        notify_producer(runtime);
    }
    return true;
}

void worker_normal(Runtime *runtime)
{
    for (;;) {
        const struct np2audio86_event *event = nullptr;
        const WorkerDecision decision = worker_decision(runtime, &event);
        if (decision == WorkerDecision::Work) {
            if (!apply_event(runtime, event)) {
                break;
            }
            continue;
        }
        if (decision == WorkerDecision::Finish ||
            decision == WorkerDecision::Abort) {
            break;
        }
        runtime->worker_waiting.store(1U, std::memory_order_release);
        const WorkerDecision recheck = worker_decision(runtime, &event);
        if (recheck != WorkerDecision::Wait) {
            runtime->worker_waiting.store(0U, std::memory_order_release);
            continue;
        }
        if (runtime->scenario == Scenario::FatalWorkerWait ||
            runtime->scenario == Scenario::StopWorkerWait) {
            xSemaphoreGive(runtime->injection);
        }
        p4_nano_audio86_notifications::wait_worker();
        ++runtime->worker_wakes;
        runtime->worker_waiting.store(0U, std::memory_order_release);
    }
}

void worker_task(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    runtime->worker_core.store(static_cast<uint32_t>(xPortGetCoreID()),
                               std::memory_order_release);
    runtime->worker_priority.store(
        static_cast<uint32_t>(uxTaskPriorityGet(nullptr)),
        std::memory_order_release);
    if (runtime->scenario != Scenario::WorkerReadyTimeout) {
        xSemaphoreGive(runtime->ready);
    }
    xSemaphoreTake(runtime->start, portMAX_DELAY);

    if (abort_requested(runtime)) {
        task_quiescent_epilogue(runtime, &runtime->worker_quiescent);
        return;
    }

    if (runtime->scenario == Scenario::FatalProducerWait) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorWorker);
    } else if (runtime->scenario == Scenario::StopEventWait) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
    } else if (runtime->scenario == Scenario::EventWaitNormal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_event_ring_consume(&runtime->events) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            publish_error(runtime, kErrorWorker);
        }
        notify_producer(runtime);
    } else if (runtime->scenario == Scenario::StopResetWait) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
    } else if (runtime->scenario == Scenario::FatalResetWait) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorWorker);
    } else if (runtime->scenario == Scenario::ByteWakeBeforeRecheck) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_byte_ring_pop(&runtime->bytes, runtime->worker_run,
                                     sizeof(kDataRun)) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            publish_error(runtime, kErrorWorker);
        }
        notify_producer(runtime);
        xSemaphoreGive(runtime->resume);
    } else if (runtime->scenario == Scenario::ByteSpuriousThenNormal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        notify_producer(runtime);
        xSemaphoreGive(runtime->resume);
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_byte_ring_pop(&runtime->bytes, runtime->worker_run,
                                     sizeof(kDataRun)) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            publish_error(runtime, kErrorWorker);
        }
        notify_producer(runtime);
    } else if (runtime->scenario == Scenario::ByteWaitStop) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
    } else if (runtime->scenario == Scenario::ByteWaitFatal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorWorker);
    } else if (runtime->scenario == Scenario::HorizonWakeBeforeRecheck) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_runtime_horizon_try_observe(
                &runtime->control, &runtime->consumer_clock) !=
            NP2_AUDIO86_RUNTIME_HORIZON_OK) {
            publish_error(runtime, kErrorHorizon);
        }
        notify_producer(runtime);
        xSemaphoreGive(runtime->resume);
        worker_normal(runtime);
    } else if (runtime->scenario == Scenario::HorizonSpuriousThenNormal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        notify_producer(runtime);
        xSemaphoreGive(runtime->resume);
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_runtime_horizon_try_observe(
                &runtime->control, &runtime->consumer_clock) !=
            NP2_AUDIO86_RUNTIME_HORIZON_OK) {
            publish_error(runtime, kErrorHorizon);
        }
        notify_producer(runtime);
        worker_normal(runtime);
    } else if (runtime->scenario == Scenario::HorizonWaitStop) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
    } else if (runtime->scenario == Scenario::HorizonWaitFatal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorWorker);
    } else if (runtime->scenario == Scenario::CrossIndexIsolation) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        /* Test-only: index 0 must not release the producer's index-1 wait. */
        (void)xTaskNotifyGiveIndexed(
            runtime->producer_task,
            p4_nano_audio86_notifications::kAudio86WorkerNotificationIndex);
        xSemaphoreGive(runtime->resume);
        const bool index0_did_not_wake =
            xSemaphoreTake(runtime->injection, pdMS_TO_TICKS(1U)) != pdTRUE;
        if (!index0_did_not_wake) {
            publish_error(runtime, kErrorWorker);
        } else {
            runtime->transport_pause_verified.store(
                1U, std::memory_order_release);
        }
        notify_producer(runtime);
        if (xSemaphoreTake(runtime->injection, kTimeout) != pdTRUE) {
            publish_error(runtime, kErrorWorker);
        } else {
            /* Leave a pending index-1 count for the delete/recreate proof. */
            notify_producer(runtime);
            s_stale_audio_index1_seeded = true;
        }
        worker_normal(runtime);
    } else if (runtime->scenario == Scenario::TransportBeforeHorizon) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        const struct np2audio86_event *event = nullptr;
        if (worker_decision(runtime, &event) != WorkerDecision::Wait ||
            runtime->action_count != 0U) {
            publish_error(runtime, kErrorHorizon);
        } else {
            runtime->transport_pause_verified.store(1U,
                                                     std::memory_order_release);
        }
        xSemaphoreGive(runtime->resume);
        worker_normal(runtime);
    } else if (is_completion_recheck_scenario(runtime->scenario)) {
        const struct np2audio86_event *stale_event = nullptr;
        const int stale_peek =
            np2audio86_event_ring_peek(&runtime->events, &stale_event);
        const int stale_horizon = np2audio86_runtime_horizon_try_observe(
            &runtime->control, &runtime->consumer_clock);
        if (stale_peek != NP2_AUDIO86_TRANSPORT_EMPTY ||
            stale_horizon != NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
            publish_error(runtime, kErrorWorker);
        } else {
            runtime->transport_pause_verified.store(
                1U, std::memory_order_release);
        }
        xSemaphoreGive(runtime->injection);
        xSemaphoreTake(runtime->resume, portMAX_DELAY);
        if (runtime->scenario == Scenario::CompletionRecheckWorkerFatal) {
            publish_error(runtime, kErrorWorker);
        }
        worker_normal(runtime);
    } else {
        worker_normal(runtime);
    }
    task_quiescent_epilogue(runtime, &runtime->worker_quiescent);
}

void reset_runtime(Scenario scenario)
{
    ++s_next_generation;
    np2audio86_event_ring_init(&s_runtime.events);
    np2audio86_byte_ring_init(&s_runtime.bytes);
    np2audio86_runtime_control_init(&s_runtime.control);
    s_runtime.producer_clock = {};
    s_runtime.consumer_clock = {};
    s_runtime.producer_waiting.store(0U, std::memory_order_relaxed);
    s_runtime.worker_waiting.store(0U, std::memory_order_relaxed);
    s_runtime.producer_core.store(UINT32_MAX, std::memory_order_relaxed);
    s_runtime.worker_core.store(UINT32_MAX, std::memory_order_relaxed);
    s_runtime.worker_priority.store(0U, std::memory_order_relaxed);
    s_runtime.byte_wait_attempts.store(0U, std::memory_order_relaxed);
    s_runtime.horizon_wait_attempts.store(0U, std::memory_order_relaxed);
    s_runtime.producer_quiescent.store(0U, std::memory_order_relaxed);
    s_runtime.worker_quiescent.store(0U, std::memory_order_relaxed);
    s_runtime.transport_pause_verified.store(0U, std::memory_order_relaxed);
    s_runtime.generation = s_next_generation;
    s_runtime.scenario = scenario;
    s_runtime.consumer_has_horizon = false;
    s_runtime.rendered_frame = 0U;
    s_runtime.next_sequence = 0U;
    s_runtime.action_count = 0U;
    s_runtime.data_bytes = 0U;
    s_runtime.reset_count = 0U;
    s_runtime.last_reset_ordinal = 0U;
    s_runtime.producer_wakes = 0U;
    s_runtime.worker_wakes = 0U;
    std::memset(s_runtime.data_sha, 0, sizeof(s_runtime.data_sha));
    np2_sha256_init(&s_runtime.data_sha_context);
    s_runtime.ready = xSemaphoreCreateCountingStatic(
        2U, 0U, &s_runtime.ready_storage);
    s_runtime.start = xSemaphoreCreateCountingStatic(
        2U, 0U, &s_runtime.start_storage);
    s_runtime.terminal = xSemaphoreCreateCountingStatic(
        2U, 0U, &s_runtime.terminal_storage);
    s_runtime.injection = xSemaphoreCreateBinaryStatic(
        &s_runtime.injection_storage);
    s_runtime.resume = xSemaphoreCreateBinaryStatic(&s_runtime.resume_storage);
}

bool run_completion_recheck_stability()
{
    constexpr uint32_t kRepetitions = 1000U;
    for (uint32_t iteration = 0U; iteration < kRepetitions; ++iteration) {
        np2audio86_event_ring_init(&s_runtime.events);
        np2audio86_byte_ring_init(&s_runtime.bytes);
        np2audio86_runtime_control_init(&s_runtime.control);
        s_runtime.producer_clock = {};
        s_runtime.consumer_clock = {};
        s_runtime.producer_task = nullptr;
        s_runtime.worker_task = nullptr;
        s_runtime.consumer_has_horizon = false;
        s_runtime.rendered_frame = 0U;
        s_runtime.next_sequence = 0U;
        s_runtime.action_count = 0U;
        s_runtime.reset_count = 0U;
        s_runtime.last_reset_ordinal = 0U;

        const struct np2audio86_event *event = nullptr;
        if (np2audio86_event_ring_peek(&s_runtime.events, &event) !=
                NP2_AUDIO86_TRANSPORT_EMPTY ||
            np2audio86_runtime_horizon_try_observe(
                &s_runtime.control, &s_runtime.consumer_clock) !=
                NP2_AUDIO86_RUNTIME_HORIZON_RETRY ||
            !completion_publish_direct(&s_runtime, true, true, false)) {
            return false;
        }
        np2audio86_runtime_producer_done_publish(&s_runtime.control);
        if (!np2audio86_runtime_producer_done(&s_runtime.control) ||
            worker_after_done(&s_runtime, &event) != WorkerDecision::Work ||
            !apply_event(&s_runtime, event) ||
            worker_after_done(&s_runtime, &event) != WorkerDecision::Finish ||
            s_runtime.action_count != 1U || s_runtime.rendered_frame != 8U ||
            s_runtime.consumer_clock.committed_frame_reconstructed != 8U ||
            np2audio86_event_ring_occupancy(&s_runtime.events) != 0U ||
            np2audio86_byte_ring_occupancy(&s_runtime.bytes) != 0U ||
            np2audio86_runtime_horizon_pending(&s_runtime.control)) {
            return false;
        }
    }
    return true;
}

bool wait_task_suspended(TaskHandle_t task)
{
    const TickType_t start = xTaskGetTickCount();
    while (eTaskGetState(task) != eSuspended) {
        if (xTaskGetTickCount() - start >= kTimeout) {
            return false;
        }
        taskYIELD();
    }
    return true;
}

bool storage_reuse_permitted(bool terminal, bool quiescent, bool suspended)
{
    return terminal && quiescent && suspended;
}

bool retire_created_tasks(uint32_t created)
{
    bool terminal = true;
    for (uint32_t index = 0U; index < created; ++index) {
        terminal = terminal &&
                   xSemaphoreTake(s_runtime.terminal, kTimeout) == pdTRUE;
    }
    const bool producer_quiescent = s_runtime.producer_task == nullptr ||
        s_runtime.producer_quiescent.load(std::memory_order_acquire) ==
            s_runtime.generation;
    const bool worker_quiescent = s_runtime.worker_task == nullptr ||
        s_runtime.worker_quiescent.load(std::memory_order_acquire) ==
            s_runtime.generation;
    const bool suspended = terminal && producer_quiescent && worker_quiescent &&
        (s_runtime.producer_task == nullptr ||
         wait_task_suspended(s_runtime.producer_task)) &&
        (s_runtime.worker_task == nullptr ||
         wait_task_suspended(s_runtime.worker_task));
    if (!storage_reuse_permitted(terminal,
                                 producer_quiescent && worker_quiescent,
                                 suspended)) {
        /* Fail closed: live task backing storage and Runtime are retained,
         * and run() must not start another in-process scenario. */
        s_storage_reusable = false;
        return false;
    }
    if (s_runtime.producer_task != nullptr) {
        vTaskDelete(s_runtime.producer_task);
        s_runtime.producer_task = nullptr;
    }
    if (s_runtime.worker_task != nullptr) {
        vTaskDelete(s_runtime.worker_task);
        s_runtime.worker_task = nullptr;
    }
    s_last_deleted_generation = s_runtime.generation;
    return true;
}

bool abort_and_retire_created_tasks(uint32_t created)
{
    publish_stop(&s_runtime);
    for (uint32_t index = 0U; index < created; ++index) {
        xSemaphoreGive(s_runtime.start);
    }
    wake_all(&s_runtime);
    return retire_created_tasks(created);
}

bool run_scenario(Scenario scenario, const char *name)
{
    if (!s_storage_reusable ||
        (s_next_generation != 0U &&
         s_last_deleted_generation != s_next_generation)) {
        return false;
    }
    reset_runtime(scenario);
    if (s_runtime.ready == nullptr || s_runtime.start == nullptr ||
        s_runtime.terminal == nullptr || s_runtime.injection == nullptr ||
        s_runtime.resume == nullptr) {
        return false;
    }
    if (scenario != Scenario::ProducerOnlyPartialCreate) {
        s_runtime.worker_task = xTaskCreateStaticPinnedToCore(
            worker_task, "audio86_worker", kWorkerStackBytes, &s_runtime,
            kWorkerPriority, s_runtime.worker_stack, &s_runtime.worker_tcb,
            kWorkerCore);
    }
    if (scenario != Scenario::WorkerOnlyPartialCreate) {
        s_runtime.producer_task = xTaskCreateStaticPinnedToCore(
            producer_task, "audio86_test_g", kProducerStackBytes, &s_runtime,
            kProducerPriority, s_runtime.producer_stack, &s_runtime.producer_tcb,
            kProducerCore);
    }
    const uint32_t created = (s_runtime.worker_task != nullptr ? 1U : 0U) +
                             (s_runtime.producer_task != nullptr ? 1U : 0U);
    const bool expected_partial =
        scenario == Scenario::WorkerOnlyPartialCreate ||
        scenario == Scenario::ProducerOnlyPartialCreate;
    if (created != (expected_partial ? 1U : 2U)) {
        if (created != 0U) {
            (void)abort_and_retire_created_tasks(created);
        }
        return false;
    }
    if (expected_partial) {
        const bool ready = xSemaphoreTake(s_runtime.ready, kTimeout) == pdTRUE;
        const bool retired = abort_and_retire_created_tasks(created);
        const bool result = ready && retired;
        std::printf("AUDIO86_86R5A_SCENARIO name=%s result=%s first_error=%" PRIu32
                    " stop=%u producer_wakes=%" PRIu32 " worker_wakes=%" PRIu32
                    "\n", name, result ? "PASS" : "FAIL",
                    np2audio86_runtime_first_error(&s_runtime.control),
                    np2audio86_runtime_stop_requested(&s_runtime.control) ? 1U : 0U,
                    s_runtime.producer_wakes, s_runtime.worker_wakes);
        return result;
    }
    const TickType_t ready_timeout =
        (scenario == Scenario::ProducerReadyTimeout ||
         scenario == Scenario::WorkerReadyTimeout)
            ? pdMS_TO_TICKS(20U) : kTimeout;
    const bool ready = xSemaphoreTake(s_runtime.ready, ready_timeout) == pdTRUE &&
                       xSemaphoreTake(s_runtime.ready, ready_timeout) == pdTRUE;
    const bool expected_ready_timeout =
        scenario == Scenario::ProducerReadyTimeout ||
        scenario == Scenario::WorkerReadyTimeout;
    if (!ready) {
        const bool retired = abort_and_retire_created_tasks(created);
        const bool result = expected_ready_timeout && retired;
        std::printf("AUDIO86_86R5A_SCENARIO name=%s result=%s first_error=%" PRIu32
                    " stop=%u producer_wakes=%" PRIu32 " worker_wakes=%" PRIu32
                    "\n", name, result ? "PASS" : "FAIL",
                    np2audio86_runtime_first_error(&s_runtime.control),
                    np2audio86_runtime_stop_requested(&s_runtime.control) ? 1U : 0U,
                    s_runtime.producer_wakes, s_runtime.worker_wakes);
        return result;
    }
    if (expected_ready_timeout) {
        (void)abort_and_retire_created_tasks(created);
        return false;
    }
    xSemaphoreGive(s_runtime.start);
    xSemaphoreGive(s_runtime.start);
    const bool quiescent = retire_created_tasks(created);

    const bool affinity =
        s_runtime.producer_core.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(kProducerCore) &&
        s_runtime.worker_core.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(kWorkerCore) &&
        s_runtime.worker_priority.load(std::memory_order_acquire) ==
            kWorkerPriority;
    bool result = quiescent && affinity;
    if (scenario == Scenario::Normal) {
        np2_sha256_final(&s_runtime.data_sha_context, s_runtime.data_sha);
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 !np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.action_count == 4U &&
                 s_runtime.data_bytes == sizeof(kDataRun) &&
                 s_runtime.reset_count == 1U &&
                 s_runtime.rendered_frame == 16U &&
                 s_runtime.consumer_clock.committed_frame_reconstructed == 16U &&
                 np2audio86_runtime_reset_ack(&s_runtime.control) == 1U &&
                 np2audio86_event_ring_occupancy(&s_runtime.events) == 0U &&
                 np2audio86_byte_ring_occupancy(&s_runtime.bytes) == 0U &&
                 !np2audio86_runtime_horizon_pending(&s_runtime.control) &&
                 s_runtime.last_reset_ordinal == 1U &&
                 std::memcmp(s_runtime.data_sha, kDataRunSha256,
                             sizeof(kDataRunSha256)) == 0;
        std::printf("AUDIO86_86R5A_HEADLESS actions=%" PRIu32
                    " final_sequence=%" PRIu64 " data_bytes=%" PRIu32
                    " resets=%" PRIu32 " committed=%" PRIu64
                    " rendered=%" PRIu64 " residual_events=%" PRIu32
                    " residual_bytes=%" PRIu32 " data_sha256=",
                    s_runtime.action_count, s_runtime.next_sequence,
                    s_runtime.data_bytes, s_runtime.reset_count,
                    s_runtime.consumer_clock.committed_frame_reconstructed,
                    s_runtime.rendered_frame,
                    np2audio86_event_ring_occupancy(&s_runtime.events),
                    np2audio86_byte_ring_occupancy(&s_runtime.bytes));
        for (uint8_t byte : s_runtime.data_sha) {
            std::printf("%02x", static_cast<unsigned>(byte));
        }
        std::printf(" result=%s\n", result ? "PASS" : "FAIL");
    } else if (scenario == Scenario::FatalWorkerWait) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorProducer &&
                 s_runtime.worker_wakes > 0U;
    } else if (scenario == Scenario::FatalProducerWait) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorWorker &&
                 s_runtime.producer_wakes > 0U;
    } else if (scenario == Scenario::StopEventWait) {
        result = result &&
                 np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.producer_wakes > 0U &&
                 s_runtime.next_sequence == NP2_AUDIO86_ASYNC_EVENT_CAPACITY;
    } else if (scenario == Scenario::EventWaitNormal) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 !np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.producer_wakes > 0U &&
                 s_runtime.next_sequence ==
                     NP2_AUDIO86_ASYNC_EVENT_CAPACITY + 1U;
    } else if (scenario == Scenario::StopResetWait) {
        result = result &&
                 np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.producer_wakes > 0U &&
                 np2audio86_runtime_reset_ack(&s_runtime.control) != 1U;
    } else if (scenario == Scenario::FatalResetWait) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorWorker &&
                 s_runtime.producer_wakes > 0U;
    } else if (scenario == Scenario::StopWorkerWait) {
        result = result &&
                 np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.worker_wakes > 0U;
    } else if (scenario == Scenario::CrossIndexIsolation) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 s_runtime.producer_wakes == 1U &&
                 s_stale_audio_index1_seeded;
    } else if (scenario == Scenario::ByteWakeBeforeRecheck) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.byte_wait_attempts.load(std::memory_order_acquire) ==
                     1U &&
                 s_runtime.producer_wakes == 0U;
    } else if (scenario == Scenario::ByteSpuriousThenNormal) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.byte_wait_attempts.load(std::memory_order_acquire) >=
                     2U &&
                 s_runtime.producer_wakes >= 2U;
    } else if (scenario == Scenario::ByteWaitStop) {
        result = result &&
                 np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.producer_wakes > 0U &&
                 np2audio86_byte_ring_occupancy(&s_runtime.bytes) ==
                     NP2_AUDIO86_ASYNC_BYTE_CAPACITY;
    } else if (scenario == Scenario::ByteWaitFatal) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorWorker &&
                 s_runtime.producer_wakes > 0U &&
                 np2audio86_byte_ring_occupancy(&s_runtime.bytes) ==
                     NP2_AUDIO86_ASYNC_BYTE_CAPACITY;
    } else if (scenario == Scenario::HorizonWakeBeforeRecheck) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.horizon_wait_attempts.load(
                     std::memory_order_acquire) == 1U &&
                 s_runtime.producer_wakes == 0U &&
                 s_runtime.consumer_clock.committed_frame_reconstructed == 0U;
    } else if (scenario == Scenario::HorizonSpuriousThenNormal) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.horizon_wait_attempts.load(
                     std::memory_order_acquire) >= 2U &&
                 s_runtime.producer_wakes >= 1U &&
                 s_runtime.consumer_clock.committed_frame_reconstructed == 0U;
    } else if (scenario == Scenario::HorizonWaitStop) {
        result = result &&
                 np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.producer_wakes > 0U &&
                 np2audio86_runtime_horizon_pending(&s_runtime.control);
    } else if (scenario == Scenario::HorizonWaitFatal) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorWorker &&
                 s_runtime.producer_wakes > 0U &&
                 np2audio86_runtime_horizon_pending(&s_runtime.control);
    } else if (scenario == Scenario::TransportBeforeHorizon) {
        result = result &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 s_runtime.action_count == 1U &&
                 np2audio86_event_ring_occupancy(&s_runtime.events) == 0U;
    } else if (scenario == Scenario::CompletionRecheckEventOnly ||
               scenario == Scenario::CompletionRecheckHorizonOnly ||
               scenario == Scenario::CompletionRecheckCombined ||
               scenario == Scenario::CompletionRecheckEmpty ||
               scenario == Scenario::CompletionRecheckReset) {
        const uint64_t expected_frame =
            (scenario == Scenario::CompletionRecheckHorizonOnly ||
             scenario == Scenario::CompletionRecheckCombined)
                ? 8U : 0U;
        const uint32_t expected_actions =
            (scenario == Scenario::CompletionRecheckEventOnly ||
             scenario == Scenario::CompletionRecheckCombined ||
             scenario == Scenario::CompletionRecheckReset)
                ? 1U : 0U;
        const bool reset_ok =
            scenario != Scenario::CompletionRecheckReset ||
            (s_runtime.reset_count == 1U &&
             s_runtime.last_reset_ordinal == 1U &&
             np2audio86_runtime_reset_ack(&s_runtime.control) == 1U);
        result = result &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 np2audio86_runtime_producer_done(&s_runtime.control) &&
                 np2audio86_runtime_first_error(&s_runtime.control) == 0U &&
                 !np2audio86_runtime_stop_requested(&s_runtime.control) &&
                 s_runtime.action_count == expected_actions &&
                 s_runtime.rendered_frame == expected_frame &&
                 s_runtime.consumer_clock.committed_frame_reconstructed ==
                     expected_frame &&
                 np2audio86_event_ring_occupancy(&s_runtime.events) == 0U &&
                 np2audio86_byte_ring_occupancy(&s_runtime.bytes) == 0U &&
                 !np2audio86_runtime_horizon_pending(&s_runtime.control) &&
                 reset_ok;
    } else if (scenario == Scenario::CompletionRecheckStop) {
        result = result &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 np2audio86_runtime_producer_done(&s_runtime.control) &&
                 np2audio86_runtime_stop_requested(&s_runtime.control);
    } else if (scenario == Scenario::CompletionRecheckProducerFatal) {
        result = result &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 np2audio86_runtime_producer_done(&s_runtime.control) &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorProducer;
    } else if (scenario == Scenario::CompletionRecheckWorkerFatal) {
        result = result &&
                 s_runtime.transport_pause_verified.load(
                     std::memory_order_acquire) == 1U &&
                 np2audio86_runtime_producer_done(&s_runtime.control) &&
                 np2audio86_runtime_first_error(&s_runtime.control) ==
                     kErrorWorker;
    }
    std::printf("AUDIO86_86R5A_SCENARIO name=%s result=%s first_error=%" PRIu32
                " stop=%u producer_wakes=%" PRIu32 " worker_wakes=%" PRIu32
                " quiescent=%u affinity=%u producer_q=%" PRIu32
                " worker_q=%" PRIu32 " generation=%" PRIu32 "\n",
                name, result ? "PASS" : "FAIL",
                np2audio86_runtime_first_error(&s_runtime.control),
                np2audio86_runtime_stop_requested(&s_runtime.control) ? 1U : 0U,
                s_runtime.producer_wakes, s_runtime.worker_wakes,
                quiescent ? 1U : 0U, affinity ? 1U : 0U,
                s_runtime.producer_quiescent.load(std::memory_order_acquire),
                s_runtime.worker_quiescent.load(std::memory_order_acquire),
                s_runtime.generation);
    return result;
}

bool memory_is_internal()
{
    return esp_ptr_internal(&s_runtime) &&
           esp_ptr_internal(&s_runtime.events) &&
           esp_ptr_internal(&s_runtime.bytes) &&
           esp_ptr_internal(&s_runtime.control) &&
           esp_ptr_internal(s_runtime.worker_run) &&
           esp_ptr_internal(s_runtime.producer_stack) &&
           esp_ptr_internal(s_runtime.worker_stack) &&
           esp_ptr_internal(&s_runtime.producer_tcb) &&
           esp_ptr_internal(&s_runtime.worker_tcb);
}

} // namespace

esp_err_t run()
{
    std::printf("AUDIO86_86R5A_PROFILE scope=TEST_ISOLATED_HEADLESS"
                " production_runtime_active=NO i2s=NO es8311=NO a2=NO"
                " guest_binding=NO\n");
    std::printf("AUDIO86_86R5A_SIZE event_abi=%zu event_ring=%zu byte_ring=%zu"
                " mailbox=%zu mailbox_align=%zu control=%zu control_align=%zu producer_clock=%zu"
                " consumer_clock=%zu worker_context=%zu worker_stack=%u"
                " producer_stack=%u\n",
                sizeof(struct np2audio86_event), sizeof(s_runtime.events),
                sizeof(s_runtime.bytes),
                sizeof(struct np2audio86_runtime_horizon_mailbox),
                alignof(struct np2audio86_runtime_horizon_mailbox),
                sizeof(s_runtime.control),
                alignof(struct np2audio86_runtime_control),
                sizeof(s_runtime.producer_clock),
                sizeof(s_runtime.consumer_clock), sizeof(s_runtime),
                static_cast<unsigned>(kWorkerStackBytes),
                static_cast<unsigned>(kProducerStackBytes));
    std::printf("AUDIO86_86R5A_MEMORY internal=%s psram_fallback=NO"
                " internal_free=%u internal_largest=%u\n",
                memory_is_internal() ? "PASS" : "FAIL",
                static_cast<unsigned>(heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    bool ok = memory_is_internal();
    ok = ok && run_completion_recheck_stability();
    ok = ok && run_scenario(Scenario::WorkerOnlyPartialCreate,
                            "worker_only_partial_create");
    ok = ok && run_scenario(Scenario::ProducerOnlyPartialCreate,
                            "producer_only_partial_create");
    ok = ok && run_scenario(Scenario::ProducerReadyTimeout,
                            "producer_ready_timeout");
    ok = ok && run_scenario(Scenario::WorkerReadyTimeout,
                            "worker_ready_timeout");
    ok = ok && run_scenario(Scenario::FatalWorkerWait, "fatal_worker_wait");
    ok = ok && run_scenario(Scenario::FatalProducerWait, "fatal_producer_wait");
    ok = ok && run_scenario(Scenario::StopEventWait, "stop_event_wait");
    ok = ok && run_scenario(Scenario::EventWaitNormal, "event_wait_normal");
    ok = ok && run_scenario(Scenario::StopResetWait, "stop_reset_wait");
    ok = ok && run_scenario(Scenario::FatalResetWait, "fatal_reset_wait");
    ok = ok && run_scenario(Scenario::StopWorkerWait, "stop_worker_wait");
    ok = ok && run_scenario(Scenario::ByteWakeBeforeRecheck,
                            "byte_wake_before_recheck");
    ok = ok && run_scenario(Scenario::ByteSpuriousThenNormal,
                            "byte_spurious_then_normal");
    ok = ok && run_scenario(Scenario::ByteWaitStop, "byte_wait_stop");
    ok = ok && run_scenario(Scenario::ByteWaitFatal, "byte_wait_fatal");
    ok = ok && run_scenario(Scenario::HorizonWakeBeforeRecheck,
                            "horizon_wake_before_recheck");
    ok = ok && run_scenario(Scenario::HorizonSpuriousThenNormal,
                            "horizon_spurious_then_normal");
    ok = ok && run_scenario(Scenario::HorizonWaitStop, "horizon_wait_stop");
    ok = ok && run_scenario(Scenario::HorizonWaitFatal, "horizon_wait_fatal");
    ok = ok && run_scenario(Scenario::TransportBeforeHorizon,
                            "transport_before_horizon");
    ok = ok && run_scenario(Scenario::CompletionRecheckEventOnly,
                            "completion_recheck_event_only");
    ok = ok && run_scenario(Scenario::CompletionRecheckHorizonOnly,
                            "completion_recheck_horizon_only");
    ok = ok && run_scenario(Scenario::CompletionRecheckCombined,
                            "completion_recheck_combined");
    ok = ok && run_scenario(Scenario::CompletionRecheckEmpty,
                            "completion_recheck_empty");
    ok = ok && run_scenario(Scenario::CompletionRecheckStop,
                            "completion_recheck_stop");
    ok = ok && run_scenario(Scenario::CompletionRecheckProducerFatal,
                            "completion_recheck_producer_fatal");
    ok = ok && run_scenario(Scenario::CompletionRecheckWorkerFatal,
                            "completion_recheck_worker_fatal");
    ok = ok && run_scenario(Scenario::CompletionRecheckReset,
                            "completion_recheck_reset");
    ok = ok && run_scenario(Scenario::CrossIndexIsolation,
                            "cross_index_isolation");
    /* Finish on the canonical successful stream so final residual evidence
     * describes successful finalize rather than an injected abort. */
    ok = ok && run_scenario(Scenario::Normal, "normal");

    std::printf("P4_AUDIO86_AFFINITY=%s producer_core=1 worker_core=0"
                " worker_priority=6 worker_stack=8192\n",
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_INTERNAL_MEMORY_ONLY=%s\n", ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_NOTIFICATION_CONFIG=%s notification_entries=%u"
                " producer_notification_index=%u worker_notification_index=%u"
                " static_task_size=%zu worker_context=%zu\n",
                ok ? "PASS" : "FAIL",
                static_cast<unsigned>(configTASK_NOTIFICATION_ARRAY_ENTRIES),
                static_cast<unsigned>(
                    p4_nano_audio86_notifications::kAudio86ProducerNotificationIndex),
                static_cast<unsigned>(
                    p4_nano_audio86_notifications::kAudio86WorkerNotificationIndex),
                sizeof(StaticTask_t), sizeof(s_runtime));
    std::printf("TASK_NOTIFICATION_OWNERSHIP=%s producer_slot=1"
                " owner=AUDIO_TRANSPORT worker_slot=0 owner=AUDIO_TRANSPORT\n",
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO_NOTIFICATION_INDEX_COMPILE_GUARD=%s\n"
                "AUDIO_NOTIFICATION_ISR_PATH=NONE\n"
                "CROSS_INDEX_WAKE_STEALING=%s\n"
                "CROSS_INDEX_STALE_VALUE_INTERFERENCE=%s\n"
                "RECREATED_TASK_AUDIO_NOTIFICATION_CLEAN=%s\n",
                ok ? "PASS" : "FAIL", ok ? "NONE" : "DETECTED",
                ok ? "NONE" : "DETECTED",
                ok && s_recreated_audio_notification_clean ? "PASS" : "FAIL");
    std::printf("P4_STOP_WAKE_ALL=%s\nP4_FATAL_WAKE_ALL=%s\n"
                "AUDIO_STOP_WAKE_FANOUT=%s\nAUDIO_FATAL_WAKE_FANOUT=%s\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::printf("BYTE_CAPACITY_WAIT_RETRY=%s\nBYTE_RETIRE_WAKE=%s\n"
                "BYTE_WAIT_LOST_WAKEUP_PROOF=%s\n"
                "BYTE_WAIT_STOP_WAKE=%s\nBYTE_WAIT_FATAL_WAKE=%s\n"
                "EVENT_WAIT_STOP_WAKE=%s\nEVENT_WAIT_FATAL_WAKE=%s\n"
                "FREERTOS_WAIT_PROTOCOL=%s\nLOST_WAKEUP_PROOF=%s\n"
                "P4_WAKE_MATRIX=%s\n"
                "EVENT_WAIT_INDEX1=%s\nBYTE_WAIT_INDEX1=%s\n"
                "HORIZON_WAIT_INDEX1=%s\nRESET_ACK_WAIT_INDEX1=%s\n"
                "WORKER_WAIT_INDEX0=%s\n"
                "PARTIAL_CREATE_INDEXED_NOTIFICATION=%s\n"
                "READY_TIMEOUT_INDEXED_NOTIFICATION=%s\n"
                "TASK_REUSE_ENTRIES2=%s\nTASK_QUIESCENCE_ENTRIES2=%s\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL");
    const bool terminal_timeout_policy =
        !storage_reuse_permitted(false, false, false);
    std::printf("HORIZON_MAILBOX_C11_PROOF=%s\n"
                "HORIZON_INDEFINITE_PUBLICATION=%s\n"
                "HORIZON_FULL_WAIT_RETRY=%s\n"
                "HORIZON_WAIT_PROTOCOL=%s\n"
                "TRANSPORT_BEFORE_HORIZON=%s\n"
                "STATIC_TASK_QUIESCENCE_PROTOCOL=%s\n"
                "PARTIAL_CREATE_QUIESCENCE=%s\n"
                "READY_TIMEOUT_QUIESCENCE=%s\n"
                "TERMINAL_PATHS_UNIFIED=%s\n"
                "TERMINAL_TIMEOUT_NO_REUSE=%s\n"
                "STATIC_STORAGE_REUSE_SAFE=%s generation=%" PRIu32 "\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL",
                ok && terminal_timeout_policy ? "PASS" : "FAIL",
                ok && s_storage_reusable &&
                        s_last_deleted_generation == s_next_generation
                    ? "PASS" : "FAIL",
                s_last_deleted_generation);
    std::printf("P4_POST_DONE_EVENT_RECHECK=%s\n"
                "P4_POST_DONE_HORIZON_ONLY=%s\n"
                "P4_POST_DONE_COMBINED_RECHECK=%s\n"
                "P4_POST_DONE_EMPTY_FINISH=%s\n"
                "FINAL_COMPLETION_LEVEL_PREDICATE=%s\n"
                "TERMINAL_ERROR_PRECEDENCE=%s\n"
                "RESET_FINALIZE_NONREGRESSION=%s\n"
                "POST_DONE_HORIZON_RECHECK=%s\n"
                "P4_POST_DONE_C11_PROOF=%s\n"
                "SINGLE_P4_COMPLETION_RULE=%s\n"
                "BYTE_POST_DONE_RECHECK_NOT_REQUIRED_BY_PROTOCOL=%s\n"
                "SUCCESSFUL_FINAL_RESIDUALS_ZERO=%s\n"
                "P4_POST_DONE_COMPLETION_STRESS=%s repetitions=1000\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_86R5A_FINAL residual_events=%" PRIu32
                " residual_bytes=%" PRIu32 " horizon_pending=%u"
                " reset_ack=%" PRIu32 " first_error=%" PRIu32
                " result=%s timing=NOT_VALIDATED\n",
                np2audio86_event_ring_occupancy(&s_runtime.events),
                np2audio86_byte_ring_occupancy(&s_runtime.bytes),
                np2audio86_runtime_horizon_pending(&s_runtime.control) ? 1U : 0U,
                np2audio86_runtime_reset_ack(&s_runtime.control),
                np2audio86_runtime_first_error(&s_runtime.control),
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_86R5A_RESULT=%s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio86_runtime
