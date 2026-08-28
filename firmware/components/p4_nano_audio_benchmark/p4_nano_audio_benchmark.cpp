#include "p4_nano_audio_benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_s98.h"
#include "np2opngen_spsc.h"
#include "np2opngen_synthetic_workload.h"

extern "C" {
extern const uint8_t _binary_retrofm_pocket_demo_strict_s98_start[];
extern const uint8_t _binary_retrofm_pocket_demo_strict_s98_end[];
}

namespace p4_nano_audio_benchmark {
namespace {

constexpr uint32_t kRate = 48000U;
constexpr uint32_t kQuantum = NP2_OPNGEN_E1B_RENDER_QUANTUM;
constexpr int kProducerCore = 1;
constexpr int kWorkerCore = 0;
constexpr UBaseType_t kProducerPriority = tskIDLE_PRIORITY + 3;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 4;
constexpr size_t kMaxQuanta = 12000U;
constexpr uint32_t kHousekeepingQuantumInterval = 64U;
constexpr uint32_t kHousekeepingDelayTicks = 1U;

struct Expected {
    uint64_t events;
    uint64_t end_frame;
    uint32_t event_crc;
    uint8_t event_sha[NP2_SHA256_DIGEST_SIZE];
    uint32_t pcm_crc;
    uint8_t pcm_sha[NP2_SHA256_DIGEST_SIZE];
};

struct Workload {
    const char *name;
    bool retro;
    enum np2opngen_synthetic_profile profile;
    uint32_t duration_seconds;
    Expected expected;
};

struct Sink {
    uint64_t frames = 0U;
    uint64_t bytes = 0U;
    uint32_t crc = np2_crc32_iso_hdlc_init();
    np2_sha256_context sha{};
    bool sha_enabled = false;
};

struct Timing {
    uint32_t *full = nullptr;
    uint32_t *pure = nullptr;
    uint8_t *seen = nullptr;
    size_t quanta = 0U;
    int64_t dequeue_start = 0;
    uint32_t pending_dequeue_us = 0U;
    int64_t event_start = 0;
    uint64_t event_quantum = 0U;
    bool event_zero = false;
    int64_t apply_start = 0;
    uint64_t apply_quantum = 0U;
    bool apply_zero = false;
    int64_t render_start = 0;
    uint64_t render_quantum = 0U;
    int64_t opngen_start = 0;
    uint64_t startup_active_us = 0U;
    uint64_t startup_zero_events = 0U;
    const char *arrays_placement = "unallocated";
};

enum class FailureStage : uint32_t {
    None = 0U,
    Preflight,
    TimingAlloc,
    AtomicGate,
    WorkerInit,
    DoneCreate,
    WorkerCreate,
    ProducerCreate,
    DoneTimeout,
    ProducerFail,
    WorkerFailed,
    ObserverInvariant,
    FinishIdentity,
    PrintTiming,
};

struct RunContext {
    const Workload *workload = nullptr;
    bool correctness = false;
    std::atomic<bool> failed{false};
    std::atomic<FailureStage> failure_stage{FailureStage::None};
    bool atomic_lock_free = false;
    struct np2opngen_spsc_queue queue{};
    struct np2opngen_e1b_control control{};
    struct np2opngen_e1b_worker worker{};
    struct np2opngen_e1b_observer observer{};
    Sink sink{};
    Timing timing{};
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t worker_task = nullptr;
    TaskHandle_t producer_task = nullptr;
    std::atomic<bool> producer_waiting{false};
    uint64_t completed_quanta = 0U;
    struct np2opngen_synth_event_trace_state producer_trace{};
    bool producer_trace_valid = false;
    uint64_t producer_count = 0U;
    uint32_t producer_crc = 0U;
    uint8_t producer_sha[NP2_SHA256_DIGEST_SIZE]{};
};

static void record_failure(RunContext *ctx, FailureStage stage)
{
    FailureStage expected = FailureStage::None;
    (void)ctx->failure_stage.compare_exchange_strong(
        expected, stage, std::memory_order_acq_rel,
        std::memory_order_acquire);
    ctx->failed.store(true, std::memory_order_release);
}

static const char *failure_stage_name(FailureStage stage)
{
    switch (stage) {
    case FailureStage::Preflight: return "preflight";
    case FailureStage::TimingAlloc: return "timing_alloc";
    case FailureStage::AtomicGate: return "atomic_gate";
    case FailureStage::WorkerInit: return "worker_init";
    case FailureStage::DoneCreate: return "done_create";
    case FailureStage::WorkerCreate: return "worker_create";
    case FailureStage::ProducerCreate: return "producer_create";
    case FailureStage::DoneTimeout: return "done_timeout";
    case FailureStage::ProducerFail: return "producer_fail";
    case FailureStage::WorkerFailed: return "worker_failed";
    case FailureStage::ObserverInvariant: return "observer_invariant";
    case FailureStage::FinishIdentity: return "finish_identity";
    case FailureStage::PrintTiming: return "print_timing";
    case FailureStage::None: break;
    }
    return "none";
}

static const uint8_t kRetroEventSha[] = {
    0x89,0x8b,0x04,0x9d,0x1c,0x37,0xc8,0xcc,0x65,0x03,0x75,0x98,0x49,0x24,0x40,0x48,
    0xe0,0xe7,0xf7,0x78,0x08,0x7e,0x1e,0xb5,0x70,0x6b,0xed,0xf1,0x16,0xe9,0xda,0xcf};
static const uint8_t kRetroSourceSha[] = {
    0x70,0x2d,0x8b,0x30,0x03,0xd2,0xd8,0x14,0x49,0xfd,0x10,0x03,0xaa,0x22,0x31,0xaf,
    0xda,0xca,0xae,0x9d,0x68,0x0f,0x73,0xfd,0xf1,0x1d,0x81,0x95,0xed,0xb0,0x46,0xc2};
static const uint8_t kRetroPcmSha[] = {
    0x1d,0x4d,0x24,0xad,0x9c,0x96,0x6d,0xea,0x08,0x56,0x07,0xaf,0xee,0x6a,0x9e,0xcb,
    0x04,0x9c,0x2c,0x47,0x68,0x63,0xc5,0x34,0xdb,0xfe,0x0e,0x50,0xac,0xe1,0x01,0x6b};
static const uint8_t kStressEventSha[] = {
    0x37,0x66,0xb6,0xfd,0x4a,0xcd,0x79,0x9b,0x75,0x17,0xc9,0x03,0xd3,0xd1,0x38,0xb3,
    0x61,0x95,0x91,0x63,0xb4,0xad,0x13,0xca,0x9a,0x6d,0x12,0xba,0x68,0x33,0x37,0x3b};
static const uint8_t kStressPcmSha[] = {
    0x46,0x32,0x66,0x98,0x86,0x93,0x0b,0x31,0x31,0x2b,0x96,0xb2,0x53,0x4e,0xb7,0xc5,
    0x0d,0xba,0x4c,0x14,0x55,0xeb,0x55,0xf4,0xf0,0x61,0xae,0x19,0x78,0x1b,0xb7,0x32};

static const Workload kRetro = {
    "RETROFM", true, NP2_OPNGEN_SYNTHETIC_LIGHT, 0U,
    {1047U, 576960U, 0x3416c2b6U, {}, 0x79b0dfadU, {}}};
static const Workload kStress = {
    "STRESS-60", false, NP2_OPNGEN_SYNTHETIC_STRESS, 60U,
    {41127U, 2880000U, 0x91eac288U, {}, 0x39c7f2d2U, {}}};

static void copy_expected_hashes(Workload *workload)
{
    if (workload->retro) {
        std::memcpy(workload->expected.event_sha, kRetroEventSha,
                    sizeof(kRetroEventSha));
        std::memcpy(workload->expected.pcm_sha, kRetroPcmSha,
                    sizeof(kRetroPcmSha));
    } else {
        std::memcpy(workload->expected.event_sha, kStressEventSha,
                    sizeof(kStressEventSha));
        std::memcpy(workload->expected.pcm_sha, kStressPcmSha,
                    sizeof(kStressPcmSha));
    }
}

static uint32_t elapsed_us(int64_t start, int64_t end)
{
    const int64_t delta = end - start;
    return delta <= 0 ? 0U : delta > UINT32_MAX ? UINT32_MAX
                                                : static_cast<uint32_t>(delta);
}

static void add_us(uint32_t *value, uint32_t delta)
{
    if (*value > UINT32_MAX - delta) *value = UINT32_MAX;
    else *value += delta;
}

static void add_startup(Timing *timing, uint32_t delta)
{
    timing->startup_active_us += delta;
}

static void *alloc_timing_array(RunContext *ctx, size_t count, size_t size)
{
    void *memory = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL);
    const char *placement = memory != nullptr ? "internal" : nullptr;
    if (memory == nullptr) {
        memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
        placement = memory != nullptr ? "psram" : nullptr;
    }
    if (placement != nullptr) {
        if (std::strcmp(ctx->timing.arrays_placement, "unallocated") == 0)
            ctx->timing.arrays_placement = placement;
        else if (std::strcmp(ctx->timing.arrays_placement, placement) != 0)
            ctx->timing.arrays_placement = "mixed";
    }
    return memory;
}

static void observer_dequeue_begin(void *opaque)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    ctx->timing.dequeue_start = esp_timer_get_time();
}

static void observer_dequeue_end(void *opaque, int status,
                                 const struct np2opngen_synth_event *)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    if (status == NP2_OPNGEN_SPSC_OK)
        ctx->timing.pending_dequeue_us =
            elapsed_us(ctx->timing.dequeue_start, esp_timer_get_time());
}

static void observer_event_begin(void *opaque,
                                 const struct np2opngen_synth_event *event,
                                 bool zero)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    ctx->timing.event_start = esp_timer_get_time();
    ctx->timing.event_quantum = event->sample_timestamp / kQuantum;
    ctx->timing.event_zero = zero;
    if (zero) ++ctx->timing.startup_zero_events;
    if (ctx->timing.pending_dequeue_us != 0U) {
        if (zero) add_startup(&ctx->timing, ctx->timing.pending_dequeue_us);
        else if (ctx->timing.event_quantum < ctx->timing.quanta)
            add_us(&ctx->timing.full[ctx->timing.event_quantum],
                   ctx->timing.pending_dequeue_us);
        ctx->timing.pending_dequeue_us = 0U;
    }
}

static void observer_event_end(void *opaque,
                               const struct np2opngen_synth_event *, int)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    const uint32_t delta = elapsed_us(ctx->timing.event_start,
                                      esp_timer_get_time());
    if (ctx->timing.event_zero) add_startup(&ctx->timing, delta);
    else if (ctx->timing.event_quantum < ctx->timing.quanta)
        add_us(&ctx->timing.full[ctx->timing.event_quantum], delta);
}

static void observer_apply_begin(void *opaque,
                                 const struct np2opngen_synth_event *event)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    ctx->timing.apply_start = esp_timer_get_time();
    ctx->timing.apply_quantum = event->sample_timestamp / kQuantum;
    ctx->timing.apply_zero = event->sample_timestamp == 0U;
}

static void observer_apply_end(void *opaque,
                               const struct np2opngen_synth_event *, int)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    const uint32_t delta = elapsed_us(ctx->timing.apply_start,
                                      esp_timer_get_time());
    if (ctx->timing.apply_zero) add_startup(&ctx->timing, delta);
    else if (ctx->timing.apply_quantum < ctx->timing.quanta)
        add_us(&ctx->timing.full[ctx->timing.apply_quantum], delta);
}

static void observer_render_begin(void *opaque, uint64_t offset,
                                  uint32_t count)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    ctx->timing.render_start = esp_timer_get_time();
    ctx->timing.render_quantum = offset / kQuantum;
    if (ctx->timing.render_quantum < ctx->timing.quanta) {
        ctx->timing.seen[ctx->timing.render_quantum] = 1U;
        if (count == 0U || count > kQuantum ||
            offset % kQuantum + count > kQuantum)
            record_failure(ctx, FailureStage::ObserverInvariant);
    } else {
        record_failure(ctx, FailureStage::ObserverInvariant);
    }
}

static void observer_opngen_begin(void *opaque, uint64_t, uint32_t)
{
    static_cast<RunContext *>(opaque)->timing.opngen_start = esp_timer_get_time();
}

static void observer_opngen_end(void *opaque, uint64_t offset, uint32_t,
                                int)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    const size_t q = static_cast<size_t>(offset / kQuantum);
    if (q < ctx->timing.quanta)
        add_us(&ctx->timing.pure[q],
               elapsed_us(ctx->timing.opngen_start, esp_timer_get_time()));
}

static void observer_render_end(void *opaque, uint64_t offset, uint32_t,
                                int status)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    const size_t q = static_cast<size_t>(offset / kQuantum);
    if (q < ctx->timing.quanta)
        add_us(&ctx->timing.full[q],
               elapsed_us(ctx->timing.render_start, esp_timer_get_time()));
    if (status != 0) record_failure(ctx, FailureStage::ObserverInvariant);
}

static void observer_quantum_complete(void *opaque, uint64_t, uint32_t)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    ++ctx->completed_quanta;
    if (ctx->completed_quanta % kHousekeepingQuantumInterval == 0U)
        vTaskDelay(kHousekeepingDelayTicks);
}

static int sink_write(const uint8_t *pcm, size_t bytes, uint64_t offset,
                      void *opaque)
{
    auto *sink = static_cast<Sink *>(opaque);
    if (pcm == nullptr || bytes % 4U != 0U || offset != sink->frames)
        return -1;
    sink->crc = np2_crc32_iso_hdlc_update(sink->crc, pcm, bytes);
    if (sink->sha_enabled) np2_sha256_update(&sink->sha, pcm, bytes);
    sink->frames += bytes / 4U;
    sink->bytes += bytes;
    return 0;
}

static void producer_fail(RunContext *ctx, enum np2opngen_e1b_error error)
{
    record_failure(ctx, FailureStage::ProducerFail);
    np2opngen_e1b_control_fail(&ctx->control, error);
}

static bool enqueue_event(RunContext *ctx,
                          const struct np2opngen_synth_event *event)
{
    for (;;) {
        const int status = np2opngen_spsc_enqueue(&ctx->queue, event);
        if (status == NP2_OPNGEN_SPSC_OK) {
            if (np2opngen_synth_event_trace_update(&ctx->producer_trace,
                                                   event) !=
                NP2_SYNTH_EVENT_STATUS_OK) {
                producer_fail(ctx, NP2_OPNGEN_E1B_ERROR_EVENT);
                return false;
            }
            ++ctx->producer_count;
            if (ctx->worker_task != nullptr) xTaskNotifyGive(ctx->worker_task);
            return true;
        }
        if (status != NP2_OPNGEN_SPSC_FULL) {
            producer_fail(ctx, NP2_OPNGEN_E1B_ERROR_QUEUE);
            return false;
        }
        /* Publish the wait state before rechecking occupancy.  If the worker
         * dequeues in either race window, its counting notification remains
         * pending and ulTaskNotifyTake cannot miss the queue-space transition. */
        ctx->producer_waiting.store(true, std::memory_order_release);
        if (np2opngen_spsc_occupancy(&ctx->queue) <
            NP2_OPNGEN_SPSC_CAPACITY) {
            ctx->producer_waiting.store(false, std::memory_order_release);
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ctx->producer_waiting.store(false, std::memory_order_release);
        if (np2opngen_e1b_control_first_error(&ctx->control) !=
            NP2_OPNGEN_E1B_ERROR_NONE) return false;
    }
}

static void producer_task(void *opaque)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    bool ok = true;
    np2opngen_synth_event_trace_init(&ctx->producer_trace);
    if (ctx->workload->retro) {
        struct np2opngen_s98_parser parser{};
        const uint8_t *start = _binary_retrofm_pocket_demo_strict_s98_start;
        const size_t size = static_cast<size_t>(
            _binary_retrofm_pocket_demo_strict_s98_end - start);
        if (np2opngen_s98_parser_init(&parser, start, size) != 0) ok = false;
        while (ok) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_s98_parser_next(&parser, &event);
            if (next == NP2_OPNGEN_S98_NEXT_END) break;
            if (next != NP2_OPNGEN_S98_NEXT_EVENT || !enqueue_event(ctx, &event)) {
                ok = false;
                break;
            }
        }
    } else {
        struct np2opngen_synthetic_workload generator{};
        if (np2opngen_synthetic_workload_init(
                &generator, ctx->workload->profile,
                ctx->workload->duration_seconds) != 0) ok = false;
        while (ok) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_synthetic_workload_peek(&generator, &event);
            if (next == 0) break;
            if (next < 0 || !enqueue_event(ctx, &event) ||
                np2opngen_synthetic_workload_commit(&generator) != 0) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) producer_fail(ctx, NP2_OPNGEN_E1B_ERROR_GENERATOR);
    ctx->producer_trace_valid = ok;
    np2opngen_e1b_control_producer_done(&ctx->control);
    if (ctx->worker_task != nullptr) xTaskNotifyGive(ctx->worker_task);
    vTaskDelete(nullptr);
}

static void worker_task(void *opaque)
{
    auto *ctx = static_cast<RunContext *>(opaque);
    for (;;) {
        const int step = np2opngen_e1b_worker_step(&ctx->worker);
        if (step == NP2_OPNGEN_E1B_STEP_PROGRESS &&
            ctx->producer_task != nullptr &&
            ctx->producer_waiting.load(std::memory_order_acquire)) {
            xTaskNotifyGive(ctx->producer_task);
        }
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (step == NP2_OPNGEN_E1B_STEP_FAILED)
            record_failure(ctx, FailureStage::WorkerFailed);
        if (step == NP2_OPNGEN_E1B_STEP_COMPLETE ||
            step == NP2_OPNGEN_E1B_STEP_FAILED) break;
    }
    if (ctx->producer_task != nullptr &&
        ctx->producer_waiting.load(std::memory_order_acquire)) {
        xTaskNotifyGive(ctx->producer_task);
    }
    if (ctx->done != nullptr) xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}

static bool digest_equal(const uint8_t *a, const uint8_t *b)
{
    return std::memcmp(a, b, NP2_SHA256_DIGEST_SIZE) == 0;
}

static bool digest_equal_explicit(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0U;
    for (size_t i = 0; i < NP2_SHA256_DIGEST_SIZE; ++i)
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0U;
}

static size_t first_digest_mismatch(const uint8_t *a, const uint8_t *b)
{
    for (size_t i = 0; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        if (a[i] != b[i]) return i;
    }
    return NP2_SHA256_DIGEST_SIZE;
}

static void print_hex(const uint8_t *digest);

static void print_failure_record(const RunContext *ctx,
                                 bool worker_done_observed,
                                 bool done_timeout)
{
    const bool producer_done = std::atomic_load_explicit(
        &ctx->control.producer_done, std::memory_order_acquire);
    const bool producer_waiting =
        ctx->producer_waiting.load(std::memory_order_acquire);
    const bool quiescent = worker_done_observed && producer_done;
    const FailureStage stage =
        ctx->failure_stage.load(std::memory_order_acquire);
    const int control_error = np2opngen_e1b_control_first_error(&ctx->control);

    std::printf("P4_AUDIO_FAILURE workload=%s stage=%s snapshot=%s"
                " done_timeout=%u control_error=%d producer_done=%u"
                " producer_waiting=%u",
                ctx->workload->name, failure_stage_name(stage),
                quiescent ? "QUIESCENT" : "UNQUIESCED",
                done_timeout ? 1U : 0U, control_error,
                producer_done ? 1U : 0U, producer_waiting ? 1U : 0U);
    if (quiescent) {
        std::printf(" worker_state=%d worker_failure_status=%d"
                    " producer_trace_valid=%u producer_count=%" PRIu64
                    " worker_dequeue_count=%" PRIu64
                    " expected_sequence=%" PRIu64
                    " last_sequence_valid=%u last_sequence=%" PRIu64
                    " sequence_errors=%" PRIu64 " queue_occupancy=%" PRIu32
                    " cursor=%" PRIu64 " rendered_frames=%" PRIu64
                    " sink_frames=%" PRIu64 " sink_bytes=%" PRIu64
                    " pcm_crc32=0x%08" PRIx32
                    " completed_quanta=%" PRIu64,
                    static_cast<int>(ctx->worker.state),
                    ctx->worker.failure_status,
                    ctx->producer_trace_valid ? 1U : 0U,
                    ctx->producer_count, ctx->worker.dequeue_count,
                    ctx->worker.expected_sequence,
                    ctx->worker.has_last_sequence ? 1U : 0U,
                    ctx->worker.last_sequence, ctx->worker.sequence_errors,
                    np2opngen_spsc_occupancy(&ctx->queue), ctx->worker.cursor,
                    ctx->worker.rendered_frames, ctx->sink.frames,
                    ctx->sink.bytes, np2_crc32_iso_hdlc_finish(ctx->sink.crc),
                    ctx->completed_quanta);
    } else {
        std::printf(" worker_state=na worker_failure_status=na"
                    " producer_trace_valid=na producer_count=na"
                    " worker_dequeue_count=na expected_sequence=na"
                    " last_sequence_valid=na last_sequence=na"
                    " sequence_errors=na queue_occupancy=na cursor=na"
                    " rendered_frames=na sink_frames=na sink_bytes=na"
                    " pcm_crc32=na completed_quanta=na");
    }
    std::printf("\n");
}

static bool finish_identity(RunContext *ctx)
{
    uint64_t event_count = 0U;
    uint32_t event_crc = 0U;
    uint8_t event_sha[NP2_SHA256_DIGEST_SIZE]{};
    const bool worker_trace_finish_ok =
        np2opngen_e1b_worker_event_trace_finish(
            &ctx->worker, &event_count, &event_crc, event_sha) == 0;
    uint64_t producer_trace_count = 0U;
    uint32_t producer_trace_crc = 0U;
    uint8_t producer_trace_sha[NP2_SHA256_DIGEST_SIZE]{};
    const bool producer_trace_finish_ok =
        np2opngen_synth_event_trace_finish(
            &ctx->producer_trace, &producer_trace_count,
            &producer_trace_crc, producer_trace_sha) == 0;
    uint8_t pcm_sha[NP2_SHA256_DIGEST_SIZE]{};
    if (ctx->correctness) np2_sha256_final(&ctx->sink.sha, pcm_sha);

    const bool producer_count_match =
        ctx->producer_count == ctx->workload->expected.events;
    const bool consumer_count_match =
        event_count == ctx->workload->expected.events;
    const bool consumer_crc_expected_match =
        event_crc == ctx->workload->expected.event_crc;
    const bool consumer_sha_expected_match =
        digest_equal(event_sha, ctx->workload->expected.event_sha);
    const bool producer_consumer_count_match =
        producer_trace_count == event_count;
    const bool producer_consumer_crc_match =
        producer_trace_crc == event_crc;
    const bool producer_consumer_sha_match =
        digest_equal(producer_trace_sha, event_sha);
    const bool sequence_match = ctx->worker.sequence_errors == 0U;
    const bool pcm_frames_match =
        ctx->sink.frames == ctx->workload->expected.end_frame;
    const bool pcm_bytes_match =
        ctx->sink.bytes == ctx->workload->expected.end_frame * 4U;
    const uint32_t pcm_crc = np2_crc32_iso_hdlc_finish(ctx->sink.crc);
    const bool pcm_crc_expected_match =
        pcm_crc == ctx->workload->expected.pcm_crc;
    const bool pcm_sha_expected_match =
        !ctx->correctness || digest_equal(pcm_sha, ctx->workload->expected.pcm_sha);
    const uint8_t *compiled_pcm_sha = ctx->workload->retro ? kRetroPcmSha : kStressPcmSha;
    const bool actual_expected_memcmp = pcm_sha_expected_match;
    const bool expected_compiled_memcmp =
        !ctx->correctness ||
        digest_equal(ctx->workload->expected.pcm_sha, compiled_pcm_sha);
    const bool actual_compiled_memcmp =
        !ctx->correctness || digest_equal(pcm_sha, compiled_pcm_sha);
    const bool actual_expected_explicit =
        !ctx->correctness ||
        digest_equal_explicit(pcm_sha, ctx->workload->expected.pcm_sha);
    const bool expected_compiled_explicit =
        !ctx->correctness ||
        digest_equal_explicit(ctx->workload->expected.pcm_sha,
                              compiled_pcm_sha);
    const bool actual_compiled_explicit =
        !ctx->correctness || digest_equal_explicit(pcm_sha, compiled_pcm_sha);
    const size_t expected_compiled_first_mismatch =
        ctx->correctness
            ? first_digest_mismatch(ctx->workload->expected.pcm_sha,
                                    compiled_pcm_sha)
            : NP2_SHA256_DIGEST_SIZE;
    const bool producer_loop_valid =
        ctx->producer_trace_valid && event_count == ctx->producer_count;

    const bool identity_match =
        worker_trace_finish_ok && producer_count_match && consumer_count_match &&
        consumer_crc_expected_match && consumer_sha_expected_match &&
        producer_trace_finish_ok && producer_consumer_count_match &&
        producer_consumer_crc_match && producer_consumer_sha_match &&
        sequence_match && pcm_frames_match && pcm_bytes_match &&
        pcm_crc_expected_match && pcm_sha_expected_match && producer_loop_valid;

    const char *first_failure = "none";
    if (!worker_trace_finish_ok) first_failure = "worker_trace_finish";
    else if (!producer_count_match) first_failure = "producer_count";
    else if (!consumer_count_match) first_failure = "consumer_count";
    else if (!consumer_crc_expected_match) first_failure = "consumer_crc";
    else if (!consumer_sha_expected_match) first_failure = "consumer_sha";
    else if (!producer_trace_finish_ok) first_failure = "producer_trace_finish";
    else if (!producer_consumer_count_match) first_failure = "trace_count_equal";
    else if (!producer_consumer_crc_match) first_failure = "trace_crc_equal";
    else if (!producer_consumer_sha_match) first_failure = "trace_sha_equal";
    else if (!sequence_match) first_failure = "sequence";
    else if (!pcm_frames_match) first_failure = "pcm_frames";
    else if (!pcm_bytes_match) first_failure = "pcm_bytes";
    else if (!pcm_crc_expected_match) first_failure = "pcm_crc";
    else if (ctx->correctness && !pcm_sha_expected_match) first_failure = "pcm_sha";
    else if (!producer_loop_valid) first_failure = "producer_loop";

    if (!identity_match && ctx->correctness) {
        std::printf("P4_AUDIO_IDENTITY_DIAG workload=%s first_failure=%s"
                    " worker_trace_finish=%u producer_trace_finish=%u"
                    " consumer_count=%" PRIu64 " consumer_count_match=%u"
                    " consumer_crc32=0x%08" PRIx32 " consumer_crc_match=%u"
                    " producer_count=%" PRIu64 " producer_count_match=%u"
                    " producer_trace_count=%" PRIu64 " trace_count_equal=%u"
                    " producer_crc32=0x%08" PRIx32 " trace_crc_equal=%u"
                    " consumer_sha_match=%u trace_sha_equal=%u"
                    " sequence_errors=%" PRIu64 " sequence_match=%u"
                    " pcm_frames=%" PRIu64 " pcm_frames_match=%u"
                    " pcm_bytes=%" PRIu64 " pcm_bytes_match=%u"
                    " pcm_crc32=0x%08" PRIx32 " pcm_crc_match=%u"
                    " pcm_sha_match=%u producer_trace_valid=%u"
                    " producer_loop_valid=%u identity_match=0"
                    " consumer_event_sha256=",
                    ctx->workload->name, first_failure,
                    worker_trace_finish_ok ? 1U : 0U,
                    producer_trace_finish_ok ? 1U : 0U, event_count,
                    consumer_count_match ? 1U : 0U, event_crc,
                    consumer_crc_expected_match ? 1U : 0U, ctx->producer_count,
                    producer_count_match ? 1U : 0U, producer_trace_count,
                    producer_consumer_count_match ? 1U : 0U, producer_trace_crc,
                    producer_consumer_crc_match ? 1U : 0U,
                    consumer_sha_expected_match ? 1U : 0U,
                    producer_consumer_sha_match ? 1U : 0U,
                    ctx->worker.sequence_errors, sequence_match ? 1U : 0U,
                    ctx->sink.frames, pcm_frames_match ? 1U : 0U,
                    ctx->sink.bytes, pcm_bytes_match ? 1U : 0U, pcm_crc,
                    pcm_crc_expected_match ? 1U : 0U,
                    pcm_sha_expected_match ? 1U : 0U,
                    ctx->producer_trace_valid ? 1U : 0U,
                    producer_loop_valid ? 1U : 0U);
        print_hex(event_sha);
        std::printf(" producer_event_sha256=");
        print_hex(producer_trace_sha);
        std::printf(" pcm_sha256=");
        print_hex(pcm_sha);
        std::printf(" runtime_expected_pcm_sha256=");
        print_hex(ctx->workload->expected.pcm_sha);
        std::printf(" compiled_pcm_sha256=");
        print_hex(compiled_pcm_sha);
        std::printf(" actual_expected_memcmp=%u"
                    " expected_compiled_memcmp=%u"
                    " actual_compiled_memcmp=%u"
                    " actual_expected_explicit=%u"
                    " expected_compiled_explicit=%u"
                    " actual_compiled_explicit=%u"
                    " expected_compiled_first_mismatch=",
                    actual_expected_memcmp ? 1U : 0U,
                    expected_compiled_memcmp ? 1U : 0U,
                    actual_compiled_memcmp ? 1U : 0U,
                    actual_expected_explicit ? 1U : 0U,
                    expected_compiled_explicit ? 1U : 0U,
                    actual_compiled_explicit ? 1U : 0U);
        if (expected_compiled_first_mismatch == NP2_SHA256_DIGEST_SIZE)
            std::printf("none");
        else
            std::printf("%zu", expected_compiled_first_mismatch);
        std::printf("\n");
    }
    if (!identity_match) return false;

    if (ctx->correctness) {
        std::printf("P4_AUDIO_IDENTITY workload=%s event_count=%" PRIu64
                    " event_crc32=0x%08" PRIx32 " event_sha256=",
                    ctx->workload->name, event_count, event_crc);
        print_hex(event_sha);
        std::printf(" sequence_errors=%" PRIu64 " pcm_frames=%" PRIu64
                    " pcm_bytes=%" PRIu64 " pcm_crc32=0x%08" PRIx32
                    " pcm_sha256=", ctx->worker.sequence_errors,
                    ctx->sink.frames, ctx->sink.bytes,
                    np2_crc32_iso_hdlc_finish(ctx->sink.crc));
        print_hex(pcm_sha);
        std::printf("\n");
    } else {
        std::printf("P4_AUDIO_IDENTITY workload=%s event_count=%" PRIu64
                    " event_crc32=0x%08" PRIx32 " sequence_errors=%" PRIu64
                    " pcm_frames=%" PRIu64 " pcm_bytes=%" PRIu64
                    " pcm_crc32=0x%08" PRIx32 " timing_sink=CRC32_ONLY\n",
                    ctx->workload->name, event_count, event_crc,
                    ctx->worker.sequence_errors, ctx->sink.frames,
                    ctx->sink.bytes, np2_crc32_iso_hdlc_finish(ctx->sink.crc));
    }
    ctx->producer_trace_valid = producer_loop_valid;
    return true;
}

static void print_hex(const uint8_t *digest)
{
    for (size_t i = 0; i < NP2_SHA256_DIGEST_SIZE; ++i)
        std::printf("%02x", digest[i]);
}

static uint32_t percentile(const uint32_t *values, size_t count, double p)
{
    if (count == 0U) return 0U;
    const size_t index = static_cast<size_t>(p * static_cast<double>(count - 1U));
    return values[index];
}

static bool print_timing(RunContext *ctx)
{
    if (ctx->timing.quanta == 0U) return false;
    uint32_t *full = static_cast<uint32_t *>(
        heap_caps_malloc(ctx->timing.quanta * sizeof(uint32_t), MALLOC_CAP_INTERNAL));
    uint32_t *pure = static_cast<uint32_t *>(
        heap_caps_malloc(ctx->timing.quanta * sizeof(uint32_t), MALLOC_CAP_INTERNAL));
    if (full == nullptr)
        full = static_cast<uint32_t *>(heap_caps_malloc(
            ctx->timing.quanta * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (pure == nullptr)
        pure = static_cast<uint32_t *>(heap_caps_malloc(
            ctx->timing.quanta * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (full == nullptr || pure == nullptr) {
        if (full != nullptr) heap_caps_free(full);
        if (pure != nullptr) heap_caps_free(pure);
        return false;
    }
    std::memcpy(full, ctx->timing.full, ctx->timing.quanta * sizeof(uint32_t));
    std::memcpy(pure, ctx->timing.pure, ctx->timing.quanta * sizeof(uint32_t));
    std::sort(full, full + ctx->timing.quanta);
    std::sort(pure, pure + ctx->timing.quanta);
    uint64_t total = 0U;
    uint64_t opngen_total = 0U;
    uint32_t max_consecutive = 0U;
    uint32_t over_5000 = 0U;
    uint32_t consecutive = 0U;
    uint32_t max_full = 0U;
    uint32_t max_pure = 0U;
    bool timing_valid = true;
    for (size_t i = 0; i < ctx->timing.quanta; ++i) {
        total += ctx->timing.full[i];
        opngen_total += ctx->timing.pure[i];
        max_full = std::max(max_full, ctx->timing.full[i]);
        max_pure = std::max(max_pure, ctx->timing.pure[i]);
        if (ctx->timing.full[i] > 5000U) {
            ++consecutive;
            ++over_5000;
        }
        else consecutive = 0U;
        max_consecutive = std::max(max_consecutive, consecutive);
        if (ctx->timing.pure[i] > ctx->timing.full[i]) timing_valid = false;
        if (ctx->timing.seen[i] == 0U) timing_valid = false;
    }
    const uint64_t logical_us = ctx->workload->expected.end_frame * 1000000ULL / kRate;
    const uint64_t active_ppm = logical_us == 0U ? 0U : total * 1000000ULL / logical_us;
    const uint64_t opngen_ppm = logical_us == 0U ? 0U : opngen_total * 1000000ULL / logical_us;
    std::printf("P4_AUDIO_STARTUP timestamp_zero_events=%" PRIu64
                " active_us=%" PRIu64 " pure_opngen_us=0\n",
                ctx->timing.startup_zero_events, ctx->timing.startup_active_us);
    std::printf("P4_AUDIO_SERVICE measured_quanta=%zu min=%u mean=%" PRIu64
                " p50=%u p90=%u p95=%u p99=%u max=%u over_5000us=%u max_consecutive=%u\n",
                ctx->timing.quanta, static_cast<unsigned>(full[0]), total / ctx->timing.quanta,
                static_cast<unsigned>(percentile(full, ctx->timing.quanta, .50)),
                static_cast<unsigned>(percentile(full, ctx->timing.quanta, .90)),
                static_cast<unsigned>(percentile(full, ctx->timing.quanta, .95)),
                static_cast<unsigned>(percentile(full, ctx->timing.quanta, .99)),
                static_cast<unsigned>(max_full), static_cast<unsigned>(over_5000),
                static_cast<unsigned>(max_consecutive));
    std::printf("P4_AUDIO_OPNGEN total=%" PRIu64 " mean=%" PRIu64 " p95=%u p99=%u max=%u\n",
                opngen_total, opngen_total / ctx->timing.quanta,
                static_cast<unsigned>(percentile(pure, ctx->timing.quanta, .95)),
                static_cast<unsigned>(percentile(pure, ctx->timing.quanta, .99)),
                static_cast<unsigned>(max_pure));
    std::printf("P4_AUDIO_CPU total_active=%" PRIu64 " logical_audio=%" PRIu64
                " active_ppm=%" PRIu64 " opngen_ppm=%" PRIu64 "\n",
                total, logical_us, active_ppm, opngen_ppm);
    heap_caps_free(full);
    heap_caps_free(pure);
    return timing_valid;
}

static bool preflight_workload(const Workload *workload)
{
    struct np2opngen_synth_event_trace_state trace{};
    uint64_t count = 0U;
    uint32_t crc = 0U;
    uint8_t digest[NP2_SHA256_DIGEST_SIZE]{};
    np2opngen_synth_event_trace_init(&trace);
    if (workload->retro) {
        const uint8_t *source = _binary_retrofm_pocket_demo_strict_s98_start;
        const size_t size = static_cast<size_t>(
            _binary_retrofm_pocket_demo_strict_s98_end - source);
        struct np2opngen_s98_parser parser{};
        np2_sha256_context source_sha{};
        uint8_t source_digest[NP2_SHA256_DIGEST_SIZE]{};
        np2_sha256_init(&source_sha);
        np2_sha256_update(&source_sha, source, size);
        np2_sha256_final(&source_sha, source_digest);
        if (size != 3753U || !digest_equal(source_digest, kRetroSourceSha) ||
            np2opngen_s98_parser_init(&parser, source, size) != 0)
            return false;
        for (;;) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_s98_parser_next(&parser, &event);
            if (next == NP2_OPNGEN_S98_NEXT_END) break;
            if (next != NP2_OPNGEN_S98_NEXT_EVENT) return false;
        }
        if (parser.metadata.end_frame != workload->expected.end_frame ||
            np2opngen_s98_parser_event_trace_finish(
                &parser, &count, &crc, digest) != 0)
            return false;
    } else {
        struct np2opngen_synthetic_workload generator{};
        if (np2opngen_synthetic_workload_init(
                &generator, workload->profile,
                workload->duration_seconds) != 0)
            return false;
        for (;;) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_synthetic_workload_peek(&generator, &event);
            if (next == 0) break;
            if (next < 0 || np2opngen_synth_event_trace_update(&trace, &event) != 0 ||
                np2opngen_synthetic_workload_commit(&generator) != 0)
                return false;
        }
        if (np2opngen_synth_event_trace_finish(&trace, &count, &crc, digest) != 0)
            return false;
    }
    return count == workload->expected.events && crc == workload->expected.event_crc &&
           digest_equal(digest, workload->expected.event_sha);
}

static bool run_once(const Workload *workload, bool correctness)
{
    RunContext ctx{};
    ctx.workload = workload;
    ctx.correctness = correctness;
    bool worker_done_observed = false;
    bool done_timeout = false;
    bool failed = false;
    np2opngen_e1b_control_init(&ctx.control);
    if (!preflight_workload(workload)) {
        record_failure(&ctx, FailureStage::Preflight);
        print_failure_record(&ctx, worker_done_observed, done_timeout);
        std::printf("P4_AUDIO_RESULT workload=%s identity=FAIL characterization=INVALID performance_valid=NO\n",
                    workload->name);
        return false;
    }
    ctx.timing.quanta = static_cast<size_t>((workload->expected.end_frame + kQuantum - 1U) / kQuantum);
    ctx.timing.full = static_cast<uint32_t *>(
        alloc_timing_array(&ctx, ctx.timing.quanta, sizeof(uint32_t)));
    ctx.timing.pure = static_cast<uint32_t *>(
        alloc_timing_array(&ctx, ctx.timing.quanta, sizeof(uint32_t)));
    ctx.timing.seen = static_cast<uint8_t *>(
        alloc_timing_array(&ctx, ctx.timing.quanta, sizeof(uint8_t)));
    if (ctx.timing.full == nullptr || ctx.timing.pure == nullptr ||
        ctx.timing.seen == nullptr) {
        record_failure(&ctx, FailureStage::TimingAlloc);
        std::printf("P4_AUDIO_META workload=%s arrays_placement=allocation_failed\n",
                    workload->name);
        print_failure_record(&ctx, worker_done_observed, done_timeout);
        if (ctx.timing.full) heap_caps_free(ctx.timing.full);
        if (ctx.timing.pure) heap_caps_free(ctx.timing.pure);
        if (ctx.timing.seen) heap_caps_free(ctx.timing.seen);
        std::printf("P4_AUDIO_RESULT workload=%s identity=FAIL"
                    " characterization=INVALID performance_valid=NO\n",
                    workload->name);
        return false;
    }
    std::printf("P4_AUDIO_META workload=%s timing_sink=%s observer=boundary_limited"
                " timing_arrays_bytes=%zu arrays_placement=%s\n",
                workload->name,
                correctness ? "CRC32_SHA256" : "CRC32_ONLY",
                ctx.timing.quanta * (sizeof(uint32_t) * 2U + sizeof(uint8_t)),
                ctx.timing.arrays_placement);
    np2opngen_spsc_init(&ctx.queue);
    bool head_lock_free = false;
    bool tail_lock_free = false;
    (void)np2opngen_spsc_atomic_lock_free(&ctx.queue, &head_lock_free,
                                          &tail_lock_free);
    ctx.atomic_lock_free = head_lock_free && tail_lock_free;
    std::printf("P4_AUDIO_ATOMIC head_size=%zu tail_size=%zu head_lock_free=%s"
                " tail_lock_free=%s codegen_audit_expected=LOCK_FREE_SPSC\n",
                sizeof(ctx.queue.head), sizeof(ctx.queue.tail),
                head_lock_free ? "PASS" : "FAIL", tail_lock_free ? "PASS" : "FAIL");
    if (!ctx.atomic_lock_free) {
        record_failure(&ctx, FailureStage::AtomicGate);
        goto cleanup;
    }
    np2_sha256_init(&ctx.sink.sha);
    ctx.sink.sha_enabled = correctness;
    {
        const struct np2opngen_e1b_pcm_sink sink{sink_write, &ctx.sink};
        if (np2opngen_e1b_worker_init_with_sink(
                &ctx.worker, &ctx.queue, &ctx.control, workload->expected.end_frame,
                0U, workload->expected.events, &sink) != 0) {
            record_failure(&ctx, FailureStage::WorkerInit);
            goto cleanup;
        }
    }
    ctx.observer = {
        observer_dequeue_begin, observer_dequeue_end, observer_event_begin,
        observer_event_end, observer_apply_begin, observer_apply_end,
        observer_render_begin, observer_opngen_begin, observer_opngen_end,
        observer_render_end, observer_quantum_complete, &ctx, true};
    np2opngen_e1b_worker_set_observer(&ctx.worker, &ctx.observer);
    ctx.done = xSemaphoreCreateBinary();
    if (ctx.done == nullptr) {
        record_failure(&ctx, FailureStage::DoneCreate);
        goto cleanup;
    }
    if (xTaskCreatePinnedToCore(worker_task, "p4_audio_worker", 8192, &ctx,
                                kWorkerPriority, &ctx.worker_task, kWorkerCore) != pdPASS) {
        record_failure(&ctx, FailureStage::WorkerCreate);
        goto cleanup;
    }
    if (xTaskCreatePinnedToCore(producer_task, "p4_audio_producer", 8192, &ctx,
                                kProducerPriority, &ctx.producer_task, kProducerCore) != pdPASS) {
        record_failure(&ctx, FailureStage::ProducerCreate);
        goto cleanup;
    }
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(120000)) != pdTRUE) {
        done_timeout = true;
        record_failure(&ctx, FailureStage::DoneTimeout);
    } else {
        worker_done_observed = true;
    }
    if (!ctx.failed.load(std::memory_order_acquire) &&
        !finish_identity(&ctx))
        record_failure(&ctx, FailureStage::FinishIdentity);
    if (!ctx.failed.load(std::memory_order_acquire) && !correctness &&
        !print_timing(&ctx))
        record_failure(&ctx, FailureStage::PrintTiming);

cleanup:
    failed = ctx.failed.load(std::memory_order_acquire);
    if (failed) print_failure_record(&ctx, worker_done_observed, done_timeout);
    if (ctx.worker.s32_pcm != nullptr || ctx.worker.canonical_pcm != nullptr ||
        ctx.worker.opngen != nullptr) np2opngen_e1b_worker_destroy(&ctx.worker);
    if (ctx.done != nullptr) vSemaphoreDelete(ctx.done);
    if (ctx.timing.full) heap_caps_free(ctx.timing.full);
    if (ctx.timing.pure) heap_caps_free(ctx.timing.pure);
    if (ctx.timing.seen) heap_caps_free(ctx.timing.seen);
    std::printf("P4_AUDIO_RESULT workload=%s identity=%s characterization=%s performance_valid=%s\n",
                workload->name, failed ? "FAIL" : "PASS",
                failed ? "INVALID" : "COMPLETE",
                (!failed && !correctness) ? "YES" : "NO");
    return !failed;
}

} // namespace

esp_err_t run()
{
    Workload retro = kRetro;
    Workload stress = kStress;
    copy_expected_hashes(&retro);
    copy_expected_hashes(&stress);
    const uint8_t *fixture = _binary_retrofm_pocket_demo_strict_s98_start;
    const size_t fixture_size = static_cast<size_t>(
        _binary_retrofm_pocket_demo_strict_s98_end - fixture);
    np2_sha256_context source_sha{};
    uint8_t source_digest[NP2_SHA256_DIGEST_SIZE]{};
    np2_sha256_init(&source_sha);
    np2_sha256_update(&source_sha, fixture, fixture_size);
    np2_sha256_final(&source_sha, source_digest);
    std::printf("P4_AUDIO_SOURCE fixture=retrofm-pocket-demo-strict.s98 bytes=%zu sha256=",
                fixture_size);
    print_hex(source_digest);
    std::printf("\n");
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    const uint32_t cpu_frequency_hz =
        static_cast<uint32_t>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) * 1000000U;
    std::printf("P4_AUDIO_META environment=%s chip_target=esp32p4 chip_revision=%d idf=%s"
                " cpu_frequency_hz=%u global_optimization=debug audio_optimization=%s"
                " capacity_housekeeping=enabled housekeeping_quantum_interval=%u"
                " housekeeping_delay_ticks=%u producer_full_wait=notification pm_enabled=0"
                " freertos_tick_hz=%d psram_enabled=%s psram_speed_mhz=%d\n",
#if defined(P4_AUDIO_EMU_TEST)
                "ESP_EMU",
#else
                "REAL_P4",
#endif
                chip.revision, esp_get_idf_version(),
                static_cast<unsigned>(cpu_frequency_hz),
                #if defined(P4_NANO_AUDIO_OPT_O2)
                "o2",
                #else
                "debug",
                #endif
                static_cast<unsigned>(kHousekeepingQuantumInterval),
                static_cast<unsigned>(kHousekeepingDelayTicks),
                configTICK_RATE_HZ,
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
                "yes",
#else
                "no",
#endif
#if defined(CONFIG_SPIRAM_SPEED)
                CONFIG_SPIRAM_SPEED
#else
                0
#endif
    );
    std::printf("P4_AUDIO_META sample_rate_hz=%u quantum_frames=%u producer_core=%d producer_priority=%u"
                " worker_core=%d worker_priority=%u observer=boundary_limited spsc_capacity=8\n",
                static_cast<unsigned>(kRate), static_cast<unsigned>(kQuantum), kProducerCore,
                static_cast<unsigned>(kProducerPriority),
                kWorkerCore, static_cast<unsigned>(kWorkerPriority));
    const bool retro_correct = run_once(&retro, true);
    const bool retro_timing = retro_correct && run_once(&retro, false);
    bool ok = retro_correct && retro_timing;
#if defined(P4_AUDIO_EMU_TEST)
    std::printf("P4_AUDIO_EMU_STRESS=SKIPPED reason=performance_not_valid_in_emulator\n");
#else
    const bool stress_correct = retro_correct && run_once(&stress, true);
    const bool stress_timing = stress_correct && run_once(&stress, false);
    ok = stress_correct && stress_timing && ok;
#endif
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio_benchmark
