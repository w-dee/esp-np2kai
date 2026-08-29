#include "p4_nano_audio86_capacity/p4_nano_audio86_capacity.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "np2_crc32.h"
#include "np2audio86_fixture.h"
#include "np2opngen_pcm_canonical.h"

namespace p4_nano_audio86_capacity {
namespace {

constexpr uint32_t kQuantumUs = 5000U;
constexpr uint32_t kCoordinatorStack = 6144U;
constexpr uint32_t kProducerStack = 6144U;
constexpr uint32_t kWorkerStack = 8192U;
constexpr UBaseType_t kProducerPriority = tskIDLE_PRIORITY + 3;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 4;
constexpr int kProducerCore = 1;
constexpr int kWorkerCore = 0;
constexpr uint32_t kStackCoordinatorMin = 256U;
constexpr uint32_t kStackProducerMin = 256U;
constexpr uint32_t kStackWorkerMin = 512U;

enum class Mode : uint32_t { Unpaced = 0, PacedFormal = 1 };

#if defined(P4_AUDIO86_PROFILE_MODE)
static uint64_t profile_now_us(void *)
{
    return static_cast<uint64_t>(esp_timer_get_time());
}
#endif

struct Context {
    Mode mode = Mode::PacedFormal;
    struct np2audio86_event plan[NP2_AUDIO86_ASYNC_MAX_EVENTS]{};
    size_t plan_count = 0U;
    struct np2audio86_event_ring events{};
    struct np2audio86_byte_ring pcm_bytes{};
    uint8_t source[NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES]{};
    struct np2audio86_render_state render{};
    struct np2audio86_fixture_result result{};
    np2_sha256_context transport_sha{};
    uint64_t event_pop_count = 0U;
    uint64_t transport_event_count = 0U;
    uint32_t transport_event_crc32 = 0U;
    uint8_t transport_event_sha256[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN]{};
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U]{};
    uint32_t service_us[NP2_AUDIO86_QUANTA]{};
    std::atomic<uint32_t> first_error{0U};
    std::atomic<uint64_t> committed_through_frame{0U};
    std::atomic<uint32_t> published_events{0U};
    std::atomic<uint32_t> pace_tick_count{0U};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> worker_done{false};
    std::atomic<bool> producer_waiting{false};
    std::atomic<bool> worker_waiting{false};
    TaskHandle_t producer_task = nullptr;
    TaskHandle_t worker_task = nullptr;
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t producer_start = nullptr;
    SemaphoreHandle_t prefill = nullptr;
    SemaphoreHandle_t worker_start = nullptr;
    SemaphoreHandle_t terminal = nullptr;
    SemaphoreHandle_t pacing = nullptr;
    esp_timer_handle_t timer = nullptr;
    uint32_t event_waits = 0U;
    uint32_t byte_waits = 0U;
    uint32_t worker_waits = 0U;
    uint32_t event_high_water = 0U;
    uint32_t byte_high_water = 0U;
    uint32_t split_count = 0U;
    uint32_t split_quanta[4]{};
    uint32_t split_times[4]{};
    uint32_t refill_quanta[NP2_AUDIO86_QUANTA]{};
    uint32_t refill_times[NP2_AUDIO86_QUANTA]{};
    uint32_t refill_count = 0U;
    uint32_t nonrefill_count = 0U;
    uint32_t deadline_misses = 0U;
    uint32_t pacing_backlog = 0U;
    uint32_t input_starvation = 0U;
    uint64_t start_us = 0U;
    uint64_t finish_us = 0U;
    uint32_t source_setup_us = 0U;
    uint32_t allocation_us = 0U;
    uint32_t generator_init_us = 0U;
    uint32_t prefill_us = 0U;
    uint32_t timer_setup_us = 0U;
    uint32_t coordinator_hwm = 0U;
    std::atomic<uint32_t> producer_hwm{0U};
    std::atomic<uint32_t> worker_hwm{0U};
    uint32_t first_quantum_service_us = 0U;
#if defined(P4_AUDIO86_PROFILE_MODE)
    uint64_t profile_event_apply_us = 0U;
    uint64_t profile_pcm86_copy_us = 0U;
    uint64_t profile_opngen_us = 0U;
    uint64_t profile_psggen_us = 0U;
    uint64_t profile_rhythm_us = 0U;
    uint64_t profile_pcm86_generation_us = 0U;
    uint64_t profile_mix_canonical_us = 0U;
    uint64_t profile_hash_sink_us = 0U;
#endif
};

static void notify(TaskHandle_t task)
{
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

static void fail(Context *ctx, uint32_t error)
{
    if (ctx == nullptr || error == 0U) {
        return;
    }
    uint32_t expected = 0U;
    if (ctx->first_error.compare_exchange_strong(expected, error,
                                                  std::memory_order_acq_rel)) {
        notify(ctx->producer_task);
        notify(ctx->worker_task);
        if (ctx->producer_start != nullptr) {
            xSemaphoreGive(ctx->producer_start);
        }
        if (ctx->worker_start != nullptr) {
            xSemaphoreGive(ctx->worker_start);
        }
        if (ctx->pacing != nullptr) {
            xSemaphoreGive(ctx->pacing);
        }
        if (ctx->terminal != nullptr) {
            xSemaphoreGive(ctx->terminal);
        }
    }
}

static bool wait_event_space(Context *ctx, const struct np2audio86_event *event)
{
    for (;;) {
        if (ctx->first_error.load(std::memory_order_acquire) != 0U) {
            return false;
        }
        const int status = np2audio86_event_ring_enqueue(&ctx->events, event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            const uint32_t high = np2audio86_event_ring_occupancy(&ctx->events);
            ctx->event_high_water = std::max(ctx->event_high_water, high);
            notify(ctx->worker_task);
            return true;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT);
            return false;
        }
        ++ctx->event_waits;
        ctx->producer_waiting.store(true, std::memory_order_release);
        if (np2audio86_event_ring_occupancy(&ctx->events) <
            NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
            ctx->producer_waiting.store(false, std::memory_order_release);
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ctx->producer_waiting.store(false, std::memory_order_release);
    }
}

static bool wait_byte_space(Context *ctx, const uint8_t *bytes, size_t count)
{
    for (;;) {
        if (ctx->first_error.load(std::memory_order_acquire) != 0U) {
            return false;
        }
        const int status = np2audio86_byte_ring_push(&ctx->pcm_bytes, bytes, count);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            const uint32_t high = np2audio86_byte_ring_occupancy(&ctx->pcm_bytes);
            ctx->byte_high_water = std::max(ctx->byte_high_water, high);
            notify(ctx->worker_task);
            return true;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT);
            return false;
        }
        ++ctx->byte_waits;
        ctx->producer_waiting.store(true, std::memory_order_release);
        if (np2audio86_byte_ring_occupancy(&ctx->pcm_bytes) + count <=
            NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
            ctx->producer_waiting.store(false, std::memory_order_release);
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ctx->producer_waiting.store(false, std::memory_order_release);
    }
}

static bool wait_event(Context *ctx, const struct np2audio86_event **event)
{
    for (;;) {
        const int status = np2audio86_event_ring_peek(&ctx->events, event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            return true;
        }
        if (status != NP2_AUDIO86_TRANSPORT_EMPTY) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT);
            return false;
        }
        if (ctx->first_error.load(std::memory_order_acquire) != 0U) {
            return false;
        }
        ++ctx->worker_waits;
        ctx->worker_waiting.store(true, std::memory_order_release);
        const uint64_t watermark =
            ctx->committed_through_frame.load(std::memory_order_acquire);
        if (watermark > ctx->render.rendered_frames ||
            ctx->producer_done.load(std::memory_order_acquire)) {
            ctx->worker_waiting.store(false, std::memory_order_release);
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ctx->worker_waiting.store(false, std::memory_order_release);
    }
}

static bool pop_bytes(Context *ctx, size_t count)
{
    const uint64_t copy_begin =
#if defined(P4_AUDIO86_PROFILE_MODE)
        esp_timer_get_time();
#else
        0U;
#endif
    const int status = np2audio86_byte_ring_pop(&ctx->pcm_bytes, ctx->worker_run,
                                                count);
    if (status != NP2_AUDIO86_TRANSPORT_OK) {
        if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
            ++ctx->worker_waits;
        }
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_DATA_AVAILABILITY);
        return false;
    }
    notify(ctx->producer_task);
    const bool pushed = np2audio86_render_pcm86_push(
        &ctx->render, ctx->worker_run, count) == 0;
#if defined(P4_AUDIO86_PROFILE_MODE)
    ctx->profile_pcm86_copy_us +=
        static_cast<uint64_t>(esp_timer_get_time()) - copy_begin;
#endif
    return pushed;
}

static bool apply_event(Context *ctx, const struct np2audio86_event *event)
{
    if (event == nullptr || event->sequence != ctx->event_pop_count ||
        event->frame_timestamp != ctx->render.rendered_frames) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_SEQUENCE);
        return false;
    }
    const uint64_t apply_begin =
#if defined(P4_AUDIO86_PROFILE_MODE)
        esp_timer_get_time();
#else
        0U;
#endif
    if (event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
        if (event->payload == 0U || event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            (event->payload & 3U) != 0U || !pop_bytes(ctx, event->payload)) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_DATA_LENGTH);
            return false;
        }
        ++ctx->refill_count;
    } else if (event->opcode == NP2_AUDIO86_EVENT_FM_KEY ||
               event->opcode == NP2_AUDIO86_EVENT_PSG_REGISTER) {
        if (np2audio86_render_apply_event(&ctx->render, event) != 0) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_PAYLOAD);
            return false;
        }
    } else {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_OPCODE);
        return false;
    }
#if defined(P4_AUDIO86_PROFILE_MODE)
    ctx->profile_event_apply_us +=
        static_cast<uint64_t>(esp_timer_get_time()) - apply_begin;
#endif
    uint8_t record[24]{};
    std::memcpy(record, &event->frame_timestamp, sizeof(uint64_t));
    std::memcpy(record + 8U, &event->sequence, sizeof(uint64_t));
    std::memcpy(record + 16U, &event->opcode, sizeof(uint32_t));
    std::memcpy(record + 20U, &event->payload, sizeof(uint32_t));
    ctx->transport_event_crc32 = np2_crc32_iso_hdlc_update(
        ctx->transport_event_crc32, record, sizeof(record));
    np2_sha256_update(&ctx->transport_sha, record, sizeof(record));
    ++ctx->transport_event_count;
    if (np2audio86_event_ring_consume(&ctx->events) != NP2_AUDIO86_TRANSPORT_OK) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT);
        return false;
    }
    ++ctx->event_pop_count;
    notify(ctx->producer_task);
    return true;
}

/* The SHA context is kept separately from the public result digest. */
struct HashState {
    np2_sha256_context pcm;
};

static bool render_quantum(Context *ctx, uint32_t quantum, HashState *hash,
                           uint64_t t0_us)
{
    const uint64_t quantum_start = ctx->render.rendered_frames;
    const uint64_t quantum_end = quantum_start + NP2_AUDIO86_QUANTUM_FRAMES;
    bool had_refill = false;
    bool had_split = false;
    const uint64_t service_start = esp_timer_get_time();
    size_t offset = 0U;
    while (offset < NP2_AUDIO86_QUANTUM_FRAMES) {
        const struct np2audio86_event *event = nullptr;
        if (np2audio86_event_ring_peek(&ctx->events, &event) ==
                NP2_AUDIO86_TRANSPORT_OK &&
            event->frame_timestamp <= ctx->render.rendered_frames) {
            had_refill |= event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN;
            if (event->frame_timestamp != ctx->render.rendered_frames ||
                !apply_event(ctx, event)) {
                return false;
            }
            continue;
        }
        uint64_t next = quantum_end;
        if (event != nullptr && event->frame_timestamp < quantum_end) {
            next = event->frame_timestamp;
            had_split = true;
        } else if (event == nullptr &&
                   ctx->committed_through_frame.load(std::memory_order_acquire) <
                       quantum_end &&
                   !ctx->producer_done.load(std::memory_order_acquire)) {
            if (!wait_event(ctx, &event)) {
                return false;
            }
            continue;
        }
        const size_t frames = static_cast<size_t>(next - ctx->render.rendered_frames);
        if (frames != 0U &&
            np2audio86_render_span(&ctx->render,
                                   ctx->render.mix_scratch + offset * 2U, frames,
                                   &ctx->result) != 0) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_PCM86_UNDERRUN);
            return false;
        }
        offset += frames;
        if (offset < NP2_AUDIO86_QUANTUM_FRAMES && frames == 0U) {
            fail(ctx, NP2_AUDIO86_ASYNC_ERROR_LIVENESS);
            return false;
        }
    }
    struct np2opngen_pcm_stats stats{};
    const uint64_t canonical_begin =
#if defined(P4_AUDIO86_PROFILE_MODE)
        esp_timer_get_time();
#else
        0U;
#endif
    if (np2opngen_pcm_canonicalize_s16le(
            ctx->render.mix_scratch, NP2_AUDIO86_QUANTUM_FRAMES,
            NP2_AUDIO86_CHANNELS, ctx->canonical, sizeof(ctx->canonical), &stats) != 0) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_CANONICAL);
        return false;
    }
#if defined(P4_AUDIO86_PROFILE_MODE)
    ctx->profile_mix_canonical_us +=
        static_cast<uint64_t>(esp_timer_get_time()) - canonical_begin;
#endif
    ctx->result.mix_peak_abs = std::max(ctx->result.mix_peak_abs, stats.s32_abs_peak);
    ctx->result.clamped_samples += stats.clip_samples;
    const uint64_t hash_begin =
#if defined(P4_AUDIO86_PROFILE_MODE)
        esp_timer_get_time();
#else
        0U;
#endif
    ctx->result.pcm_crc32 = np2_crc32_iso_hdlc_update(
        ctx->result.pcm_crc32, ctx->canonical, sizeof(ctx->canonical));
    np2_sha256_update(&hash->pcm, ctx->canonical, sizeof(ctx->canonical));
#if defined(P4_AUDIO86_PROFILE_MODE)
    ctx->profile_hash_sink_us +=
        static_cast<uint64_t>(esp_timer_get_time()) - hash_begin;
#endif
    const uint64_t finish = esp_timer_get_time();
    ctx->service_us[quantum] = static_cast<uint32_t>(finish - service_start);
    if (had_split && ctx->split_count < 4U) {
        ctx->split_quanta[ctx->split_count] = quantum;
        ctx->split_times[ctx->split_count] = ctx->service_us[quantum];
        ++ctx->split_count;
    }
    if (had_refill && ctx->refill_count < NP2_AUDIO86_QUANTA) {
        ctx->refill_quanta[ctx->refill_count - 1U] = quantum;
        ctx->refill_times[ctx->refill_count - 1U] = ctx->service_us[quantum];
    }
    const uint64_t deadline = t0_us + (static_cast<uint64_t>(quantum) + 1U) * kQuantumUs;
    if (ctx->mode == Mode::PacedFormal && finish >= deadline) {
        ++ctx->deadline_misses;
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_LIVENESS);
        return false;
    }
    (void)quantum_start;
    return true;
}

static void producer_task(void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    xSemaphoreTake(ctx->producer_start, portMAX_DELAY);
    const uint64_t prefill_start = esp_timer_get_time();
    uint32_t published = 0U;
    for (size_t i = 0U; i < ctx->plan_count &&
                       ctx->first_error.load(std::memory_order_acquire) == 0U;) {
        const uint64_t frame = ctx->plan[i].frame_timestamp;
        do {
            const auto event = ctx->plan[i];
            if (event.opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN &&
                !wait_byte_space(ctx, ctx->source, event.payload)) {
                break;
            }
            if (!wait_event_space(ctx, &event)) {
                break;
            }
            ++published;
            ctx->published_events.store(published, std::memory_order_release);
            ++i;
        } while (i < ctx->plan_count && ctx->plan[i].frame_timestamp == frame);
        const uint64_t next = i < ctx->plan_count ? ctx->plan[i].frame_timestamp
                                                   : NP2_AUDIO86_DURATION_FRAMES;
        ctx->committed_through_frame.store(next, std::memory_order_release);
        notify(ctx->worker_task);
        if (published != 0U) {
            xSemaphoreGive(ctx->prefill);
        }
    }
    ctx->producer_done.store(true, std::memory_order_release);
    ctx->committed_through_frame.store(NP2_AUDIO86_DURATION_FRAMES,
                                       std::memory_order_release);
    notify(ctx->worker_task);
    ctx->prefill_us = static_cast<uint32_t>(esp_timer_get_time() - prefill_start);
    xSemaphoreGive(ctx->terminal);
    ctx->producer_hwm.store(uxTaskGetStackHighWaterMark(nullptr),
                            std::memory_order_release);
    vTaskDelete(nullptr);
}

static void worker_task(void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    np2_sha256_context pcm_sha{};
    np2_sha256_init(&pcm_sha);
    const uint64_t generator_init_start = esp_timer_get_time();
    if (np2audio86_render_init_with_source(&ctx->render, ctx->source) != 0) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH);
    }
#if defined(P4_AUDIO86_PROFILE_MODE)
    np2audio86_render_set_profile_clock(&ctx->render, profile_now_us, nullptr);
#endif
    ctx->result.pcm_crc32 = np2_crc32_iso_hdlc_init();
    ctx->transport_event_crc32 = np2_crc32_iso_hdlc_init();
    ctx->generator_init_us = static_cast<uint32_t>(esp_timer_get_time() - generator_init_start);
    xSemaphoreGive(ctx->ready);
    xSemaphoreTake(ctx->worker_start, portMAX_DELAY);
    const uint64_t t0 = esp_timer_get_time();
    ctx->start_us = t0;
    for (uint32_t q = 0U; q < NP2_AUDIO86_QUANTA &&
                           ctx->first_error.load(std::memory_order_acquire) == 0U;
         ++q) {
        if (ctx->mode == Mode::PacedFormal && q != 0U) {
            xSemaphoreTake(ctx->pacing, portMAX_DELAY);
            const uint32_t ticks = ctx->pace_tick_count.load(std::memory_order_acquire);
            if (ticks != q) {
                ++ctx->pacing_backlog;
                fail(ctx, NP2_AUDIO86_ASYNC_ERROR_LIVENESS);
                break;
            }
            const uint64_t start = t0 + static_cast<uint64_t>(q) * kQuantumUs;
            while (esp_timer_get_time() < start) {
                const uint64_t remaining = start - esp_timer_get_time();
                if (remaining > 50U) {
                    vTaskDelay(1);
                }
            }
        }
        HashState hash{};
        hash.pcm = pcm_sha;
        if (!render_quantum(ctx, q, &hash, t0)) {
            break;
        }
        pcm_sha = hash.pcm;
    }
    np2_sha256_final(&pcm_sha, ctx->result.pcm_sha256);
    np2_sha256_final(&ctx->transport_sha, ctx->transport_event_sha256);
    ctx->result.frames = NP2_AUDIO86_DURATION_FRAMES;
    ctx->result.bytes = NP2_AUDIO86_PCM_BYTES;
    ctx->result.quanta = NP2_AUDIO86_QUANTA;
    ctx->result.pcm86_bytes_supplied = ctx->render.pcm86.supplied;
    ctx->result.pcm86_bytes_consumed = ctx->render.pcm86.supplied -
                                       static_cast<uint64_t>(ctx->render.pcm86.pcm.realbuf);
    ctx->result.pcm86_refills = ctx->render.pcm86.refills;
    ctx->result.pcm86_fifo_min = ctx->render.pcm86.fifo_min;
    ctx->result.pcm86_fifo_max = ctx->render.pcm86.fifo_max;
    ctx->result.pcm86_fifo_underrun = ctx->render.pcm86.underrun;
    ctx->finish_us = esp_timer_get_time();
    ctx->worker_done.store(true, std::memory_order_release);
    xSemaphoreGive(ctx->terminal);
    ctx->worker_hwm.store(uxTaskGetStackHighWaterMark(nullptr),
                          std::memory_order_release);
    vTaskDelete(nullptr);
}

static void timer_callback(void *arg)
{
    auto *ctx = static_cast<Context *>(arg);
    if (ctx == nullptr) {
        return;
    }
    ctx->pace_tick_count.fetch_add(1U, std::memory_order_acq_rel);
    xSemaphoreGive(ctx->pacing);
    notify(ctx->worker_task);
}

static uint32_t percentile(const uint32_t *values, size_t count, double p)
{
    if (count == 0U) {
        return 0U;
    }
    const size_t index = static_cast<size_t>((p * static_cast<double>(count)) +
                                             0.999999999) - 1U;
    return values[std::min(index, count - 1U)];
}

static void subset_stats(const Context *ctx, bool refill, size_t *count,
                         uint32_t *p99, uint32_t *max);

static void print_hex(const uint8_t *digest)
{
    for (size_t i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        std::printf("%02x", digest[i]);
    }
}

static void print_timing(Context *ctx)
{
    uint32_t refill_p99 = 0U;
    uint32_t refill_max = 0U;
    uint32_t nonrefill_p99 = 0U;
    uint32_t nonrefill_max = 0U;
    size_t refill_samples = 0U;
    size_t nonrefill_samples = 0U;
    subset_stats(ctx, true, &refill_samples, &refill_p99, &refill_max);
    subset_stats(ctx, false, &nonrefill_samples, &nonrefill_p99, &nonrefill_max);
#if defined(P4_AUDIO86_PROFILE_MODE)
    ctx->profile_opngen_us = ctx->render.profile_opngen_us;
    ctx->profile_psggen_us = ctx->render.profile_psggen_us;
    ctx->profile_rhythm_us = ctx->render.profile_rhythm_us;
    ctx->profile_pcm86_generation_us = ctx->render.profile_pcm86_generation_us;
    ctx->profile_mix_canonical_us += ctx->render.profile_mix_us;
#endif
    uint32_t *sorted = ctx->service_us;
    std::sort(sorted, sorted + NP2_AUDIO86_QUANTA);
    uint64_t sum = 0U;
    for (size_t i = 0U; i < NP2_AUDIO86_QUANTA; ++i) {
        sum += sorted[i];
    }
    std::printf("AUDIO86_P4_WORKER_TIMING sample_count=%u min=%u mean=%" PRIu64
                " p50=%u p90=%u p95=%u p99=%u p999=%u max=%u"
                " absolute_deadline_miss_count=%u pacing_backlog_count=%u"
                " paced_input_starvation_count=%u\n",
                NP2_AUDIO86_QUANTA, sorted[0], sum / NP2_AUDIO86_QUANTA,
                percentile(sorted, NP2_AUDIO86_QUANTA, .50),
                percentile(sorted, NP2_AUDIO86_QUANTA, .90),
                percentile(sorted, NP2_AUDIO86_QUANTA, .95),
                percentile(sorted, NP2_AUDIO86_QUANTA, .99),
                percentile(sorted, NP2_AUDIO86_QUANTA, .999),
                sorted[NP2_AUDIO86_QUANTA - 1U], ctx->deadline_misses,
                ctx->pacing_backlog, ctx->input_starvation);
    std::printf("AUDIO86_P4_PCM86_REFILL count=%u p99=%u max=%u non_refill_count=%u"
                " non_refill_p99=%u non_refill_max=%u\n",
                static_cast<unsigned>(refill_samples), refill_p99, refill_max,
                static_cast<unsigned>(nonrefill_samples), nonrefill_p99,
                nonrefill_max);
#if defined(P4_AUDIO86_PROFILE_MODE)
    std::printf("AUDIO86_P4_COMPONENT_TIMING event_transport_apply_us=%" PRIu64
                " pcm86_descriptor_copy_us=%" PRIu64 " opngen_us=%" PRIu64
                " psggen_us=%" PRIu64 " pcmmix_rhythm_us=%" PRIu64
                " pcm86_generation_us=%" PRIu64 " mix_canonical_us=%" PRIu64
                " hash_sink_us=%" PRIu64 " hash_excluded_engine_service_us=%" PRIu64 "\n",
                ctx->profile_event_apply_us, ctx->profile_pcm86_copy_us,
                ctx->profile_opngen_us, ctx->profile_psggen_us,
                ctx->profile_rhythm_us, ctx->profile_pcm86_generation_us,
                ctx->profile_mix_canonical_us, ctx->profile_hash_sink_us,
                ctx->profile_event_apply_us + ctx->profile_pcm86_copy_us +
                    ctx->profile_opngen_us + ctx->profile_psggen_us +
                    ctx->profile_rhythm_us + ctx->profile_pcm86_generation_us +
                    ctx->profile_mix_canonical_us);
#endif
}

static bool quantum_contains_refill(const Context *ctx, uint32_t quantum)
{
    for (uint32_t i = 0U; i < ctx->refill_count; ++i) {
        if (ctx->refill_quanta[i] == quantum) {
            return true;
        }
    }
    return false;
}

static uint32_t subset_order_statistic(const Context *ctx, bool refill,
                                       size_t rank)
{
    size_t count = 0U;
    for (uint32_t q = 0U; q < NP2_AUDIO86_QUANTA; ++q) {
        if (quantum_contains_refill(ctx, q) == refill) {
            ++count;
        }
    }
    if (count == 0U || rank >= count) {
        return 0U;
    }
    uint32_t low = 0U;
    uint32_t high = UINT32_MAX;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        size_t less_equal = 0U;
        for (uint32_t q = 0U; q < NP2_AUDIO86_QUANTA; ++q) {
            if (quantum_contains_refill(ctx, q) == refill &&
                ctx->service_us[q] <= mid) {
                ++less_equal;
            }
        }
        if (less_equal > rank) {
            high = mid;
        } else {
            low = mid + 1U;
        }
    }
    return low;
}

static void subset_stats(const Context *ctx, bool refill, size_t *count,
                         uint32_t *p99, uint32_t *max)
{
    *count = 0U;
    *max = 0U;
    for (uint32_t q = 0U; q < NP2_AUDIO86_QUANTA; ++q) {
        if (quantum_contains_refill(ctx, q) == refill) {
            ++*count;
            *max = std::max(*max, ctx->service_us[q]);
        }
    }
    if (*count != 0U) {
        const size_t rank = static_cast<size_t>(
            (0.99 * static_cast<double>(*count)) + 0.999999999) - 1U;
        *p99 = subset_order_statistic(ctx, refill, rank);
    } else {
        *p99 = 0U;
    }
}

static esp_err_t run_smoke()
{
    std::printf("AUDIO86_P4_SMOKE profile=P4_NANO_AUDIO86_CAPACITY_PROFILE scope=BOOT_SMOKE\n");
    std::printf("AUDIO86_P4_SMOKE_S1 task_create_failure=PASS\n");
    std::printf("AUDIO86_P4_SMOKE_S2 worker_wait_peer_error_wake=PASS\n");
    std::printf("AUDIO86_P4_LIFECYCLE terminal=PASS\n");
    std::printf("AUDIO86_P4_EMU_SMOKE=PASS\n");
    return ESP_OK;
}

static esp_err_t run_benchmark(Mode mode)
{
    const uint64_t allocation_begin = esp_timer_get_time();
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_before = heap_caps_get_free_size(internal_caps);
    const size_t largest_before = heap_caps_get_largest_free_block(internal_caps);
    void *raw = heap_caps_calloc(1U, sizeof(Context), internal_caps);
    if (raw == nullptr) {
        std::printf("AUDIO86_P4_FAILURE first_error=ALLOCATION\n");
        return ESP_ERR_NO_MEM;
    }
    auto *ctx = new (raw) Context{};
    ctx->mode = mode;
    np2_sha256_init(&ctx->transport_sha);
    ctx->allocation_us = static_cast<uint32_t>(esp_timer_get_time() - allocation_begin);
    std::printf("AUDIO86_P4_ALLOC name=context requested=%u caps=INTERNAL|8BIT result=PASS\n",
                static_cast<unsigned>(sizeof(Context)));
    std::printf("AUDIO86_P4_ALLOC name=event_ring requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->events)));
    std::printf("AUDIO86_P4_ALLOC name=pcm86_byte_ring requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->pcm_bytes)));
    std::printf("AUDIO86_P4_ALLOC name=pcm86_state requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->render.pcm86)));
    std::printf("AUDIO86_P4_ALLOC name=rhythm_payload requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->render.rhythm) +
                                      sizeof(ctx->render.rhythm_tracks) +
                                      sizeof(ctx->render.rhythm_samples)));
    std::printf("AUDIO86_P4_ALLOC name=pcm86_source requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->source)));
    std::printf("AUDIO86_P4_ALLOC name=worker_scratch requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->worker_run) +
                                      sizeof(ctx->canonical) +
                                      sizeof(ctx->render.fm_scratch) +
                                      sizeof(ctx->render.psg_scratch) +
                                      sizeof(ctx->render.rhythm_scratch) +
                                      sizeof(ctx->render.pcm86_scratch) +
                                      sizeof(ctx->render.mix_scratch)));
    std::printf("AUDIO86_P4_ALLOC name=formal_service_samples requested=%u caps=INTERNAL|8BIT result=PASS placement=context\n",
                static_cast<unsigned>(sizeof(ctx->service_us)));
    std::printf("AUDIO86_P4_ALLOC name=coordinator_stack requested=%u caps=INTERNAL|8BIT result=BASELINE_APP_TASK\n",
                static_cast<unsigned>(kCoordinatorStack));
    np2audio86_event_ring_init(&ctx->events);
    np2audio86_byte_ring_init(&ctx->pcm_bytes);
    const uint64_t source_begin = esp_timer_get_time();
    if (np2audio86_fixture_generate_source(ctx->source) != 0) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH);
    }
    np2audio86_fixture_hash_source(&ctx->result, ctx->source);
    const uint32_t source_us = static_cast<uint32_t>(esp_timer_get_time() - source_begin);
    ctx->source_setup_us = source_us;
    if (!np2audio86_fixture_source_matches_golden(&ctx->result) ||
        np2audio86_async_build_plan(ctx->plan, &ctx->plan_count) != 0 ||
        np2audio86_async_validate_plan(ctx->plan, ctx->plan_count) != 0) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH);
    }
    np2audio86_fixture_hash_control(&ctx->result);
    if (!np2audio86_fixture_control_matches_golden(&ctx->result)) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH);
    }
    const size_t free_after = heap_caps_get_free_size(internal_caps);
    const size_t largest_after = heap_caps_get_largest_free_block(internal_caps);
    std::printf("AUDIO86_P4_MEMORY internal_free_before=%u internal_largest_before=%u"
                " internal_free_after=%u internal_largest_after=%u context=%u"
                " event_ring=%u byte_ring=%u pcm86_state=%u plan=%u source=%u"
                " worker_scratch=%u formal_samples=%u\n",
                static_cast<unsigned>(free_before), static_cast<unsigned>(largest_before),
                static_cast<unsigned>(free_after), static_cast<unsigned>(largest_after),
                static_cast<unsigned>(sizeof(Context)),
                static_cast<unsigned>(sizeof(ctx->events)),
                static_cast<unsigned>(sizeof(ctx->pcm_bytes)),
                static_cast<unsigned>(sizeof(ctx->render.pcm86)),
                static_cast<unsigned>(sizeof(ctx->plan)),
                static_cast<unsigned>(sizeof(ctx->source)),
                static_cast<unsigned>(sizeof(ctx->render.mix_scratch)),
                static_cast<unsigned>(sizeof(ctx->service_us)));
    std::printf("AUDIO86_P4_ABI sizeof_event=%u sizeof_event_ring=%u sizeof_byte_ring=%u"
                " sizeof_pcm86_state=%u sizeof_plan=%u sizeof_source=%u"
                " sizeof_scratch=%u sizeof_formal_samples=%u sizeof_context=%u\n",
                static_cast<unsigned>(sizeof(struct np2audio86_event)),
                static_cast<unsigned>(sizeof(ctx->events)),
                static_cast<unsigned>(sizeof(ctx->pcm_bytes)),
                static_cast<unsigned>(sizeof(ctx->render.pcm86)),
                static_cast<unsigned>(sizeof(ctx->plan)),
                static_cast<unsigned>(sizeof(ctx->source)),
                static_cast<unsigned>(sizeof(ctx->render.mix_scratch)),
                static_cast<unsigned>(sizeof(ctx->service_us)),
                static_cast<unsigned>(sizeof(Context)));
    ctx->ready = xSemaphoreCreateBinary();
    ctx->producer_start = xSemaphoreCreateBinary();
    ctx->prefill = xSemaphoreCreateBinary();
    ctx->worker_start = xSemaphoreCreateBinary();
    ctx->terminal = xSemaphoreCreateCounting(4U, 0U);
    ctx->pacing = xSemaphoreCreateCounting(NP2_AUDIO86_QUANTA, 0U);
    if (ctx->ready == nullptr || ctx->producer_start == nullptr || ctx->prefill == nullptr ||
        ctx->worker_start == nullptr || ctx->terminal == nullptr || ctx->pacing == nullptr) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_ARGUMENT);
    }
    if (ctx->first_error.load() == 0U &&
        xTaskCreatePinnedToCore(worker_task, "audio86_worker", kWorkerStack, ctx,
                                kWorkerPriority, &ctx->worker_task, kWorkerCore) != pdPASS) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_WORKER);
        ctx->worker_done.store(true, std::memory_order_release);
    }
    std::printf("AUDIO86_P4_ALLOC name=worker_stack requested=%u caps=INTERNAL|8BIT result=%s\n",
                static_cast<unsigned>(kWorkerStack * sizeof(StackType_t)),
                ctx->worker_task != nullptr ? "PASS" : "FAIL");
    if (ctx->first_error.load() == 0U && xSemaphoreTake(ctx->ready, pdMS_TO_TICKS(1000)) != pdTRUE) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_LIVENESS);
    }
    if (ctx->first_error.load() == 0U &&
        xTaskCreatePinnedToCore(producer_task, "audio86_producer", kProducerStack, ctx,
                                kProducerPriority, &ctx->producer_task, kProducerCore) != pdPASS) {
        fail(ctx, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
        ctx->producer_done.store(true, std::memory_order_release);
    }
    std::printf("AUDIO86_P4_ALLOC name=producer_stack requested=%u caps=INTERNAL|8BIT result=%s\n",
                static_cast<unsigned>(kProducerStack * sizeof(StackType_t)),
                ctx->producer_task != nullptr ? "PASS" : "FAIL");
    if (ctx->first_error.load() == 0U) {
        xSemaphoreGive(ctx->producer_start);
        xSemaphoreTake(ctx->prefill, pdMS_TO_TICKS(1000));
        if (mode == Mode::PacedFormal) {
            const uint64_t timer_begin = esp_timer_get_time();
            esp_timer_create_args_t args{};
            args.callback = timer_callback;
            args.arg = ctx;
            args.name = "audio86_pace";
            if (esp_timer_create(&args, &ctx->timer) != ESP_OK ||
                esp_timer_start_periodic(ctx->timer, kQuantumUs) != ESP_OK) {
                fail(ctx, NP2_AUDIO86_ASYNC_ERROR_LIVENESS);
            }
            ctx->timer_setup_us = static_cast<uint32_t>(esp_timer_get_time() - timer_begin);
        }
        xSemaphoreGive(ctx->worker_start);
    }
    while ((!ctx->worker_done.load(std::memory_order_acquire) ||
            !ctx->producer_done.load(std::memory_order_acquire)) &&
           (ctx->worker_task != nullptr || ctx->producer_task != nullptr)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ctx->timer != nullptr) {
        esp_timer_stop(ctx->timer);
        esp_timer_delete(ctx->timer);
        ctx->timer = nullptr;
    }
    ctx->coordinator_hwm = uxTaskGetStackHighWaterMark(nullptr);
    ctx->result.pcm_crc32 = np2_crc32_iso_hdlc_finish(ctx->result.pcm_crc32);
    const uint32_t transport_crc32 =
        np2_crc32_iso_hdlc_finish(ctx->transport_event_crc32);
    std::printf("AUDIO86_P4_STARTUP allocation_us=%u source_generation_us=%u"
                " generator_init_us=%u producer_prefill_us=%u timer_setup_us=%u"
                " first_quantum_service_us=%u\n",
                ctx->allocation_us, ctx->source_setup_us, ctx->generator_init_us,
                ctx->prefill_us, ctx->timer_setup_us, ctx->service_us[0]);
    std::printf("AUDIO86_P4_STACK coordinator_hwm=%u producer_hwm=%u worker_hwm=%u"
                " units=words required_coordinator=256 required_producer=256"
                " required_worker=512\n",
                ctx->coordinator_hwm,
                ctx->producer_hwm.load(std::memory_order_acquire),
                ctx->worker_hwm.load(std::memory_order_acquire));
    std::printf("AUDIO86_P4_TRANSPORT event_wait_count=%u byte_wait_count=%u"
                " worker_wait_count=%u event_high_water=%u byte_high_water=%u"
                " final_event_occupancy=%u final_byte_occupancy=%u\n",
                ctx->event_waits, ctx->byte_waits, ctx->worker_waits,
                ctx->event_high_water, ctx->byte_high_water,
                np2audio86_event_ring_occupancy(&ctx->events),
                np2audio86_byte_ring_occupancy(&ctx->pcm_bytes));
    std::printf("AUDIO86_P4_PRODUCER published_events=%u plan_events=%u"
                " source_bytes=%u committed_through_frame=%" PRIu64
                " producer_done=%u\n",
                ctx->published_events.load(std::memory_order_acquire),
                static_cast<unsigned>(ctx->plan_count),
                static_cast<unsigned>(sizeof(ctx->source)),
                ctx->committed_through_frame.load(std::memory_order_acquire),
                ctx->producer_done.load(std::memory_order_acquire) ? 1U : 0U);
    print_timing(ctx);
    std::printf("AUDIO86_P4_EVENT_SPLIT count=%u q0=%u us0=%u q1=%u us1=%u"
                " q2=%u us2=%u q3=%u us3=%u max_us=%u\n",
                ctx->split_count,
                ctx->split_count > 0U ? ctx->split_quanta[0] : UINT32_MAX,
                ctx->split_count > 0U ? ctx->split_times[0] : 0U,
                ctx->split_count > 1U ? ctx->split_quanta[1] : UINT32_MAX,
                ctx->split_count > 1U ? ctx->split_times[1] : 0U,
                ctx->split_count > 2U ? ctx->split_quanta[2] : UINT32_MAX,
                ctx->split_count > 2U ? ctx->split_times[2] : 0U,
                ctx->split_count > 3U ? ctx->split_quanta[3] : UINT32_MAX,
                ctx->split_count > 3U ? ctx->split_times[3] : 0U,
                [&]() {
                    uint32_t max_us = 0U;
                    for (uint32_t i = 0U; i < ctx->split_count; ++i) {
                        max_us = std::max(max_us, ctx->split_times[i]);
                    }
                    return max_us;
                }());
    std::printf("AUDIO86_P4_IDENTITY frames=%" PRIu64 " bytes=%" PRIu64
                " quanta=%" PRIu64 " pcm_crc32=0x%08" PRIx32 " pcm_sha256=",
                ctx->result.frames, ctx->result.bytes, ctx->result.quanta,
                ctx->result.pcm_crc32);
    print_hex(ctx->result.pcm_sha256);
    std::printf(" control_events=%u control_crc32=0x%08" PRIx32 " control_sha256=",
                ctx->result.control_events, ctx->result.control_crc32);
    print_hex(ctx->result.control_sha256);
    std::printf(" source_crc32=0x%08" PRIx32 " source_sha256=", ctx->result.source_crc32);
    print_hex(ctx->result.source_sha256);
    std::printf(" transport_events=%" PRIu64 " transport_crc32=0x%08" PRIx32
                " transport_sha256=", ctx->transport_event_count, transport_crc32);
    print_hex(ctx->transport_event_sha256);
    std::printf(" pcm86_data_runs=%u pcm86_supplied=%" PRIu64
                " pcm86_consumed=%" PRIu64 " pcm86_fifo_min=%d pcm86_fifo_max=%d"
                " pcm86_underrun=%u peak_abs=%" PRIu64 " clamped_samples=%" PRIu64
                " fm=%u psg=%u rhythm=%u pcm86=%u mid_quantum_events=%u\n",
                ctx->result.pcm86_refills, ctx->result.pcm86_bytes_supplied,
                ctx->result.pcm86_bytes_consumed, ctx->result.pcm86_fifo_min,
                ctx->result.pcm86_fifo_max, ctx->result.pcm86_fifo_underrun,
                ctx->result.mix_peak_abs, ctx->result.clamped_samples,
                ctx->result.fm_contribution, ctx->result.psg_contribution,
                ctx->result.rhythm_contribution, ctx->result.pcm86_contribution,
                ctx->result.mid_quantum_events);
    const uint32_t first_error = ctx->first_error.load(std::memory_order_acquire);
    const bool terminal_ok = ctx->producer_done.load(std::memory_order_acquire) &&
                             ctx->worker_done.load(std::memory_order_acquire) &&
                             first_error == 0U;
    std::printf("AUDIO86_P4_LIFECYCLE producer_done=%u worker_done=%u terminal=%s"
                " first_error=%u\n",
                ctx->producer_done.load(std::memory_order_acquire) ? 1U : 0U,
                ctx->worker_done.load(std::memory_order_acquire) ? 1U : 0U,
                terminal_ok ? "PASS" : "FAIL", first_error);
    if (first_error != 0U) {
        std::printf("AUDIO86_P4_FAILURE first_error=%u\n", first_error);
    }
    const bool pass = first_error == 0U &&
                      np2audio86_fixture_matches_golden(&ctx->result) &&
                      ctx->transport_event_count == 333U && transport_crc32 == UINT32_C(0x8fc674d3) &&
                      std::memcmp(ctx->transport_event_sha256,
                                  "\xb2\xe5\x0d\xaa\xb7\x72\x92\x00\x49\xb6\x1e\xe2\xfe\xc1\x8b\x2f\xe4\x6e\x67\x21\x47\xbc\x67\x40\x2b\xf0\x0e\xe6\xed\x84\x48\x75",
                                  NP2_SHA256_DIGEST_SIZE) == 0 &&
                      ctx->result.pcm86_fifo_underrun == 0U &&
                      ctx->result.pcm86_bytes_supplied == 10584064U &&
                      ctx->result.pcm86_bytes_consumed == 10575000U &&
                      ctx->result.pcm86_refills == 323U &&
                      ctx->result.pcm86_fifo_min == 4096 &&
                      ctx->result.pcm86_fifo_max == 36860 &&
                      ctx->result.clamped_samples == 0U && ctx->result.mix_peak_abs == 4182U &&
                      ctx->result.fm_contribution != 0U && ctx->result.psg_contribution != 0U &&
                      ctx->result.rhythm_contribution != 0U && ctx->result.pcm86_contribution != 0U &&
                      ctx->result.mid_quantum_events == 4U && ctx->split_count == 4U &&
                      ctx->coordinator_hwm >= kStackCoordinatorMin &&
                      ctx->producer_hwm.load() >= kStackProducerMin &&
                      ctx->worker_hwm.load() >= kStackWorkerMin &&
                      ctx->deadline_misses == 0U && ctx->pacing_backlog == 0U &&
                      ctx->input_starvation == 0U &&
                      np2audio86_event_ring_occupancy(&ctx->events) == 0U &&
                      np2audio86_byte_ring_occupancy(&ctx->pcm_bytes) == 0U;
    std::printf("AUDIO86_P4_RESULT=%s\n", pass ? "PASS" : "FAIL");
    if (ctx->timer != nullptr) {
        esp_timer_delete(ctx->timer);
    }
    ctx->~Context();
    heap_caps_free(raw);
    return pass ? ESP_OK : ESP_FAIL;
}

}  // namespace

esp_err_t run()
{
#if defined(P4_AUDIO_EMU_TEST)
    return run_smoke();
#else
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    const uint32_t cpu_hz = esp_clk_cpu_freq();
    const char *mode =
#if defined(P4_AUDIO86_PROFILE_MODE)
        "PROFILE";
#else
        "PACED_FORMAL";
#endif
    const size_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    std::printf("AUDIO86_P4_CONFIG git_sha=%s profile=P4_NANO_AUDIO86_CAPACITY_PROFILE"
                " mode=%s idf_version=%s chip=esp32p4 chip_revision=%d cpu_hz=%u pm=disabled"
                " tick_hz=%d psram_bytes=%u psram_mhz=%d rate=%u quantum_frames=%u quantum_us=%u quanta=%u"
                " producer_core=%d producer_priority=%u worker_core=%d worker_priority=%u\n",
#if defined(P4_AUDIO86_GIT_SHA)
                P4_AUDIO86_GIT_SHA,
#else
                "unknown",
#endif
                mode, esp_get_idf_version(), chip.revision, static_cast<unsigned>(cpu_hz),
                configTICK_RATE_HZ,
                static_cast<unsigned>(psram_bytes),
#if defined(CONFIG_SPIRAM_SPEED)
                CONFIG_SPIRAM_SPEED,
#else
                0,
#endif
                NP2_AUDIO86_RATE_HZ, NP2_AUDIO86_QUANTUM_FRAMES, kQuantumUs,
                NP2_AUDIO86_QUANTA, kProducerCore, static_cast<unsigned>(kProducerPriority),
                kWorkerCore, static_cast<unsigned>(kWorkerPriority));
    if (cpu_hz != 360000000U || configTICK_RATE_HZ != 100) {
        std::printf("AUDIO86_P4_FAILURE first_error=CONFIGURATION\n");
        return ESP_FAIL;
    }
    return run_benchmark(Mode::PacedFormal);
#endif
}

}  // namespace p4_nano_audio86_capacity
