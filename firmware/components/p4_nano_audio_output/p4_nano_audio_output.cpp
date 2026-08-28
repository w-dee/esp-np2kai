/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_audio_output/p4_nano_audio_output.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "p4_nano_audio_output/tone_model.hpp"
#include "p4_nano_board/p4_nano_board.hpp"

namespace p4_nano_audio_output {
namespace {

constexpr std::uint8_t kCodecAddress = 0x18U;
constexpr int kI2cTimeoutMs = 100;
constexpr std::uint32_t kMclkMultiple = 256U;
constexpr std::uint32_t kMclkHz = kSampleRateHz * kMclkMultiple;
constexpr std::uint32_t kBclkHz = kSampleRateHz * 16U * 2U;
constexpr std::uint32_t kWriteTimeoutMs = 1000U;
constexpr TickType_t kClockSettleTicks = pdMS_TO_TICKS(2);

constexpr std::uint8_t kRegReset = 0x00U;
constexpr std::uint8_t kRegClock1 = 0x01U;
constexpr std::uint8_t kRegClock2 = 0x02U;
constexpr std::uint8_t kRegClock3 = 0x03U;
constexpr std::uint8_t kRegClock4 = 0x04U;
constexpr std::uint8_t kRegClock5 = 0x05U;
constexpr std::uint8_t kRegClock6 = 0x06U;
constexpr std::uint8_t kRegClock7 = 0x07U;
constexpr std::uint8_t kRegClock8 = 0x08U;
constexpr std::uint8_t kRegSdpIn = 0x09U;
constexpr std::uint8_t kRegSdpOut = 0x0aU;
constexpr std::uint8_t kRegSystem0d = 0x0dU;
constexpr std::uint8_t kRegSystem12 = 0x12U;
constexpr std::uint8_t kRegSystem13 = 0x13U;
constexpr std::uint8_t kRegDacMute = 0x31U;
constexpr std::uint8_t kRegDacVolume = 0x32U;
constexpr std::uint8_t kRegDacRamp = 0x37U;
constexpr std::uint8_t kMuteMask = 0x60U;
constexpr std::uint8_t kS16Resolution = 0x0cU;

constexpr std::uint32_t kExpectedCrc32 = UINT32_C(0x3054ef52);
constexpr std::array<std::uint8_t, NP2_SHA256_DIGEST_SIZE> kExpectedSha256 = {
    0xa0, 0xa4, 0xe3, 0x11, 0x82, 0xd3, 0x2b, 0xfd,
    0x51, 0x48, 0x5f, 0xfe, 0xf2, 0xee, 0x80, 0x30,
    0x6a, 0x57, 0x67, 0x0c, 0x56, 0x67, 0x87, 0x9d,
    0xfd, 0x35, 0x6d, 0x9d, 0x63, 0x6d, 0x91, 0xc5,
};

struct ToneIdentity {
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint32_t crc32 = np2_crc32_iso_hdlc_init();
    np2_sha256_context sha{};
    std::int16_t peak = 0;
    std::uint32_t clipping = 0;
    std::array<std::uint8_t, NP2_SHA256_DIGEST_SIZE> sha_digest{};
};

struct WriteMetrics {
    std::uint64_t submitted_frames = 0;
    std::uint64_t submitted_bytes = 0;
    std::uint32_t complete = 0;
    std::uint32_t partial = 0;
    std::uint32_t timeout = 0;
    std::uint32_t errors = 0;
    std::array<std::uint32_t, kLogicalBlockCount> latency_us{};
    std::size_t latency_count = 0;
};

void print_sha(const std::uint8_t *digest)
{
    for (std::size_t index = 0; index < NP2_SHA256_DIGEST_SIZE; ++index) {
        std::printf("%02x", digest[index]);
    }
}

void generate_block(std::size_t first_frame, std::uint8_t *block,
                    ToneIdentity *identity)
{
    fill_block(first_frame, block);
    for (std::size_t frame = 0; frame < kBlockFrames; ++frame) {
        const std::size_t offset = frame * kBytesPerFrame;
        const auto raw = static_cast<std::uint16_t>(block[offset]) |
                         (static_cast<std::uint16_t>(block[offset + 1U]) << 8U);
        const auto sample = static_cast<std::int16_t>(raw);
        const std::int32_t magnitude = sample < 0 ? -static_cast<std::int32_t>(sample)
                                                   : sample;
        identity->peak = std::max(identity->peak,
                                  static_cast<std::int16_t>(magnitude));
        if (sample == INT16_MAX || sample == INT16_MIN) {
            ++identity->clipping;
        }
    }
    identity->crc32 = np2_crc32_iso_hdlc_update(
        identity->crc32, block, kBlockBytes);
    np2_sha256_update(&identity->sha, block, kBlockBytes);
    identity->frames += kBlockFrames;
    identity->bytes += kBlockBytes;
}

esp_err_t codec_write(const p4_nano_board::I2cDeviceLease &lease,
                      std::uint8_t reg, std::uint8_t value)
{
    const std::uint8_t payload[] = {reg, value};
    return p4_nano_board::shared_i2c_device_transmit(
        &lease, payload, sizeof(payload), kI2cTimeoutMs);
}

esp_err_t codec_read(const p4_nano_board::I2cDeviceLease &lease,
                     std::uint8_t reg, std::uint8_t *value)
{
    return p4_nano_board::shared_i2c_device_transmit_receive(
        &lease, &reg, sizeof(reg), value, sizeof(*value), kI2cTimeoutMs);
}

esp_err_t codec_mute(const p4_nano_board::I2cDeviceLease &lease, bool mute)
{
    std::uint8_t value = 0;
    esp_err_t ret = codec_read(lease, kRegDacMute, &value);
    if (ret != ESP_OK) return ret;
    value = mute ? static_cast<std::uint8_t>(value | kMuteMask)
                 : static_cast<std::uint8_t>(value & ~kMuteMask);
    return codec_write(lease, kRegDacMute, value);
}

esp_err_t codec_configure(const p4_nano_board::I2cDeviceLease &lease)
{
    /* Register order/values follow Espressif's IDF v5.5.4 ES8311 example
     * and espressif/es8311 1.0.0 driver.  The ADC-only writes are omitted. */
    esp_err_t ret = codec_write(lease, kRegReset, 0x1fU);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(20));
    for (const auto value : {std::uint8_t{0x00U}, std::uint8_t{0x80U}}) {
        ret = codec_write(lease, kRegReset, value);
        if (ret != ESP_OK) return ret;
    }
    const std::array<std::pair<std::uint8_t, std::uint8_t>, 6> clocks = {{
        {kRegClock1, 0x3fU}, {kRegClock2, 0x00U}, {kRegClock3, 0x10U},
        {kRegClock4, 0x10U}, {kRegClock5, 0x00U}, {kRegClock8, 0xffU},
    }};
    for (const auto &[reg, value] : clocks) {
        ret = codec_write(lease, reg, value);
        if (ret != ESP_OK) return ret;
    }
    std::uint8_t value = 0;
    ret = codec_read(lease, kRegClock6, &value);
    if (ret != ESP_OK) return ret;
    ret = codec_write(lease, kRegClock6,
                      static_cast<std::uint8_t>((value & 0xe0U) | 0x03U));
    if (ret != ESP_OK) return ret;
    ret = codec_read(lease, kRegClock7, &value);
    if (ret != ESP_OK) return ret;
    ret = codec_write(lease, kRegClock7,
                      static_cast<std::uint8_t>(value & 0xc0U));
    if (ret != ESP_OK) return ret;
    ret = codec_read(lease, kRegReset, &value);
    if (ret != ESP_OK) return ret;
    ret = codec_write(lease, kRegReset, static_cast<std::uint8_t>(value & 0xbfU));
    if (ret != ESP_OK) return ret;
    for (const auto &[reg, setting] : std::array<std::pair<std::uint8_t, std::uint8_t>, 6>{{
             {kRegSdpIn, kS16Resolution}, {kRegSdpOut, kS16Resolution},
             {kRegSystem0d, 0x01U}, {kRegSystem12, 0x00U},
             {kRegSystem13, 0x10U}, {kRegDacRamp, 0x08U}}}) {
        ret = codec_write(lease, reg, setting);
        if (ret != ESP_OK) return ret;
    }
    ret = codec_write(lease, kRegDacVolume, 0x80U);
    if (ret != ESP_OK) return ret;
    return codec_mute(lease, true);
}

bool codec_readback_ok(const p4_nano_board::I2cDeviceLease &lease)
{
    struct Expect { std::uint8_t reg; std::uint8_t value; std::uint8_t mask; };
    constexpr std::array<Expect, 8> expected = {{
        {kRegClock1, 0x3fU, 0xffU}, {kRegClock3, 0x10U, 0xffU},
        {kRegClock4, 0x10U, 0xffU}, {kRegSdpIn, kS16Resolution, 0x1cU},
        {kRegSdpOut, kS16Resolution, 0x1cU}, {kRegDacMute, kMuteMask, kMuteMask},
        {kRegSystem12, 0x00U, 0x03U}, {kRegClock6, 0x03U, 0x1fU},
    }};
    for (const auto item : expected) {
        std::uint8_t value = 0;
        if (codec_read(lease, item.reg, &value) != ESP_OK ||
            (value & item.mask) != (item.value & item.mask)) {
            return false;
        }
    }
    return true;
}

esp_err_t configure_i2s(i2s_chan_handle_t *tx)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescriptorCount;
    chan_cfg.dma_frame_num = kBlockFrames;
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, tx, nullptr);
    if (ret != ESP_OK) return ret;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13,
            .bclk = GPIO_NUM_12,
            .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9,
            .din = GPIO_NUM_11,
            .invert_flags = {},
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ret = i2s_channel_init_std_mode(*tx, &std_cfg);
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

std::uint32_t percentile_us(const WriteMetrics &metrics, unsigned percentile)
{
    std::array<std::uint32_t, kLogicalBlockCount> sorted = metrics.latency_us;
    std::sort(sorted.begin(), sorted.begin() + metrics.latency_count);
    const std::size_t rank =
        ((metrics.latency_count * percentile) + 99U) / 100U;
    return sorted[rank == 0U ? 0U : rank - 1U];
}

} // namespace

esp_err_t run()
{
    std::printf("P4_AUDIO_I2S_TONE_PROFILE=1 board=p4-nano controller_i2c=1 sda=7 scl=8 codec=ES8311 codec_address=0x18\n");
    std::printf("P4_AUDIO_I2S_CONFIG controller=I2S_NUM_0 mclk_gpio=13 bclk_gpio=12 ws_gpio=10 dout_gpio=9 din_gpio=11 rate_hz=%" PRIu32 " format=S16_STEREO i2s_format=philips clock_source=APLL mclk_multiple=%" PRIu32 " mclk_hz=%" PRIu32 " bclk_hz=%" PRIu32 " dma_desc=%zu dma_frames=%zu dma_bytes=%zu\n",
                kSampleRateHz, kMclkMultiple, kMclkHz, kBclkHz,
                kDmaDescriptorCount, kBlockFrames, kDmaTotalBytes);

    p4_nano_board::I2cDeviceLease codec_lease;
    i2s_chan_handle_t tx = nullptr;
    bool i2s_created = false;
    bool i2s_enabled = false;
    bool pa_ready = false;
    bool write_active = false;
    esp_err_t result = p4_nano_board::pa_service_init();
    if (result == ESP_OK) pa_ready = true;
    std::printf("P4_AUDIO_PA transition=LOW result=%s\n", esp_err_to_name(result));
    if (result == ESP_OK) {
        result = p4_nano_board::shared_i2c_acquire_device(
            kCodecAddress, &codec_lease);
        std::printf("P4_AUDIO_CODEC_LEASE address=0x%02x result=%s\n",
                    kCodecAddress, esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = configure_i2s(&tx);
        i2s_created = tx != nullptr;
        std::printf("P4_AUDIO_I2S_INIT result=%s created=%d\n",
                    esp_err_to_name(result), i2s_created ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = codec_configure(codec_lease);
        std::printf("P4_AUDIO_CODEC_INIT result=%s muted_start=1\n",
                    esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        const bool readback_ok = codec_readback_ok(codec_lease);
        std::printf("P4_AUDIO_CODEC_READBACK result=%s\n",
                    readback_ok ? "ESP_OK" : "ESP_FAIL");
        if (!readback_ok) result = ESP_FAIL;
    }

    ToneIdentity identity;
    np2_sha256_init(&identity.sha);
    /* Keep the finite profile's sizeable buffers out of the 3584-byte
     * ESP_MAIN_TASK stack; these are internal DRAM objects, not PSRAM. */
    static std::array<std::uint8_t, kBlockBytes> block{};
    static std::array<std::uint8_t, kBlockBytes> silence{};
    if (result == ESP_OK) {
        for (std::size_t index = 0; index < kLogicalBlockCount; ++index) {
            generate_block(index * kBlockFrames, block.data(), &identity);
        }
        identity.crc32 = np2_crc32_iso_hdlc_finish(identity.crc32);
        np2_sha256_final(&identity.sha, identity.sha_digest.data());
        if (identity.frames != kExpectedFrames || identity.bytes != kExpectedBytes ||
            identity.crc32 != kExpectedCrc32 ||
            std::memcmp(identity.sha_digest.data(), kExpectedSha256.data(),
                        kExpectedSha256.size()) != 0 ||
            identity.peak != kTonePeak || identity.clipping != 0U) {
            result = ESP_FAIL;
        }
    }
    std::printf("P4_AUDIO_TONE_IDENTITY frames=%" PRIu64 " bytes=%" PRIu64 " crc32=0x%08" PRIx32 " sha256=",
                identity.frames, identity.bytes, identity.crc32);
    print_sha(identity.sha_digest.data());
    std::printf(" peak=%d clipping=%" PRIu32 " blocks=%zu\n",
                identity.peak, identity.clipping, kLogicalBlockCount);

    if (result == ESP_OK) {
        for (std::size_t index = 0; index < kDmaDescriptorCount; ++index) {
            std::size_t loaded = 0;
            result = i2s_channel_preload_data(tx, silence.data(),
                                              kBlockBytes, &loaded);
            if (result != ESP_OK || loaded != kBlockBytes) {
                result = result == ESP_OK ? ESP_FAIL : result;
                break;
            }
        }
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(tx);
        i2s_enabled = result == ESP_OK;
        std::printf("P4_AUDIO_I2S_ENABLE result=%s\n", esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        vTaskDelay(kClockSettleTicks);
        result = p4_nano_board::pa_service_enable();
        std::printf("P4_AUDIO_PA transition=HIGH result=%s\n",
                    esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = codec_mute(codec_lease, false);
        std::printf("P4_AUDIO_CODEC_MUTE state=unmuted result=%s\n",
                    esp_err_to_name(result));
    }

    static WriteMetrics metrics{};
    metrics = WriteMetrics{};
    if (result == ESP_OK) {
        for (std::size_t index = 0; index < kLogicalBlockCount; ++index) {
            fill_block(index * kBlockFrames, block.data());
            std::size_t written = 0;
            write_active = true;
            const std::int64_t started = esp_timer_get_time();
            const esp_err_t write_result = i2s_channel_write(
                tx, block.data(), kBlockBytes, &written, kWriteTimeoutMs);
            const std::int64_t elapsed = esp_timer_get_time() - started;
            write_active = false;
            metrics.latency_us[metrics.latency_count++] =
                static_cast<std::uint32_t>(elapsed < 0 ? 0 : elapsed);
            if (write_result == ESP_ERR_TIMEOUT) ++metrics.timeout;
            else if (write_result != ESP_OK) ++metrics.errors;
            if (written == kBlockBytes && write_result == ESP_OK) {
                ++metrics.complete;
                metrics.submitted_bytes += written;
                metrics.submitted_frames += kBlockFrames;
            } else {
                ++metrics.partial;
            }
            if (write_result != ESP_OK || written != kBlockBytes) {
                result = write_result == ESP_OK ? ESP_FAIL : write_result;
                break;
            }
        }
    }
    std::printf("P4_AUDIO_I2S_WRITES expected_frames=%zu submitted_frames=%" PRIu64 " expected_bytes=%zu submitted_bytes=%" PRIu64 " complete=%" PRIu32 " partial=%" PRIu32 " timeout=%" PRIu32 " errors=%" PRIu32,
                kExpectedFrames, metrics.submitted_frames, kExpectedBytes,
                metrics.submitted_bytes, metrics.complete, metrics.partial,
                metrics.timeout, metrics.errors);
    if (metrics.latency_count != 0U) {
        const auto minmax = std::minmax_element(metrics.latency_us.begin(),
                                                metrics.latency_us.begin() + metrics.latency_count);
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < metrics.latency_count; ++i) sum += metrics.latency_us[i];
        std::printf(" latency_min_us=%" PRIu32 " latency_mean_us=%" PRIu64 " latency_p50_us=%" PRIu32 " latency_p95_us=%" PRIu32 " latency_p99_us=%" PRIu32 " latency_max_us=%" PRIu32,
                    *minmax.first, sum / metrics.latency_count,
                    percentile_us(metrics, 50), percentile_us(metrics, 95),
                    percentile_us(metrics, 99), *minmax.second);
    }
    std::printf("\n");

    /* Shutdown is deliberately idempotent and fail-closed on every path. */
    esp_err_t cleanup_result = ESP_OK;
    const auto record_cleanup = [&cleanup_result](esp_err_t cleanup_ret) {
        if (cleanup_result == ESP_OK && cleanup_ret != ESP_OK)
            cleanup_result = cleanup_ret;
    };
    if (pa_ready) {
        const esp_err_t pa_ret = p4_nano_board::pa_service_disable();
        record_cleanup(pa_ret);
        std::printf("P4_AUDIO_PA transition=LOW result=%s\n",
                    esp_err_to_name(pa_ret));
    }
    if (codec_lease.is_active()) {
        const esp_err_t mute_ret = codec_mute(codec_lease, true);
        record_cleanup(mute_ret);
        std::printf("P4_AUDIO_CODEC_MUTE state=muted result=%s\n",
                    esp_err_to_name(mute_ret));
    }
    if (i2s_enabled) {
        vTaskDelay(pdMS_TO_TICKS(20));
        record_cleanup(i2s_channel_disable(tx));
        i2s_enabled = false;
    }
    if (write_active) result = ESP_FAIL;
    if (i2s_created) {
        record_cleanup(i2s_del_channel(tx));
        tx = nullptr;
    }
    if (codec_lease.is_active()) {
        record_cleanup(p4_nano_board::shared_i2c_release_device(&codec_lease));
    }
    if (pa_ready) record_cleanup(p4_nano_board::pa_service_shutdown());
    if (cleanup_result != ESP_OK) result = cleanup_result;
    std::printf("P4_AUDIO_I2S_TONE_SHUTDOWN pa=LOW write_active=%d codec_lease_released=%d shared_i2c_bus_deleted_by_audio=0\n",
                write_active ? 1 : 0, codec_lease.is_active() ? 0 : 1);
    return result == ESP_OK && metrics.submitted_frames == kExpectedFrames &&
                   metrics.submitted_bytes == kExpectedBytes &&
                   metrics.complete == kLogicalBlockCount && metrics.partial == 0 &&
                   metrics.timeout == 0 && metrics.errors == 0 && !write_active
               ? ESP_OK
               : ESP_FAIL;
}

} // namespace p4_nano_audio_output
