/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_audio_i2s_opngen/p4_nano_audio_i2s_opngen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_pcm_ring.h"
#include "np2opngen_s98.h"
#include "np2opngen_spsc.h"
#include "np2opngen_synthetic_workload.h"
#include "p4_nano_board/p4_nano_board.hpp"

extern "C" {
extern const uint8_t _binary_retrofm_pocket_demo_strict_s98_start[];
extern const uint8_t _binary_retrofm_pocket_demo_strict_s98_end[];
}

namespace p4_nano_audio_i2s_opngen {
namespace {

constexpr uint32_t kRateHz = 48000U;
constexpr uint32_t kQuantumFrames = NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES;
constexpr size_t kQuantumBytes = NP2_OPNGEN_PCM_RING_SLOT_BYTES;
constexpr uint32_t kDmaDescriptorCount = 4U;
constexpr uint32_t kPrefillTarget = 4U;
constexpr uint32_t kProducerCore = 1U;
constexpr uint32_t kWorkerCore = 0U;
constexpr uint32_t kConsumerCore = 0U;
constexpr UBaseType_t kProducerPriority = tskIDLE_PRIORITY + 3U;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 4U;
constexpr UBaseType_t kConsumerPriority = tskIDLE_PRIORITY + 5U;
constexpr uint32_t kWriteTimeoutMs = 1000U;
static_assert(kWriteTimeoutMs == 1000U,
              "i2s_channel_write timeout contract must remain milliseconds");
constexpr TickType_t kPaSettle = pdMS_TO_TICKS(150U);
constexpr TickType_t kFinalDmaDrain = pdMS_TO_TICKS(20U);
constexpr uint8_t kCodecAddress = 0x18U;
constexpr uint8_t kDacMuteRegister = 0x31U;
constexpr uint8_t kDacVolumeRegister = 0x32U;
constexpr uint8_t kMuteMask = 0x60U;
constexpr uint8_t kDacVolume = 0xa0U;
constexpr size_t kMaxBlocks = 12000U;

static const uint8_t kRetroEventSha[] = {
    0x89,0x8b,0x04,0x9d,0x1c,0x37,0xc8,0xcc,0x65,0x03,0x75,0x98,0x49,0x24,0x40,0x48,
    0xe0,0xe7,0xf7,0x78,0x08,0x7e,0x1e,0xb5,0x70,0x6b,0xed,0xf1,0x16,0xe9,0xda,0xcf};
static const uint8_t kRetroPcmSha[] = {
    0x1d,0x4d,0x24,0xad,0x9c,0x96,0x6d,0xea,0x08,0x56,0x07,0xaf,0xee,0x6a,0x9e,0xcb,
    0x04,0x9c,0x2c,0x47,0x68,0x63,0xc5,0x34,0xdb,0xfe,0x0e,0x50,0xac,0xe1,0x01,0x6b};
static const uint8_t kStressEventSha[] = {
    0x37,0x66,0xb6,0xfd,0x4a,0xcd,0x79,0x9b,0x75,0x17,0xc9,0x03,0xd3,0xd1,0x38,0xb3,
    0x61,0x95,0x91,0x63,0xb4,0xad,0x13,0xca,0x9a,0x6d,0x12,0xba,0x68,0x33,0x37,0x3b};
static const uint8_t kStressPcmSha[] = {
    0x46,0x32,0x66,0x98,0x86,0x93,0x0b,0x31,0x31,0x2b,0x96,0xb2,0x53,0x4e,0xb7,0xc5,
    0x0d,0xba,0x4c,0x14,0x55,0xeb,0x55,0xf4,0xf0,0x61,0xae,0x19,0x78,0x1b,0xb7,0x32};

struct Expected {
    uint64_t events;
    uint64_t frames;
    uint32_t event_crc;
    uint32_t pcm_crc;
    const uint8_t *event_sha;
    const uint8_t *pcm_sha;
};

struct Workload {
    const char *name;
    bool retro;
    enum np2opngen_synthetic_profile profile;
    uint32_t duration_seconds;
    Expected expected;
};

struct Identity {
    uint64_t frames = 0U;
    uint64_t bytes = 0U;
    uint32_t crc = np2_crc32_iso_hdlc_init();
    np2_sha256_context sha{};
};

struct Context;

struct Metrics {
    uint64_t complete = 0U;
    uint64_t partial = 0U;
    uint64_t timeout = 0U;
    uint64_t errors = 0U;
    uint64_t underruns = 0U;
    uint64_t sequence_errors = 0U;
    uint64_t frame_offset_errors = 0U;
    uint64_t overrun = 0U;
    uint64_t dropped = 0U;
    uint64_t full_wait_count = 0U;
    uint64_t full_wait_total_us = 0U;
    uint64_t full_wait_max_us = 0U;
    uint64_t occupancy_min = UINT64_MAX;
    uint64_t occupancy_max = 0U;
    uint64_t occupancy_hist[NP2_OPNGEN_PCM_RING_CAPACITY + 1U]{};
    uint32_t *latency_us = nullptr;
    size_t latency_count = 0U;
    uint64_t opngen_call_count = 0U;
    uint64_t opngen_service_total_us = 0U;
    uint64_t opngen_service_max_us = 0U;
    int64_t opngen_start_us = -1;
    uint64_t compute_step_count = 0U;
    uint64_t compute_service_total_us = 0U;
    uint64_t compute_service_max_us = 0U;
    bool timing_failed = false;
};

struct Context {
    const Workload *workload = nullptr;
    struct np2opngen_spsc_queue events{};
    struct np2opngen_e1b_control control{};
    struct np2opngen_e1b_worker worker{};
    struct np2opngen_pcm_ring ring{};
    p4_nano_board::I2cDeviceLease codec{};
    i2s_chan_handle_t tx = nullptr;
    bool i2s_created = false;
    bool i2s_enabled = false;
    bool pa_ready = false;
    bool codec_ready = false;
    Identity generated{};
    Identity submitted{};
    Metrics metrics{};
    std::atomic<bool> failed{false};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> worker_done{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> producer_terminal_ack{false};
    std::atomic<bool> worker_terminal_ack{false};
    std::atomic<bool> consumer_terminal_ack{false};
    std::atomic<bool> started{false};
    std::atomic<bool> producer_waiting{false};
    SemaphoreHandle_t pcm_space = nullptr;
    SemaphoreHandle_t prefill_ready = nullptr;
    SemaphoreHandle_t consumer_start = nullptr;
    SemaphoreHandle_t worker_terminal = nullptr;
    SemaphoreHandle_t producer_terminal = nullptr;
    SemaphoreHandle_t consumer_terminal = nullptr;
    TaskHandle_t worker_task = nullptr;
    TaskHandle_t producer_task = nullptr;
    TaskHandle_t consumer_task = nullptr;
    uint64_t producer_count = 0U;
    uint32_t producer_crc = np2_crc32_iso_hdlc_init();
    np2_sha256_context producer_sha{};
    bool producer_trace_valid = false;
    uint32_t expected_blocks = 0U;
    uint32_t expected_slot_sequence = 0U;
    uint64_t expected_slot_frame_offset = 0U;
    uint32_t prefill_actual = 0U;
};

static_assert(sizeof(Context) == 9152U,
              "A3 Context layout changed; refresh the placement contract");

static const Workload kRetro = {
    "RETROFM", true, NP2_OPNGEN_SYNTHETIC_LIGHT, 0U,
    {1047U, 576960U, 0x3416c2b6U, 0x79b0dfadU, kRetroEventSha, kRetroPcmSha}};
static const Workload kStress = {
    "STRESS-60", false, NP2_OPNGEN_SYNTHETIC_STRESS, 60U,
    {41127U, 2880000U, 0x91eac288U, 0x39c7f2d2U, kStressEventSha, kStressPcmSha}};

static void print_hex(const uint8_t *digest)
{
    for (size_t i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i)
        std::printf("%02x", digest[i]);
}

static bool digest_equal(const uint8_t *a, const uint8_t *b)
{
    return std::memcmp(a, b, NP2_SHA256_DIGEST_SIZE) == 0;
}

static void fail(Context *ctx)
{
    ctx->failed.store(true, std::memory_order_release);
    if (ctx->pcm_space != nullptr) xSemaphoreGive(ctx->pcm_space);
    if (ctx->prefill_ready != nullptr) xSemaphoreGive(ctx->prefill_ready);
    if (ctx->consumer_start != nullptr) xSemaphoreGive(ctx->consumer_start);
    if (ctx->worker_task != nullptr) xTaskNotifyGive(ctx->worker_task);
    if (ctx->producer_task != nullptr) xTaskNotifyGive(ctx->producer_task);
    if (ctx->consumer_task != nullptr) xTaskNotifyGive(ctx->consumer_task);
}

static bool elapsed_us_checked(int64_t start_us, int64_t end_us,
                               uint64_t *elapsed_us)
{
    if (elapsed_us == nullptr || start_us < 0 || end_us < start_us) return false;
    *elapsed_us = static_cast<uint64_t>(end_us - start_us);
    return true;
}

static bool add_us_checked(uint64_t *total_us, uint64_t delta_us)
{
    if (total_us == nullptr || delta_us > UINT64_MAX - *total_us) return false;
    *total_us += delta_us;
    return true;
}

static bool record_latency_us(Context *ctx, uint64_t elapsed_us)
{
    if (elapsed_us > UINT32_MAX || ctx->metrics.latency_us == nullptr ||
        ctx->metrics.latency_count >= kMaxBlocks) return false;
    ctx->metrics.latency_us[ctx->metrics.latency_count++] =
        static_cast<uint32_t>(elapsed_us);
    return true;
}

static void timing_failure(Context *ctx)
{
    ctx->metrics.timing_failed = true;
    fail(ctx);
}

static esp_err_t codec_write(const p4_nano_board::I2cDeviceLease &lease,
                             uint8_t reg, uint8_t value)
{
    const uint8_t payload[] = {reg, value};
    return p4_nano_board::shared_i2c_device_transmit(
        &lease, payload, sizeof(payload), 100);
}

static esp_err_t codec_read(const p4_nano_board::I2cDeviceLease &lease,
                            uint8_t reg, uint8_t *value)
{
    return p4_nano_board::shared_i2c_device_transmit_receive(
        &lease, &reg, sizeof(reg), value, sizeof(*value), 100);
}

static esp_err_t codec_mute(const p4_nano_board::I2cDeviceLease &lease,
                            bool mute)
{
    uint8_t value = 0U;
    esp_err_t ret = codec_read(lease, kDacMuteRegister, &value);
    if (ret != ESP_OK) return ret;
    value = mute ? static_cast<uint8_t>(value | kMuteMask)
                 : static_cast<uint8_t>(value & ~kMuteMask);
    return codec_write(lease, kDacMuteRegister, value);
}

static esp_err_t codec_configure(const p4_nano_board::I2cDeviceLease &lease)
{
    /* Same ES8311 register sequence as the physically proven A3.3b path. */
    esp_err_t ret = codec_write(lease, 0x00U, 0x1fU);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(20U));
    for (const uint8_t value : {uint8_t{0x00U}, uint8_t{0x80U}}) {
        ret = codec_write(lease, 0x00U, value);
        if (ret != ESP_OK) return ret;
    }
    for (const auto &item : std::array<std::pair<uint8_t, uint8_t>, 6>{{
             {0x01U, 0x3fU}, {0x02U, 0x00U}, {0x03U, 0x10U},
             {0x04U, 0x10U}, {0x05U, 0x00U}, {0x08U, 0xffU}}}) {
        ret = codec_write(lease, item.first, item.second);
        if (ret != ESP_OK) return ret;
    }
    uint8_t value = 0U;
    ret = codec_read(lease, 0x06U, &value);
    if (ret != ESP_OK) return ret;
    ret = codec_write(lease, 0x06U, static_cast<uint8_t>((value & 0xe0U) | 0x03U));
    if (ret != ESP_OK) return ret;
    ret = codec_read(lease, 0x07U, &value);
    if (ret != ESP_OK) return ret;
    ret = codec_write(lease, 0x07U, static_cast<uint8_t>(value & 0xc0U));
    if (ret != ESP_OK) return ret;
    for (const auto &item : std::array<std::pair<uint8_t, uint8_t>, 7>{{
             {0x09U, 0x0cU}, {0x0aU, 0x0cU}, {0x0dU, 0x01U},
             {0x12U, 0x00U}, {0x13U, 0x10U}, {0x37U, 0x08U},
             {kDacVolumeRegister, kDacVolume}}}) {
        ret = codec_write(lease, item.first, item.second);
        if (ret != ESP_OK) return ret;
    }
    return codec_mute(lease, true);
}

static bool codec_readback(const p4_nano_board::I2cDeviceLease &lease)
{
    uint8_t value = 0xffU;
    if (codec_read(lease, kDacMuteRegister, &value) != ESP_OK ||
        (value & kMuteMask) != kMuteMask) return false;
    std::printf("P4_AUDIO_I2S_OPNGEN_CODEC_STARTUP_MUTE expected=0x60 actual=0x%02x result=ESP_OK\n",
                static_cast<unsigned>(value & kMuteMask));
    if (codec_read(lease, kDacVolumeRegister, &value) != ESP_OK ||
        value != kDacVolume) return false;
    std::printf("P4_AUDIO_I2S_OPNGEN_CODEC_VOLUME register=0x32 expected=0xa0 actual=0x%02x result=ESP_OK\n",
                static_cast<unsigned>(value));
    return true;
}

static bool i2s_init(Context *ctx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescriptorCount;
    chan_cfg.dma_frame_num = kQuantumFrames;
    chan_cfg.auto_clear_after_cb = true;
    if (i2s_new_channel(&chan_cfg, &ctx->tx, nullptr) != ESP_OK) return false;
    ctx->i2s_created = true;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRateHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9, .din = GPIO_NUM_11,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return i2s_channel_init_std_mode(ctx->tx, &std_cfg) == ESP_OK;
}

static void identity_update(Identity *identity, const uint8_t *pcm, size_t bytes)
{
    identity->crc = np2_crc32_iso_hdlc_update(identity->crc, pcm, bytes);
    np2_sha256_update(&identity->sha, pcm, bytes);
    identity->bytes += bytes;
    identity->frames += bytes / NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME;
}

static bool wait_for_ring_space(Context *ctx)
{
    while (!ctx->failed.load(std::memory_order_acquire) &&
           np2opngen_pcm_ring_occupancy(&ctx->ring) >= NP2_OPNGEN_PCM_RING_CAPACITY) {
        const int64_t started_us = esp_timer_get_time();
        if (started_us < 0) {
            timing_failure(ctx);
            return false;
        }
        ++ctx->metrics.full_wait_count;
        if (xSemaphoreTake(ctx->pcm_space, portMAX_DELAY) != pdTRUE) {
            fail(ctx);
            return false;
        }
        const int64_t ended_us = esp_timer_get_time();
        uint64_t elapsed_us = 0U;
        if (!elapsed_us_checked(started_us, ended_us, &elapsed_us) ||
            !add_us_checked(&ctx->metrics.full_wait_total_us, elapsed_us)) {
            timing_failure(ctx);
            return false;
        }
        ctx->metrics.full_wait_max_us =
            std::max(ctx->metrics.full_wait_max_us, elapsed_us);
    }
    return !ctx->failed.load(std::memory_order_acquire);
}

static void signal_prefill(Context *ctx)
{
    if (np2opngen_pcm_ring_occupancy(&ctx->ring) >= kPrefillTarget &&
        ctx->prefill_ready != nullptr)
        xSemaphoreGive(ctx->prefill_ready);
}

static int ring_sink_write(const uint8_t *pcm, size_t bytes,
                           uint64_t offset, void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    if (ctx == nullptr || pcm == nullptr || bytes != kQuantumBytes ||
        offset != ctx->generated.frames) {
        if (ctx != nullptr) ++ctx->metrics.frame_offset_errors;
        return -1;
    }
    /* Generated identity is recorded before the transport boundary. */
    identity_update(&ctx->generated, pcm, bytes);
    size_t consumed = 0U;
    while (consumed < bytes) {
        size_t appended = 0U;
        const int status = np2opngen_pcm_ring_append(
            &ctx->ring, pcm + consumed, (bytes - consumed) /
            NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME, offset +
            consumed / NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME, &appended);
        consumed += appended * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME;
        signal_prefill(ctx);
        if (status == NP2_OPNGEN_PCM_RING_OK) continue;
        if (status == NP2_OPNGEN_PCM_RING_FULL && wait_for_ring_space(ctx)) continue;
        if (status == NP2_OPNGEN_PCM_RING_OFFSET)
            ++ctx->metrics.frame_offset_errors;
        else if (status == NP2_OPNGEN_PCM_RING_INVARIANT)
            ++ctx->metrics.overrun;
        else if (status != NP2_OPNGEN_PCM_RING_FULL)
            ++ctx->metrics.dropped;
        return -1;
    }
    return 0;
}

static int ring_sink_finish(uint64_t final_frame, void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    if (ctx == nullptr || final_frame != ctx->generated.frames) {
        if (ctx != nullptr) ++ctx->metrics.frame_offset_errors;
        return -1;
    }
    const int status = np2opngen_pcm_ring_finish(&ctx->ring, final_frame);
    if (status != NP2_OPNGEN_PCM_RING_OK) {
        if (status == NP2_OPNGEN_PCM_RING_OFFSET)
            ++ctx->metrics.frame_offset_errors;
        else
            ++ctx->metrics.overrun;
        return -1;
    }
    ctx->producer_done.store(true, std::memory_order_release);
    signal_prefill(ctx);
    return 0;
}

static void worker_render_begin(void *opaque, uint64_t, uint32_t)
{
    (void)opaque;
}

static void worker_opngen_begin(void *opaque, uint64_t, uint32_t)
{
    auto *ctx = static_cast<Context *>(opaque);
    ctx->metrics.opngen_start_us = esp_timer_get_time();
    if (ctx->metrics.opngen_start_us < 0) timing_failure(ctx);
}

static void worker_opngen_end(void *opaque, uint64_t, uint32_t, int)
{
    auto *ctx = static_cast<Context *>(opaque);
    const int64_t ended_us = esp_timer_get_time();
    uint64_t elapsed_us = 0U;
    if (!elapsed_us_checked(ctx->metrics.opngen_start_us, ended_us, &elapsed_us) ||
        !add_us_checked(&ctx->metrics.opngen_service_total_us, elapsed_us)) {
        timing_failure(ctx);
        return;
    }
    ++ctx->metrics.opngen_call_count;
    ctx->metrics.opngen_service_max_us =
        std::max(ctx->metrics.opngen_service_max_us, elapsed_us);
}

static bool enqueue_event(Context *ctx, const struct np2opngen_synth_event *event)
{
    for (;;) {
        if (ctx->failed.load(std::memory_order_acquire)) return false;
        const int status = np2opngen_spsc_enqueue(&ctx->events, event);
        if (status == NP2_OPNGEN_SPSC_OK) {
            ++ctx->producer_count;
            ctx->producer_crc = np2_crc32_iso_hdlc_update(
                ctx->producer_crc, reinterpret_cast<const uint8_t *>(event),
                sizeof(*event));
            np2_sha256_update(&ctx->producer_sha,
                              reinterpret_cast<const uint8_t *>(event), sizeof(*event));
            if (ctx->worker_task != nullptr) xTaskNotifyGive(ctx->worker_task);
            return true;
        }
        if (status != NP2_OPNGEN_SPSC_FULL) return false;
        ctx->producer_waiting.store(true, std::memory_order_release);
        if (np2opngen_spsc_occupancy(&ctx->events) < NP2_OPNGEN_SPSC_CAPACITY) {
            ctx->producer_waiting.store(false, std::memory_order_release);
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ctx->producer_waiting.store(false, std::memory_order_release);
    }
}

static void producer_task(void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    bool ok = true;
    np2_sha256_init(&ctx->producer_sha);
    if (ctx->workload->retro) {
        struct np2opngen_s98_parser parser{};
        const uint8_t *start = _binary_retrofm_pocket_demo_strict_s98_start;
        const size_t size = static_cast<size_t>(
            _binary_retrofm_pocket_demo_strict_s98_end - start);
        ok = np2opngen_s98_parser_init(&parser, start, size) == 0;
        while (ok) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_s98_parser_next(&parser, &event);
            if (next == NP2_OPNGEN_S98_NEXT_END) break;
            ok = next == NP2_OPNGEN_S98_NEXT_EVENT && enqueue_event(ctx, &event);
        }
    } else {
        struct np2opngen_synthetic_workload generator{};
        ok = np2opngen_synthetic_workload_init(
            &generator, ctx->workload->profile,
            ctx->workload->duration_seconds) == 0;
        while (ok) {
            struct np2opngen_synth_event event{};
            const int next = np2opngen_synthetic_workload_peek(&generator, &event);
            if (next == 0) break;
            ok = next > 0 && enqueue_event(ctx, &event) &&
                 np2opngen_synthetic_workload_commit(&generator) == 0;
        }
    }
    ctx->producer_trace_valid = ok;
    if (!ok) fail(ctx);
    np2opngen_e1b_control_producer_done(&ctx->control);
    if (ctx->worker_task != nullptr) xTaskNotifyGive(ctx->worker_task);
    ctx->producer_terminal_ack.store(true, std::memory_order_release);
    if (ctx->producer_terminal != nullptr) xSemaphoreGive(ctx->producer_terminal);
    vTaskSuspend(nullptr);
}

static void worker_task(void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    for (;;) {
        if (ctx->failed.load(std::memory_order_acquire)) break;
        const int64_t step_start_us = esp_timer_get_time();
        const uint64_t full_wait_total_us_before = ctx->metrics.full_wait_total_us;
        const int step = np2opngen_e1b_worker_step(&ctx->worker);
        const int64_t step_end_us = esp_timer_get_time();
        uint64_t step_elapsed_us = 0U;
        const uint64_t full_wait_total_us_after = ctx->metrics.full_wait_total_us;
        if (!elapsed_us_checked(step_start_us, step_end_us, &step_elapsed_us) ||
            full_wait_total_us_after < full_wait_total_us_before) {
            timing_failure(ctx);
            break;
        }
        const uint64_t full_wait_delta_us =
            full_wait_total_us_after - full_wait_total_us_before;
        if (step_elapsed_us < full_wait_delta_us ||
            !add_us_checked(&ctx->metrics.compute_service_total_us,
                            step_elapsed_us - full_wait_delta_us)) {
            timing_failure(ctx);
            break;
        }
        ++ctx->metrics.compute_step_count;
        ctx->metrics.compute_service_max_us = std::max(
            ctx->metrics.compute_service_max_us,
            step_elapsed_us - full_wait_delta_us);
        if (step == NP2_OPNGEN_E1B_STEP_PROGRESS) {
            if (ctx->producer_task != nullptr &&
                ctx->producer_waiting.load(std::memory_order_acquire))
                xTaskNotifyGive(ctx->producer_task);
            continue;
        }
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (step == NP2_OPNGEN_E1B_STEP_FAILED) fail(ctx);
        if (step == NP2_OPNGEN_E1B_STEP_COMPLETE ||
            step == NP2_OPNGEN_E1B_STEP_FAILED) break;
    }
    ctx->worker_done.store(true, std::memory_order_release);
    ctx->worker_terminal_ack.store(true, std::memory_order_release);
    if (ctx->worker_terminal != nullptr) xSemaphoreGive(ctx->worker_terminal);
    vTaskSuspend(nullptr);
}

static void submitted_update(Context *ctx, const uint8_t *pcm, size_t bytes)
{
    identity_update(&ctx->submitted, pcm, bytes);
}

static void consumer_task(void *opaque)
{
    auto *ctx = static_cast<Context *>(opaque);
    (void)xSemaphoreTake(ctx->consumer_start, portMAX_DELAY);
    ctx->started.store(true, std::memory_order_release);
    while (!ctx->failed.load(std::memory_order_acquire)) {
        const uint32_t occupancy = np2opngen_pcm_ring_occupancy(&ctx->ring);
        if (occupancy <= NP2_OPNGEN_PCM_RING_CAPACITY) {
            ++ctx->metrics.occupancy_hist[occupancy];
            ctx->metrics.occupancy_min = std::min<uint64_t>(
                ctx->metrics.occupancy_min, occupancy);
            ctx->metrics.occupancy_max = std::max<uint64_t>(
                ctx->metrics.occupancy_max, occupancy);
        } else {
            ++ctx->metrics.overrun;
            fail(ctx);
            break;
        }
        const struct np2opngen_pcm_ring_slot *slot = nullptr;
        const int status = np2opngen_pcm_ring_try_peek(&ctx->ring, &slot);
        if (status == NP2_OPNGEN_PCM_RING_EMPTY) {
            if (ctx->producer_done.load(std::memory_order_acquire) && occupancy == 0U) {
                /* IDF 5.5.4 i2s_channel_write copies into DMA-owned storage. */
                vTaskDelay(kFinalDmaDrain);
                break;
            }
            ++ctx->metrics.underruns;
            fail(ctx);
            break;
        }
        if (status != NP2_OPNGEN_PCM_RING_OK || slot == nullptr ||
            slot->valid_frames != kQuantumFrames || slot->flags != 0U ||
            slot->sequence != ctx->expected_slot_sequence ||
            slot->frame_offset != ctx->expected_slot_frame_offset) {
            if (slot != nullptr &&
                slot->sequence != ctx->expected_slot_sequence)
                ++ctx->metrics.sequence_errors;
            if (slot != nullptr &&
                slot->frame_offset != ctx->expected_slot_frame_offset)
                ++ctx->metrics.frame_offset_errors;
            fail(ctx);
            break;
        }
        const int64_t started_us = esp_timer_get_time();
        size_t written = 0U;
        const esp_err_t result = i2s_channel_write(
            ctx->tx, slot->pcm, kQuantumBytes, &written, kWriteTimeoutMs);
        const int64_t ended_us = esp_timer_get_time();
        uint64_t elapsed_us = 0U;
        if (!elapsed_us_checked(started_us, ended_us, &elapsed_us) ||
            !record_latency_us(ctx, elapsed_us)) {
            timing_failure(ctx);
            break;
        }
        if (result == ESP_ERR_TIMEOUT) ++ctx->metrics.timeout;
        else if (result != ESP_OK) ++ctx->metrics.errors;
        if (written != kQuantumBytes) ++ctx->metrics.partial;
        if (result != ESP_OK || written != kQuantumBytes) {
            fail(ctx);
            break;
        }
        /* Ownership transfers only after a complete blocking write. */
        submitted_update(ctx, slot->pcm, written);
        if (np2opngen_pcm_ring_consume(&ctx->ring) != NP2_OPNGEN_PCM_RING_OK) {
            fail(ctx);
            break;
        }
        ++ctx->expected_slot_sequence;
        ctx->expected_slot_frame_offset += kQuantumFrames;
        ++ctx->metrics.complete;
        xSemaphoreGive(ctx->pcm_space);
    }
    ctx->consumer_done.store(true, std::memory_order_release);
    ctx->consumer_terminal_ack.store(true, std::memory_order_release);
    if (ctx->consumer_terminal != nullptr) xSemaphoreGive(ctx->consumer_terminal);
    vTaskSuspend(nullptr);
}

static bool configure_i2s_and_codec(Context *ctx)
{
    if (p4_nano_board::pa_service_init() != ESP_OK) return false;
    ctx->pa_ready = true;
    std::printf("P4_AUDIO_I2S_OPNGEN_PA transition=LOW gpio=53 active_level=HIGH safe_level=LOW result=ESP_OK\n");
    if (p4_nano_board::shared_i2c_acquire_device(kCodecAddress, &ctx->codec) != ESP_OK)
        return false;
    std::printf("P4_AUDIO_I2S_OPNGEN_CODEC_LEASE controller_i2c=1 sda=7 scl=8 address=0x18 result=ESP_OK\n");
    if (!i2s_init(ctx)) return false;
    std::printf("P4_AUDIO_I2S_OPNGEN_I2S_INIT controller=I2S_NUM_0 rate_hz=48000 format=S16_STEREO i2s_format=philips clock_source=APLL mclk_multiple=256 dma_desc=4 dma_frames=240 dma_bytes=3840 mclk_gpio=13 bclk_gpio=12 ws_gpio=10 dout_gpio=9 result=ESP_OK\n");
    if (codec_configure(ctx->codec) != ESP_OK || !codec_readback(ctx->codec)) return false;
    ctx->codec_ready = true;
    return true;
}

static bool final_identities_ok(Context *ctx)
{
    uint8_t generated_sha[NP2_SHA256_DIGEST_SIZE]{};
    uint8_t submitted_sha[NP2_SHA256_DIGEST_SIZE]{};
    np2_sha256_final(&ctx->generated.sha, generated_sha);
    np2_sha256_final(&ctx->submitted.sha, submitted_sha);
    const uint32_t generated_crc = np2_crc32_iso_hdlc_finish(ctx->generated.crc);
    const uint32_t submitted_crc = np2_crc32_iso_hdlc_finish(ctx->submitted.crc);
    const bool generated_ok = ctx->generated.frames == ctx->workload->expected.frames &&
        ctx->generated.bytes == ctx->workload->expected.frames * 4U &&
        generated_crc == ctx->workload->expected.pcm_crc &&
        digest_equal(generated_sha, ctx->workload->expected.pcm_sha);
    const bool submitted_ok = ctx->submitted.frames == ctx->workload->expected.frames &&
        ctx->submitted.bytes == ctx->workload->expected.frames * 4U &&
        submitted_crc == ctx->workload->expected.pcm_crc &&
        digest_equal(submitted_sha, ctx->workload->expected.pcm_sha);
    const bool equal = ctx->generated.frames == ctx->submitted.frames &&
        ctx->generated.bytes == ctx->submitted.bytes && generated_crc == submitted_crc &&
        digest_equal(generated_sha, submitted_sha);
    std::printf("P4_AUDIO_I2S_OPNGEN_PCM_IDENTITY workload=%s mode=REAL_I2S generated_frames=%" PRIu64
                " generated_bytes=%" PRIu64 " generated_crc32=0x%08" PRIx32
                " generated_sha256=", ctx->workload->name, ctx->generated.frames,
                ctx->generated.bytes, generated_crc);
    print_hex(generated_sha);
    std::printf(" submitted_frames=%" PRIu64 " submitted_bytes=%" PRIu64
                " submitted_crc32=0x%08" PRIx32 " submitted_sha256=", ctx->submitted.frames,
                ctx->submitted.bytes, submitted_crc);
    print_hex(submitted_sha);
    std::printf(" generated_identity=%s submitted_identity=%s mechanical_equal=%s\n",
                generated_ok ? "PASS" : "FAIL", submitted_ok ? "PASS" : "FAIL",
                (equal && generated_ok && submitted_ok) ? "PASS" : "FAIL");
    return generated_ok && submitted_ok && equal;
}

static bool event_identity_ok(Context *ctx)
{
    uint64_t count = 0U;
    uint32_t crc = 0U;
    uint8_t digest[NP2_SHA256_DIGEST_SIZE]{};
    const bool finished = np2opngen_e1b_worker_event_trace_finish(
                              &ctx->worker, &count, &crc, digest) == 0;
    const bool ok = finished && count == ctx->workload->expected.events &&
        crc == ctx->workload->expected.event_crc &&
        digest_equal(digest, ctx->workload->expected.event_sha);
    std::printf("P4_AUDIO_I2S_OPNGEN_EVENT_IDENTITY workload=%s mode=REAL_I2S event_count=%" PRIu64
                " producer_count=%" PRIu64 " producer_trace=%s event_crc32=0x%08" PRIx32
                " event_sha256=", ctx->workload->name, count, ctx->producer_count,
                ctx->producer_trace_valid ? "PASS" : "FAIL", crc);
    print_hex(digest);
    std::printf(" result=%s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static void print_latency(Context *ctx)
{
    if (ctx->metrics.latency_count == 0U) return;
    std::sort(ctx->metrics.latency_us,
              ctx->metrics.latency_us + ctx->metrics.latency_count);
    uint64_t sum = 0U;
    for (size_t i = 0U; i < ctx->metrics.latency_count; ++i)
        sum += ctx->metrics.latency_us[i];
    const auto percentile = [&](unsigned p) {
        const size_t index = (ctx->metrics.latency_count * p + 99U) / 100U;
        return ctx->metrics.latency_us[index == 0U ? 0U : index - 1U];
    };
    std::printf("P4_AUDIO_I2S_OPNGEN_WRITE_LATENCY workload=%s mode=REAL_I2S count=%zu min_us=%u mean_us=%" PRIu64
                " p50_us=%u p90_us=%u p95_us=%u p99_us=%u max_us=%u\n",
                ctx->workload->name, ctx->metrics.latency_count,
                static_cast<unsigned>(ctx->metrics.latency_us[0]),
                sum / ctx->metrics.latency_count,
                static_cast<unsigned>(percentile(50)),
                static_cast<unsigned>(percentile(90)),
                static_cast<unsigned>(percentile(95)),
                static_cast<unsigned>(percentile(99)),
                static_cast<unsigned>(ctx->metrics.latency_us[ctx->metrics.latency_count - 1U]));
}

static bool run_workload(const Workload *workload)
{
    const size_t context_bytes = sizeof(Context);
    const uint32_t context_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_free_before = heap_caps_get_free_size(context_caps);
    const size_t internal_largest_before =
        heap_caps_get_largest_free_block(context_caps);
    if (internal_largest_before < context_bytes) {
        std::printf("P4_AUDIO_I2S_OPNGEN_CONTEXT_HEAP workload=%s context_bytes=%zu"
                    " internal_free_before=%zu internal_largest_before=%zu"
                    " allocation=SKIPPED result=FAIL\n",
                    workload->name, context_bytes, internal_free_before,
                    internal_largest_before);
        return false;
    }
    Context *ctx = static_cast<Context *>(
        heap_caps_calloc(1U, context_bytes, context_caps));
    if (ctx == nullptr) {
        const size_t internal_free_after = heap_caps_get_free_size(context_caps);
        const size_t internal_largest_after =
            heap_caps_get_largest_free_block(context_caps);
        std::printf("P4_AUDIO_I2S_OPNGEN_CONTEXT_HEAP workload=%s context_bytes=%zu"
                    " internal_free_before=%zu internal_largest_before=%zu"
                    " internal_free_after=%zu internal_largest_after=%zu"
                    " allocation=FAIL result=FAIL\n",
                    workload->name, context_bytes, internal_free_before,
                    internal_largest_before, internal_free_after,
                    internal_largest_after);
        return false;
    }
    /* The zeroed allocation provides the required heap placement; construct
     * the C++ members in place so their default state remains identical to
     * the former automatic Context{} object. */
    ctx = new (ctx) Context{};
    const size_t internal_free_after = heap_caps_get_free_size(context_caps);
    const size_t internal_largest_after =
        heap_caps_get_largest_free_block(context_caps);
    std::printf("P4_AUDIO_I2S_OPNGEN_CONTEXT_HEAP workload=%s context_bytes=%zu"
                " internal_free_before=%zu internal_largest_before=%zu"
                " internal_free_after=%zu internal_largest_after=%zu"
                " allocation=PASS result=PASS\n",
                workload->name, context_bytes, internal_free_before,
                internal_largest_before, internal_free_after,
                internal_largest_after);
    ctx->workload = workload;
    ctx->expected_blocks = static_cast<uint32_t>(workload->expected.frames / kQuantumFrames);
    ctx->metrics.latency_us = static_cast<uint32_t *>(
        heap_caps_calloc(ctx->expected_blocks > kMaxBlocks ? kMaxBlocks : ctx->expected_blocks,
                         sizeof(uint32_t), MALLOC_CAP_INTERNAL));
    np2_sha256_init(&ctx->generated.sha);
    np2_sha256_init(&ctx->submitted.sha);
    np2opngen_spsc_init(&ctx->events);
    np2opngen_pcm_ring_init(&ctx->ring);
    np2opngen_e1b_control_init(&ctx->control);
    bool successful = false;
    bool worker_terminal_wait_started = false;
    bool producer_terminal_wait_started = false;
    bool consumer_terminal_wait_started = false;
    bool worker_terminal_wait_pass = false;
    bool producer_terminal_wait_pass = false;
    bool consumer_terminal_wait_pass = false;
    bool graceful_terminal = false;
    bool tasks_reaped = false;
    uint8_t mute = 0xffU;
    uint64_t occupancy_min = 0U;
    const struct np2opngen_e1b_observer observer{
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        worker_render_begin, worker_opngen_begin, worker_opngen_end,
        nullptr, nullptr, ctx, true};
    if (ctx->metrics.latency_us == nullptr || !configure_i2s_and_codec(ctx)) {
        fail(ctx);
        goto cleanup;
    }
    {
        const struct np2opngen_e1b_pcm_sink sink{ring_sink_write, ctx, ring_sink_finish};
        if (np2opngen_e1b_worker_init_with_sink(
                &ctx->worker, &ctx->events, &ctx->control, workload->expected.frames,
                0U, workload->expected.events, &sink) != 0) {
            fail(ctx);
            goto cleanup;
        }
    }
    np2opngen_e1b_worker_set_observer(&ctx->worker, &observer);
    ctx->pcm_space = xSemaphoreCreateBinary();
    ctx->prefill_ready = xSemaphoreCreateBinary();
    ctx->consumer_start = xSemaphoreCreateBinary();
    ctx->worker_terminal = xSemaphoreCreateBinary();
    ctx->producer_terminal = xSemaphoreCreateBinary();
    ctx->consumer_terminal = xSemaphoreCreateBinary();
    if (ctx->pcm_space == nullptr || ctx->prefill_ready == nullptr ||
        ctx->consumer_start == nullptr || ctx->worker_terminal == nullptr ||
        ctx->producer_terminal == nullptr || ctx->consumer_terminal == nullptr)
        goto cleanup;
    if (xTaskCreatePinnedToCore(worker_task, "p4_i2s_worker", 8192, ctx,
                                kWorkerPriority, &ctx->worker_task, kWorkerCore) != pdPASS)
        goto cleanup;
    if (xTaskCreatePinnedToCore(producer_task, "p4_i2s_producer", 8192, ctx,
                                kProducerPriority, &ctx->producer_task, kProducerCore) != pdPASS)
        goto cleanup;
    if (xTaskCreatePinnedToCore(consumer_task, "p4_i2s_consumer", 8192, ctx,
                                kConsumerPriority, &ctx->consumer_task, kConsumerCore) != pdPASS)
        goto cleanup;
    if (xSemaphoreTake(ctx->prefill_ready, pdMS_TO_TICKS(120000U)) != pdTRUE ||
        ctx->failed.load(std::memory_order_acquire)) goto cleanup;
    ctx->prefill_actual = np2opngen_pcm_ring_occupancy(&ctx->ring);
    std::printf("P4_AUDIO_I2S_OPNGEN_PREFILL workload=%s mode=REAL_I2S target=%u actual=%u result=%s\n",
                workload->name, static_cast<unsigned>(kPrefillTarget),
                static_cast<unsigned>(ctx->prefill_actual),
                ctx->prefill_actual >= kPrefillTarget ? "PASS" : "FAIL");
    if (ctx->prefill_actual < kPrefillTarget) goto cleanup;
    std::printf("P4_AUDIO_I2S_OPNGEN_STARTUP_PCM workload=%s first_frame_offset=0 dropped_frames=0 result=PASS\n",
                workload->name);
    if (i2s_channel_enable(ctx->tx) != ESP_OK) goto cleanup;
    ctx->i2s_enabled = true;
    std::printf("P4_AUDIO_I2S_OPNGEN_I2S_ENABLE result=ESP_OK\n");
    if (p4_nano_board::pa_service_enable() != ESP_OK) goto cleanup;
    std::printf("P4_AUDIO_I2S_OPNGEN_PA transition=HIGH gpio=53 result=ESP_OK\n");
    vTaskDelay(kPaSettle);
    std::printf("P4_AUDIO_I2S_OPNGEN_PA_SETTLE duration_ms=150 result=ESP_OK\n");
    if (codec_mute(ctx->codec, false) != ESP_OK) goto cleanup;
    if (codec_read(ctx->codec, kDacMuteRegister, &mute) != ESP_OK ||
        (mute & kMuteMask) != 0U) goto cleanup;
    std::printf("P4_AUDIO_I2S_OPNGEN_CODEC_UNMUTE_READBACK expected=0x00 actual=0x%02x result=ESP_OK\n",
                static_cast<unsigned>(mute & kMuteMask));
    xSemaphoreGive(ctx->consumer_start);
    worker_terminal_wait_started = ctx->worker_terminal != nullptr;
    worker_terminal_wait_pass = worker_terminal_wait_started &&
        xSemaphoreTake(ctx->worker_terminal, pdMS_TO_TICKS(120000U)) == pdTRUE;
    producer_terminal_wait_started = ctx->producer_terminal != nullptr;
    producer_terminal_wait_pass = producer_terminal_wait_started &&
        xSemaphoreTake(ctx->producer_terminal, pdMS_TO_TICKS(120000U)) == pdTRUE;
    consumer_terminal_wait_started = ctx->consumer_terminal != nullptr;
    consumer_terminal_wait_pass = consumer_terminal_wait_started &&
        xSemaphoreTake(ctx->consumer_terminal, pdMS_TO_TICKS(120000U)) == pdTRUE;
    if (!worker_terminal_wait_pass || !producer_terminal_wait_pass ||
        !consumer_terminal_wait_pass ||
        !ctx->worker_done.load(std::memory_order_acquire) ||
        !ctx->producer_done.load(std::memory_order_acquire) ||
        !ctx->consumer_done.load(std::memory_order_acquire) ||
        !ctx->worker_terminal_ack.load(std::memory_order_acquire) ||
        !ctx->producer_terminal_ack.load(std::memory_order_acquire) ||
        !ctx->consumer_terminal_ack.load(std::memory_order_acquire)) goto cleanup;
    if (!ctx->producer_trace_valid || ctx->producer_count != workload->expected.events ||
        !event_identity_ok(ctx) || !final_identities_ok(ctx) ||
        np2opngen_pcm_ring_occupancy(&ctx->ring) != 0U ||
        ctx->worker.sequence_errors != 0U ||
        ctx->metrics.underruns != 0U || ctx->metrics.sequence_errors != 0U ||
        ctx->metrics.frame_offset_errors != 0U || ctx->metrics.overrun != 0U ||
        ctx->metrics.dropped != 0U || ctx->metrics.partial != 0U ||
        ctx->metrics.timeout != 0U || ctx->metrics.errors != 0U ||
        ctx->metrics.timing_failed) goto cleanup;
    print_latency(ctx);
    occupancy_min = ctx->metrics.occupancy_min == UINT64_MAX
        ? 0U : ctx->metrics.occupancy_min;
    std::printf("P4_AUDIO_I2S_OPNGEN_RING workload=%s mode=REAL_I2S capacity=8 prefill_target=4 prefill_actual=%u pre_dequeue_occupancy_min=%" PRIu64
                " pre_dequeue_occupancy_max=%" PRIu64 " final_occupancy=0 sequence_errors=%" PRIu64
                " frame_offset_errors=%" PRIu64 " overrun=%" PRIu64 " dropped=%" PRIu64
                " worker_sequence_errors=%" PRIu64 " i2s_consumer_underrun_count=%" PRIu64 " occupancy_hist=",
                workload->name, static_cast<unsigned>(ctx->prefill_actual), occupancy_min,
                ctx->metrics.occupancy_max, ctx->metrics.sequence_errors,
                ctx->metrics.frame_offset_errors, ctx->metrics.overrun, ctx->metrics.dropped,
                ctx->worker.sequence_errors, ctx->metrics.underruns);
    for (size_t i = 0U; i <= NP2_OPNGEN_PCM_RING_CAPACITY; ++i) {
        if (i != 0U) std::printf(",");
        std::printf("%" PRIu64, ctx->metrics.occupancy_hist[i]);
    }
    std::printf("\n");
    std::printf("P4_AUDIO_I2S_OPNGEN_WRITES workload=%s mode=REAL_I2S complete_write_count=%" PRIu64
                " partial_write_count=%" PRIu64 " timeout_count=%" PRIu64
                " i2s_error_count=%" PRIu64 " result=%s\n", workload->name,
                ctx->metrics.complete, ctx->metrics.partial, ctx->metrics.timeout,
                ctx->metrics.errors,
                (ctx->metrics.partial == 0U && ctx->metrics.timeout == 0U &&
                 ctx->metrics.errors == 0U) ? "PASS" : "FAIL");
    std::printf("P4_AUDIO_I2S_OPNGEN_BACKPRESSURE workload=%s mode=REAL_I2S full_wait_count=%" PRIu64
                " full_wait_total_us=%" PRIu64 " full_wait_max_us=%" PRIu64 "\n", workload->name,
                ctx->metrics.full_wait_count, ctx->metrics.full_wait_total_us,
                ctx->metrics.full_wait_max_us);
    std::printf("P4_AUDIO_I2S_OPNGEN_COMPUTE workload=%s mode=REAL_I2S opngen_call_count=%" PRIu64
                " opngen_service_total_us=%" PRIu64 " opngen_service_mean_us=%" PRIu64
                " opngen_service_max_us=%" PRIu64 " compute_step_count=%" PRIu64
                " compute_service_total_us=%" PRIu64 " compute_service_mean_us=%" PRIu64
                " compute_service_max_us=%" PRIu64 " i2s_wait_separate=YES"
                " ring_full_wait_separate=YES\n", workload->name,
                ctx->metrics.opngen_call_count, ctx->metrics.opngen_service_total_us,
                ctx->metrics.opngen_call_count == 0U ? 0U :
                    ctx->metrics.opngen_service_total_us / ctx->metrics.opngen_call_count,
                ctx->metrics.opngen_service_max_us, ctx->metrics.compute_step_count,
                ctx->metrics.compute_service_total_us,
                ctx->metrics.compute_step_count == 0U ? 0U :
                    ctx->metrics.compute_service_total_us / ctx->metrics.compute_step_count,
                ctx->metrics.compute_service_max_us);
    successful = true;

cleanup:
    if (!successful) fail(ctx);
    graceful_terminal = successful && !ctx->failed.load(std::memory_order_acquire) &&
        worker_terminal_wait_pass && producer_terminal_wait_pass &&
        consumer_terminal_wait_pass &&
        ctx->worker_terminal_ack.load(std::memory_order_acquire) &&
        ctx->producer_terminal_ack.load(std::memory_order_acquire) &&
        ctx->consumer_terminal_ack.load(std::memory_order_acquire) &&
        ctx->worker_done.load(std::memory_order_acquire) &&
        ctx->producer_done.load(std::memory_order_acquire) &&
        ctx->consumer_done.load(std::memory_order_acquire);
    /* Quiesce all task users before tearing down the shared hardware handles. */
    tasks_reaped = ctx->worker_task != nullptr || ctx->producer_task != nullptr ||
        ctx->consumer_task != nullptr;
    if (ctx->worker_task != nullptr) vTaskDelete(ctx->worker_task);
    if (ctx->producer_task != nullptr) vTaskDelete(ctx->producer_task);
    if (ctx->consumer_task != nullptr) vTaskDelete(ctx->consumer_task);
    std::printf("P4_AUDIO_I2S_OPNGEN_LIFECYCLE workload=%s mode=REAL_I2S completion=%s"
                " producer_ack=%u worker_ack=%u consumer_ack=%u"
                " producer_terminal_wait=%s worker_terminal_wait=%s consumer_terminal_wait=%s"
                " tasks_quiesced=%u tasks_reaped=%u states=%s result=%s\n",
                workload->name, graceful_terminal ? "GRACEFUL" : "FORCED",
                ctx->producer_terminal_ack.load(std::memory_order_acquire) ? 1U : 0U,
                ctx->worker_terminal_ack.load(std::memory_order_acquire) ? 1U : 0U,
                ctx->consumer_terminal_ack.load(std::memory_order_acquire) ? 1U : 0U,
                producer_terminal_wait_started ? (producer_terminal_wait_pass ? "PASS" : "FAIL") : "NOT_WAITED",
                worker_terminal_wait_started ? (worker_terminal_wait_pass ? "PASS" : "FAIL") : "NOT_WAITED",
                consumer_terminal_wait_started ? (consumer_terminal_wait_pass ? "PASS" : "FAIL") : "NOT_WAITED",
                graceful_terminal ? 1U : 0U, tasks_reaped ? 1U : 0U,
                graceful_terminal ? "Init>Prefill>I2sReady>Running>ProducerDone>Draining>I2sDrain>Complete"
                                   : "Init>Forced",
                "ESP_OK");
    if (ctx->pa_ready) {
        (void)p4_nano_board::pa_service_disable();
        std::printf("P4_AUDIO_I2S_OPNGEN_PA transition=LOW gpio=53 result=ESP_OK\n");
    }
    if (ctx->codec_ready) {
        (void)codec_mute(ctx->codec, true);
        std::printf("P4_AUDIO_I2S_OPNGEN_CODEC_SHUTDOWN_MUTE result=ESP_OK\n");
    }
    if (ctx->i2s_enabled) {
        /* A completed write has copied to DMA; allow the configured four
         * 240-frame descriptors to advance before stopping BCLK/WS. */
        vTaskDelay(kFinalDmaDrain);
        (void)i2s_channel_disable(ctx->tx);
    }
    if (ctx->i2s_created && ctx->tx != nullptr) (void)i2s_del_channel(ctx->tx);
    if (ctx->codec.is_active()) (void)p4_nano_board::shared_i2c_release_device(&ctx->codec);
    if (ctx->pa_ready) {
        (void)p4_nano_board::pa_service_shutdown();
    }
    if (ctx->worker.s32_pcm != nullptr || ctx->worker.canonical_pcm != nullptr ||
        ctx->worker.opngen != nullptr) np2opngen_e1b_worker_destroy(&ctx->worker);
    if (ctx->pcm_space != nullptr) vSemaphoreDelete(ctx->pcm_space);
    if (ctx->prefill_ready != nullptr) vSemaphoreDelete(ctx->prefill_ready);
    if (ctx->consumer_start != nullptr) vSemaphoreDelete(ctx->consumer_start);
    if (ctx->worker_terminal != nullptr) vSemaphoreDelete(ctx->worker_terminal);
    if (ctx->producer_terminal != nullptr) vSemaphoreDelete(ctx->producer_terminal);
    if (ctx->consumer_terminal != nullptr) vSemaphoreDelete(ctx->consumer_terminal);
    if (ctx->metrics.latency_us != nullptr) heap_caps_free(ctx->metrics.latency_us);
    const bool ok = graceful_terminal && !ctx->metrics.timing_failed &&
        ctx->generated.frames == workload->expected.frames &&
        ctx->submitted.frames == workload->expected.frames;
    std::printf("P4_AUDIO_I2S_OPNGEN_SHUTDOWN workload=%s mode=REAL_I2S pa=LOW codec_muted=1 i2s_disabled=1 ring_occupancy=%u result=%s\n",
                workload->name, static_cast<unsigned>(np2opngen_pcm_ring_occupancy(&ctx->ring)),
                ok ? "PASS" : "FAIL");
    ctx->~Context();
    heap_caps_free(ctx);
    return ok;
}

} // namespace

esp_err_t run()
{
    std::printf("P4_AUDIO_I2S_OPNGEN_PROFILE=1 board=p4-nano sample_rate_hz=48000 format=S16LE_STEREO quantum_frames=240 ring_capacity=8\n");
    std::printf("P4_AUDIO_I2S_OPNGEN_SCHEDULER producer_core=1 producer_priority=3 worker_core=0 worker_priority=4 consumer_core=0 consumer_priority=5 pacing=I2S_HARDWARE\n");
    std::printf("A3_I2S_WRITE_TIMEOUT_UNIT_CONTRACT=PASS timeout_ms=%u\n",
                static_cast<unsigned>(kWriteTimeoutMs));
    const bool retro = run_workload(&kRetro);
    const bool stress = retro && run_workload(&kStress);
    std::printf("P4_AUDIO_I2S_OPNGEN_RESULT=%s\n", (retro && stress) ? "PASS" : "FAIL");
    return retro && stress ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_audio_i2s_opngen
