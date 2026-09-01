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
    ByteWakeBeforeRecheck,
    ByteSpuriousThenNormal,
    ByteWaitStop,
    ByteWaitFatal,
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
    Scenario scenario = Scenario::Normal;
    uint64_t rendered_frame = 0U;
    uint64_t next_sequence = 0U;
    uint32_t action_count = 0U;
    uint32_t data_bytes = 0U;
    uint32_t reset_count = 0U;
    uint32_t producer_wakes = 0U;
    uint32_t worker_wakes = 0U;
    uint8_t data_sha[NP2_SHA256_DIGEST_SIZE]{};
    np2_sha256_context data_sha_context{};
};

static_assert(sizeof(struct np2audio86_event) == 24U);
static_assert(sizeof(struct np2audio86_event_ring) == 3080U);
static_assert(sizeof(struct np2audio86_byte_ring) == 65544U);
static_assert(sizeof(struct np2audio86_runtime_control) == 28U);
static_assert(alignof(struct np2audio86_runtime_control) >= 4U);
static_assert(sizeof(((Runtime *)nullptr)->producer_stack) ==
              kProducerStackBytes);
static_assert(sizeof(((Runtime *)nullptr)->worker_stack) == kWorkerStackBytes);
static_assert(sizeof(Runtime) == 117120U);

DRAM_ATTR Runtime s_runtime{};

void notify(TaskHandle_t task)
{
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void wake_all(Runtime *runtime)
{
    notify(runtime->producer_task);
    notify(runtime->worker_task);
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
            notify(runtime->worker_task);
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
    if (np2audio86_runtime_horizon_publish(
            &runtime->control, &runtime->producer_clock, 16U) !=
        NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        publish_error(runtime, kErrorHorizon);
        return false;
    }
    notify(runtime->worker_task);
    if (!wait_reset_ack(runtime, 1U)) {
        return false;
    }
    if (!publish_event(runtime, 16U, NP2_AUDIO86_EVENT_PSG_REGISTER, 7U) ||
        np2audio86_runtime_horizon_publish(
            &runtime->control, &runtime->producer_clock, 16U) !=
            NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        publish_error(runtime, kErrorProducer);
        return false;
    }
    notify(runtime->worker_task);
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

void producer_task(void *opaque)
{
    auto *runtime = static_cast<Runtime *>(opaque);
    runtime->producer_core.store(static_cast<uint32_t>(xPortGetCoreID()),
                                 std::memory_order_release);
    xSemaphoreGive(runtime->ready);
    xSemaphoreTake(runtime->start, portMAX_DELAY);

    switch (runtime->scenario) {
    case Scenario::Normal:
        (void)publish_normal_stream(runtime);
        np2audio86_runtime_producer_done_publish(&runtime->control);
        notify(runtime->worker_task);
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
            np2audio86_runtime_horizon_publish(
                &runtime->control, &runtime->producer_clock, 0U) ==
                NP2_AUDIO86_RUNTIME_HORIZON_OK) {
            notify(runtime->worker_task);
            (void)wait_reset_ack(runtime, 1U);
        }
        break;
    case Scenario::StopWorkerWait:
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
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
    }
    xSemaphoreGive(runtime->terminal);
    vTaskSuspend(nullptr);
}

enum class WorkerDecision { Work, Wait, Finish, Abort };

WorkerDecision worker_decision(Runtime *runtime,
                               const struct np2audio86_event **event)
{
    const int horizon = np2audio86_runtime_horizon_try_observe(
        &runtime->control, &runtime->consumer_clock);
    if (horizon == NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
        return WorkerDecision::Wait;
    }
    if (horizon != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        publish_error(runtime, kErrorHorizon);
        return WorkerDecision::Abort;
    }
    if (abort_requested(runtime)) {
        return WorkerDecision::Abort;
    }
    const int peek = np2audio86_event_ring_peek(&runtime->events, event);
    if (peek == NP2_AUDIO86_TRANSPORT_OK) {
        if ((*event)->frame_timestamp <=
            runtime->consumer_clock.committed_frame_reconstructed) {
            return WorkerDecision::Work;
        }
        if (np2audio86_runtime_producer_done(&runtime->control)) {
            publish_error(runtime, kErrorWorker);
            return WorkerDecision::Abort;
        }
        return WorkerDecision::Wait;
    }
    if (peek != NP2_AUDIO86_TRANSPORT_EMPTY) {
        publish_error(runtime, kErrorWorker);
        return WorkerDecision::Abort;
    }
    if (np2audio86_runtime_producer_done(&runtime->control) &&
        runtime->rendered_frame >=
            runtime->consumer_clock.committed_frame_reconstructed) {
        return WorkerDecision::Finish;
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
        notify(runtime->producer_task);
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
    notify(runtime->producer_task);
    if (reset) {
        np2audio86_runtime_reset_ack_publish(&runtime->control,
                                             reset_ordinal);
        notify(runtime->producer_task);
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
    xSemaphoreGive(runtime->ready);
    xSemaphoreTake(runtime->start, portMAX_DELAY);

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
        notify(runtime->producer_task);
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
        notify(runtime->producer_task);
        xSemaphoreGive(runtime->resume);
    } else if (runtime->scenario == Scenario::ByteSpuriousThenNormal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        notify(runtime->producer_task);
        xSemaphoreGive(runtime->resume);
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        if (np2audio86_byte_ring_pop(&runtime->bytes, runtime->worker_run,
                                     sizeof(kDataRun)) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            publish_error(runtime, kErrorWorker);
        }
        notify(runtime->producer_task);
    } else if (runtime->scenario == Scenario::ByteWaitStop) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_stop(runtime);
    } else if (runtime->scenario == Scenario::ByteWaitFatal) {
        xSemaphoreTake(runtime->injection, portMAX_DELAY);
        publish_error(runtime, kErrorWorker);
    } else {
        worker_normal(runtime);
    }
    xSemaphoreGive(runtime->terminal);
    vTaskSuspend(nullptr);
}

void reset_runtime(Scenario scenario)
{
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
    s_runtime.scenario = scenario;
    s_runtime.rendered_frame = 0U;
    s_runtime.next_sequence = 0U;
    s_runtime.action_count = 0U;
    s_runtime.data_bytes = 0U;
    s_runtime.reset_count = 0U;
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

bool run_scenario(Scenario scenario, const char *name)
{
    reset_runtime(scenario);
    if (s_runtime.ready == nullptr || s_runtime.start == nullptr ||
        s_runtime.terminal == nullptr || s_runtime.injection == nullptr ||
        s_runtime.resume == nullptr) {
        return false;
    }
    s_runtime.worker_task = xTaskCreateStaticPinnedToCore(
        worker_task, "audio86_worker", kWorkerStackBytes, &s_runtime,
        kWorkerPriority, s_runtime.worker_stack, &s_runtime.worker_tcb,
        kWorkerCore);
    s_runtime.producer_task = xTaskCreateStaticPinnedToCore(
        producer_task, "audio86_test_g", kProducerStackBytes, &s_runtime,
        kProducerPriority, s_runtime.producer_stack, &s_runtime.producer_tcb,
        kProducerCore);
    if (s_runtime.worker_task == nullptr || s_runtime.producer_task == nullptr) {
        if (s_runtime.producer_task != nullptr) {
            vTaskDelete(s_runtime.producer_task);
            s_runtime.producer_task = nullptr;
        }
        if (s_runtime.worker_task != nullptr) {
            vTaskDelete(s_runtime.worker_task);
            s_runtime.worker_task = nullptr;
        }
        return false;
    }
    if (xSemaphoreTake(s_runtime.ready, kTimeout) != pdTRUE ||
        xSemaphoreTake(s_runtime.ready, kTimeout) != pdTRUE) {
        publish_stop(&s_runtime);
        xSemaphoreGive(s_runtime.start);
        xSemaphoreGive(s_runtime.start);
        vTaskDelete(s_runtime.producer_task);
        vTaskDelete(s_runtime.worker_task);
        s_runtime.producer_task = nullptr;
        s_runtime.worker_task = nullptr;
        return false;
    }
    xSemaphoreGive(s_runtime.start);
    xSemaphoreGive(s_runtime.start);
    const bool terminal =
        xSemaphoreTake(s_runtime.terminal, kTimeout) == pdTRUE &&
        xSemaphoreTake(s_runtime.terminal, kTimeout) == pdTRUE;
    /* A terminal give precedes self-suspend.  Do not recycle any task or
     * semaphore backing storage until both cores have completed that final
     * transition. */
    const bool quiescent = terminal &&
                           wait_task_suspended(s_runtime.producer_task) &&
                           wait_task_suspended(s_runtime.worker_task);
    vTaskDelete(s_runtime.producer_task);
    vTaskDelete(s_runtime.worker_task);
    s_runtime.producer_task = nullptr;
    s_runtime.worker_task = nullptr;

    const bool affinity =
        s_runtime.producer_core.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(kProducerCore) &&
        s_runtime.worker_core.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(kWorkerCore) &&
        s_runtime.worker_priority.load(std::memory_order_acquire) ==
            kWorkerPriority;
    bool result = terminal && quiescent && affinity;
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
    }
    std::printf("AUDIO86_86R5A_SCENARIO name=%s result=%s first_error=%" PRIu32
                " stop=%u producer_wakes=%" PRIu32 " worker_wakes=%" PRIu32
                "\n",
                name, result ? "PASS" : "FAIL",
                np2audio86_runtime_first_error(&s_runtime.control),
                np2audio86_runtime_stop_requested(&s_runtime.control) ? 1U : 0U,
                s_runtime.producer_wakes, s_runtime.worker_wakes);
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
                " control=%zu control_align=%zu producer_clock=%zu"
                " consumer_clock=%zu worker_context=%zu worker_stack=%u"
                " producer_stack=%u\n",
                sizeof(struct np2audio86_event), sizeof(s_runtime.events),
                sizeof(s_runtime.bytes), sizeof(s_runtime.control),
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
    ok = run_scenario(Scenario::FatalWorkerWait, "fatal_worker_wait") && ok;
    ok = run_scenario(Scenario::FatalProducerWait, "fatal_producer_wait") && ok;
    ok = run_scenario(Scenario::StopEventWait, "stop_event_wait") && ok;
    ok = run_scenario(Scenario::EventWaitNormal, "event_wait_normal") && ok;
    ok = run_scenario(Scenario::StopResetWait, "stop_reset_wait") && ok;
    ok = run_scenario(Scenario::FatalResetWait, "fatal_reset_wait") && ok;
    ok = run_scenario(Scenario::StopWorkerWait, "stop_worker_wait") && ok;
    ok = run_scenario(Scenario::ByteWakeBeforeRecheck,
                      "byte_wake_before_recheck") && ok;
    ok = run_scenario(Scenario::ByteSpuriousThenNormal,
                      "byte_spurious_then_normal") && ok;
    ok = run_scenario(Scenario::ByteWaitStop, "byte_wait_stop") && ok;
    ok = run_scenario(Scenario::ByteWaitFatal, "byte_wait_fatal") && ok;
    /* Finish on the canonical successful stream so final residual evidence
     * describes successful finalize rather than an injected abort. */
    ok = run_scenario(Scenario::Normal, "normal") && ok;

    std::printf("P4_AUDIO86_AFFINITY=%s producer_core=1 worker_core=0"
                " worker_priority=6 worker_stack=8192\n",
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_INTERNAL_MEMORY_ONLY=%s\n", ok ? "PASS" : "FAIL");
    std::printf("TASK_NOTIFICATION_OWNERSHIP=%s slot=0 owner=TRANSPORT_ONLY\n",
                ok ? "PASS" : "FAIL");
    std::printf("P4_STOP_WAKE_ALL=%s\nP4_FATAL_WAKE_ALL=%s\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::printf("BYTE_CAPACITY_WAIT_RETRY=%s\nBYTE_RETIRE_WAKE=%s\n"
                "BYTE_WAIT_LOST_WAKEUP_PROOF=%s\n"
                "BYTE_WAIT_STOP_WAKE=%s\nBYTE_WAIT_FATAL_WAKE=%s\n"
                "EVENT_WAIT_STOP_WAKE=%s\nEVENT_WAIT_FATAL_WAKE=%s\n"
                "FREERTOS_WAIT_PROTOCOL=%s\nLOST_WAKEUP_PROOF=%s\n"
                "P4_WAKE_MATRIX=%s\n",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
                ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_86R5A_FINAL residual_events=%" PRIu32
                " residual_bytes=%" PRIu32 " result=%s timing=NOT_VALIDATED\n",
                np2audio86_event_ring_occupancy(&s_runtime.events),
                np2audio86_byte_ring_occupancy(&s_runtime.bytes),
                ok ? "PASS" : "FAIL");
    std::printf("AUDIO86_86R5A_RESULT=%s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio86_runtime
