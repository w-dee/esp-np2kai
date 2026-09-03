/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink_idf.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "p4_nano_board/p4_nano_board.hpp"

namespace p4_nano_audio86_physical {
namespace {

constexpr uint32_t kRateHz = 48000U;
constexpr uint8_t kCodecAddress = 0x18U;
constexpr uint8_t kDacMuteRegister = 0x31U;
constexpr uint8_t kDacVolumeRegister = 0x32U;
constexpr uint8_t kMuteMask = 0x60U;
constexpr uint8_t kDacVolume = 0xa0U;
constexpr TickType_t kPaSettleTicks = pdMS_TO_TICKS(150U);

struct IdfBackend {
    p4_nano_board::I2cDeviceLease codec{};
    i2s_chan_handle_t tx = nullptr;
    TaskHandle_t *waiter_slot = nullptr;
    struct p4_nano_audio86_callback_gate *callback_gate = nullptr;
    bool codec_acquired = false;
    bool pa_initialized = false;
    bool channel_created = false;
    bool channel_enabled = false;
    bool callbacks_registered = false;
    uint32_t enable_stream_duration_us = 0U;
    uint32_t codec_unmute_duration_us = 0U;
};

uint32_t elapsed_us(const int64_t started, const int64_t completed)
{
    if (started < 0 || completed < started) return 0U;
    const uint64_t elapsed = static_cast<uint64_t>(completed - started);
    return elapsed > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed);
}

esp_err_t codec_write(const p4_nano_board::I2cDeviceLease &lease,
                      uint8_t reg, uint8_t value)
{
    const uint8_t payload[] = {reg, value};
    return p4_nano_board::shared_i2c_device_transmit(
        &lease, payload, sizeof(payload), 100);
}

esp_err_t codec_read(const p4_nano_board::I2cDeviceLease &lease,
                     uint8_t reg, uint8_t *value)
{
    return p4_nano_board::shared_i2c_device_transmit_receive(
        &lease, &reg, sizeof(reg), value, sizeof(*value), 100);
}

esp_err_t codec_mute(const p4_nano_board::I2cDeviceLease &lease, bool mute)
{
    uint8_t value = 0U;
    esp_err_t ret = codec_read(lease, kDacMuteRegister, &value);
    if (ret != ESP_OK) return ret;
    value = mute ? static_cast<uint8_t>(value | kMuteMask)
                 : static_cast<uint8_t>(value & ~kMuteMask);
    ret = codec_write(lease, kDacMuteRegister, value);
    if (ret != ESP_OK) return ret;
    value = 0U;
    ret = codec_read(lease, kDacMuteRegister, &value);
    if (ret != ESP_OK) return ret;
    return ((value & kMuteMask) == (mute ? kMuteMask : 0U))
               ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t codec_configure(const p4_nano_board::I2cDeviceLease &lease)
{
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
    ret = codec_write(lease, 0x06U,
                      static_cast<uint8_t>((value & 0xe0U) | 0x03U));
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
    ret = codec_mute(lease, true);
    if (ret != ESP_OK) return ret;
    value = 0U;
    ret = codec_read(lease, kDacVolumeRegister, &value);
    return ret == ESP_OK && value == kDacVolume
               ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t create_i2s(IdfBackend *backend)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS;
    channel.dma_frame_num = P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT;
    channel.auto_clear_after_cb = true;
    esp_err_t ret = i2s_new_channel(&channel, &backend->tx, nullptr);
    if (ret != ESP_OK) return ret;
    backend->channel_created = true;
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRateHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13,
            .bclk = GPIO_NUM_12,
            .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9,
            .din = GPIO_NUM_11,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return i2s_channel_init_std_mode(backend->tx, &standard);
}

bool IRAM_ATTR on_sent(i2s_chan_handle_t, i2s_event_data_t *, void *opaque)
{
    p4_nano_audio86_callback_gate_on_sent(
        static_cast<p4_nano_audio86_callback_gate *>(opaque));
    return false;
}

bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *,
                             void *opaque)
{
    p4_nano_audio86_callback_gate_on_send_q_ovf(
        static_cast<p4_nano_audio86_callback_gate *>(opaque));
    return false;
}

void rollback_prepare(IdfBackend *backend)
{
    if (backend->codec_acquired) (void)codec_mute(backend->codec, true);
    if (backend->pa_initialized) (void)p4_nano_board::pa_service_disable();
    if (backend->channel_enabled && backend->tx != nullptr) {
        if (i2s_channel_disable(backend->tx) == ESP_OK)
            backend->channel_enabled = false;
    }
    if (backend->channel_created && !backend->channel_enabled &&
        backend->tx != nullptr) {
        if (i2s_del_channel(backend->tx) == ESP_OK) {
            backend->tx = nullptr;
            backend->channel_created = false;
            backend->callbacks_registered = false;
            backend->callback_gate = nullptr;
        }
    }
    if (backend->codec_acquired) {
        (void)p4_nano_board::shared_i2c_release_device(&backend->codec);
        backend->codec_acquired = false;
        (void)p4_nano_board::shared_i2c_shutdown();
    }
    if (backend->pa_initialized) {
        (void)p4_nano_board::pa_service_shutdown();
        backend->pa_initialized = false;
    }
}

int prepare(void *opaque, struct p4_nano_audio86_callback_gate *callback_gate,
            uint32_t generation)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    const auto fail_prepare = [backend]() {
        rollback_prepare(backend);
        return -1;
    };
    if (p4_nano_board::pa_service_init() != ESP_OK) return fail_prepare();
    backend->pa_initialized = true;
    if (p4_nano_board::pa_service_disable() != ESP_OK) return fail_prepare();
    if (p4_nano_board::shared_i2c_acquire_device(kCodecAddress,
                                                 &backend->codec) != ESP_OK)
        return fail_prepare();
    backend->codec_acquired = true;
    if (codec_configure(backend->codec) != ESP_OK ||
        create_i2s(backend) != ESP_OK) return fail_prepare();

    /* Muted zero warm-up is outside semantic PCM.  No user callback is
     * registered for this generation, and disable resets the driver's free
     * buffer queue before semantic READY-state preload begins. */
    if (i2s_channel_enable(backend->tx) != ESP_OK) return fail_prepare();
    backend->channel_enabled = true;
    if (p4_nano_board::pa_service_enable() != ESP_OK) return fail_prepare();
    vTaskDelay(kPaSettleTicks);
    if (i2s_channel_disable(backend->tx) != ESP_OK) return fail_prepare();
    backend->channel_enabled = false;

    (void)generation;
    backend->callback_gate = callback_gate;
    const i2s_event_callbacks_t callbacks = {
        .on_recv = nullptr,
        .on_recv_q_ovf = nullptr,
        .on_sent = on_sent,
        .on_send_q_ovf = on_send_q_ovf,
    };
    if (i2s_channel_register_event_callback(backend->tx, &callbacks,
                                            backend->callback_gate) != ESP_OK)
        return fail_prepare();
    backend->callbacks_registered = true;
    return 0;
}

enum p4_nano_audio86_physical_io_result preload(
    void *opaque, const uint8_t *pcm, size_t bytes, size_t *bytes_loaded)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    const esp_err_t ret = i2s_channel_preload_data(
        backend->tx, pcm, bytes, bytes_loaded);
    return ret == ESP_OK ? P4_NANO_AUDIO86_PHYSICAL_IO_OK
                         : P4_NANO_AUDIO86_PHYSICAL_IO_ERROR;
}

int enable_stream(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    const int64_t enable_started_us = esp_timer_get_time();
    const esp_err_t enable_result = i2s_channel_enable(backend->tx);
    backend->enable_stream_duration_us = elapsed_us(
        enable_started_us, esp_timer_get_time());
    if (enable_result != ESP_OK) return -1;
    backend->channel_enabled = true;
    p4_nano_audio86_callback_gate_set_service_phase(
        backend->callback_gate, P4_NANO_AUDIO86_CONSUMER_PHASE_CODEC_UNMUTE);
    const int64_t unmute_started_us = esp_timer_get_time();
    const esp_err_t unmute_result = codec_mute(backend->codec, false);
    backend->codec_unmute_duration_us = elapsed_us(
        unmute_started_us, esp_timer_get_time());
    if (unmute_result != ESP_OK) {
        (void)i2s_channel_disable(backend->tx);
        backend->channel_enabled = false;
        return -1;
    }
    return 0;
}

void get_startup_durations(void *opaque, uint32_t *enable_stream_us,
                           uint32_t *codec_unmute_us)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (enable_stream_us != nullptr)
        *enable_stream_us = backend->enable_stream_duration_us;
    if (codec_unmute_us != nullptr)
        *codec_unmute_us = backend->codec_unmute_duration_us;
}

enum p4_nano_audio86_physical_io_result write(
    void *opaque, const uint8_t *pcm, size_t bytes, size_t *bytes_written,
    uint32_t timeout_ms)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    const esp_err_t ret = i2s_channel_write(
        backend->tx, pcm, bytes, bytes_written, timeout_ms);
    if (ret == ESP_OK) return P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    if (ret == ESP_ERR_TIMEOUT)
        return P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT;
    return P4_NANO_AUDIO86_PHYSICAL_IO_ERROR;
}

int mute(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (!backend->codec_acquired) return 0;
    return codec_mute(backend->codec, true) == ESP_OK ? 0 : -1;
}

int pa_low(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (!backend->pa_initialized) return 0;
    return p4_nano_board::pa_service_disable() == ESP_OK ? 0 : -1;
}

int disable(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (!backend->channel_enabled) return 0;
    if (i2s_channel_disable(backend->tx) != ESP_OK) return -1;
    backend->channel_enabled = false;
    return 0;
}

int unregister_callbacks(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    /* i2s_channel_disable()/gdma_stop() alone is not an ISR-start barrier.
     * i2s_del_channel() reaches gdma_del_channel()/esp_intr_free() on the
     * interrupt-owning core; only after it returns may the embedded user_data
     * gate be reclaimed once its portable in_flight count is also zero. */
    if (backend->channel_created) {
        if (backend->channel_enabled ||
            i2s_del_channel(backend->tx) != ESP_OK) return -1;
        backend->tx = nullptr;
        backend->channel_created = false;
    }
    backend->callbacks_registered = false;
    backend->callback_gate = nullptr;
    return 0;
}

uint64_t now_ms(void *)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us < 0 ? UINT64_MAX : static_cast<uint64_t>(now_us) / 1000U;
}

void wait_hint(void *, uint32_t timeout_ms)
{
    (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, pdMS_TO_TICKS(timeout_ms));
}

void notify_waiter(void *opaque, bool from_isr)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (backend->waiter_slot == nullptr || *backend->waiter_slot == nullptr)
        return;
    if (from_isr) {
        BaseType_t high_priority_woken = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(*backend->waiter_slot, 0U,
                                      &high_priority_woken);
        if (high_priority_woken == pdTRUE) portYIELD_FROM_ISR();
    } else {
        (void)xTaskNotifyGiveIndexed(*backend->waiter_slot, 0U);
    }
}

void release(void *opaque)
{
    auto *backend = static_cast<IdfBackend *>(opaque);
    if (backend->channel_enabled) {
        (void)i2s_channel_disable(backend->tx);
        backend->channel_enabled = false;
    }
    if (backend->channel_created) {
        (void)i2s_del_channel(backend->tx);
        backend->tx = nullptr;
        backend->channel_created = false;
    }
    if (backend->codec_acquired) {
        (void)p4_nano_board::shared_i2c_release_device(&backend->codec);
        backend->codec_acquired = false;
        const esp_err_t shutdown = p4_nano_board::shared_i2c_shutdown();
        (void)shutdown;
    }
    if (backend->pa_initialized) {
        (void)p4_nano_board::pa_service_shutdown();
        backend->pa_initialized = false;
    }
    backend->~IdfBackend();
    heap_caps_free(backend);
}

const struct p4_nano_audio86_physical_backend kOperations = {
    prepare, preload, enable_stream, get_startup_durations, write, mute, pa_low, disable,
    unregister_callbacks, now_ms, wait_hint, notify_waiter, release, nullptr};

} // namespace

esp_err_t create_idf(struct p4_nano_audio86_physical_sink **sink,
                     TaskHandle_t *waiter_slot) noexcept
{
    if (sink == nullptr || waiter_slot == nullptr) return ESP_ERR_INVALID_ARG;
    void *memory = heap_caps_calloc(1U, sizeof(IdfBackend),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memory == nullptr) return ESP_ERR_NO_MEM;
    auto *backend = new (memory) IdfBackend{};
    backend->waiter_slot = waiter_slot;
    struct p4_nano_audio86_physical_backend operations = kOperations;
    operations.opaque = backend;
    if (p4_nano_audio86_physical_sink_create(sink, &operations) != 0) {
        backend->~IdfBackend();
        heap_caps_free(backend);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

} // namespace p4_nano_audio86_physical
