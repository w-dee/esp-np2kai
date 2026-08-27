/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_dma2d_copy/dma2d_copy.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/dma2d_types.h"
#include "soc/dma2d_channel.h"
#include "soc/soc_caps.h"

/* This is intentionally the only project source that includes the private
 * DMA2D driver API. */
#include "esp_private/dma2d.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "p4_nano_dma2d_copy requires the audited ESP-IDF v5.5.4 private DMA2D API"
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "p4_nano_dma2d_copy is restricted to ESP32-P4"
#endif
#if !SOC_DMA2D_SUPPORTED
#error "p4_nano_dma2d_copy requires SOC_DMA2D_SUPPORTED"
#endif
#if !SOC_PSRAM_DMA_CAPABLE
#error "p4_nano_dma2d_copy requires DMA-capable PSRAM"
#endif

static_assert(sizeof(dma2d_descriptor_t) == 24U);
static_assert(alignof(dma2d_descriptor_t) >= 8U);

namespace p4_nano_dma2d_copy {

enum class State : std::uint8_t {
    Idle,
    Queued,
    Active,
    Completed,
    Failed,
    RetainedAmbiguous,
};

static_assert(std::atomic<State>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

struct Adapter final {
    dma2d_pool_handle_t pool = nullptr;
    dma2d_trans_t *transaction = nullptr;
    dma2d_descriptor_t *tx_descriptor = nullptr;
    dma2d_descriptor_t *rx_descriptor = nullptr;
    StaticSemaphore_t semaphore_storage{};
    SemaphoreHandle_t complete = nullptr;
    dma2d_trans_config_t transaction_config{};
    std::atomic<State> state{State::Idle};
    std::atomic<std::uint32_t> completion_status{ESP_OK};
    std::atomic<bool> timing_active{false};
    std::atomic<bool> wait_active{false};
    std::atomic<std::uint32_t> on_job_wait_cycles{0U};
    std::atomic<std::uint32_t> on_job_outside_cycles{0U};
    std::atomic<std::uint32_t> eof_wait_cycles{0U};
    std::atomic<std::uint32_t> eof_outside_cycles{0U};
};

namespace {

constexpr std::size_t kDescriptorAlignment = 64U;
constexpr std::size_t kDescriptorStorageBytes = 64U;
constexpr TickType_t kCompletionTimeout = pdMS_TO_TICKS(5000U);
static_assert(kCompletionTimeout > 0);
static_assert(std::atomic<bool>::is_always_lock_free);

static_assert(sizeof(Adapter) > 0U);

enum class CallbackKind : std::uint8_t {
    OnJobPicked,
    Eof,
};

struct CallbackTimingStart final {
    std::uint32_t cycles = 0U;
    bool enabled = false;
    bool during_wait = false;
};

CallbackTimingStart IRAM_ATTR callback_timing_start(Adapter *adapter) noexcept
{
    if (adapter == nullptr ||
        !adapter->timing_active.load(std::memory_order_relaxed)) {
        return {};
    }
    return CallbackTimingStart{
        .cycles = esp_cpu_get_cycle_count(),
        .enabled = true,
        .during_wait = adapter->wait_active.load(std::memory_order_relaxed),
    };
}

void IRAM_ATTR callback_timing_finish(Adapter *adapter, CallbackKind kind,
                                      CallbackTimingStart start) noexcept
{
    if (adapter == nullptr || !start.enabled) {
        return;
    }
    const std::uint32_t elapsed = esp_cpu_get_cycle_count() - start.cycles;
    std::atomic<std::uint32_t> *target = nullptr;
    if (kind == CallbackKind::OnJobPicked) {
        target = start.during_wait ? &adapter->on_job_wait_cycles :
                                    &adapter->on_job_outside_cycles;
    } else {
        target = start.during_wait ? &adapter->eof_wait_cycles :
                                    &adapter->eof_outside_cycles;
    }
    /* Release-publish the callback result before the completion semaphore is
     * given.  The waiting task acquires these counters after taking it. */
    target->fetch_add(elapsed, std::memory_order_release);
}

void reset_timing(Adapter *adapter) noexcept
{
    adapter->wait_active.store(false, std::memory_order_relaxed);
    adapter->on_job_wait_cycles.store(0U, std::memory_order_relaxed);
    adapter->on_job_outside_cycles.store(0U, std::memory_order_relaxed);
    adapter->eof_wait_cycles.store(0U, std::memory_order_relaxed);
    adapter->eof_outside_cycles.store(0U, std::memory_order_relaxed);
}

void copy_timing(Adapter *adapter, CopyTiming *timing,
                 std::uint64_t total_wall_us,
                 std::uint64_t blocked_wait_us) noexcept
{
    if (timing == nullptr) {
        return;
    }
    *timing = CopyTiming{
        .total_wall_us = total_wall_us,
        .blocked_wait_us = blocked_wait_us,
        .on_job_cycles_during_wait = adapter->on_job_wait_cycles.load(
        std::memory_order_acquire),
        .on_job_cycles_outside_wait = adapter->on_job_outside_cycles.load(
        std::memory_order_acquire),
        .eof_cycles_during_wait = adapter->eof_wait_cycles.load(
        std::memory_order_acquire),
        .eof_cycles_outside_wait = adapter->eof_outside_cycles.load(
        std::memory_order_acquire),
    };
}

bool terminalize_ambiguous(Adapter *adapter) noexcept
{
    State observed = adapter->state.load(std::memory_order_acquire);
    for (;;) {
        if (observed == State::RetainedAmbiguous) {
            return false;
        }
        if (adapter->state.compare_exchange_weak(
                observed, State::RetainedAmbiguous,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

bool activate_from_queue(Adapter *adapter) noexcept
{
    State expected = State::Queued;
    return adapter->state.compare_exchange_strong(
        expected, State::Active, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool complete_from_active(Adapter *adapter) noexcept
{
    State expected = State::Active;
    return adapter->state.compare_exchange_strong(
        expected, State::Completed, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool return_to_idle(Adapter *adapter) noexcept
{
    State expected = State::Completed;
    return adapter->state.compare_exchange_strong(
        expected, State::Idle, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void IRAM_ATTR publish_completion_status(Adapter *adapter,
                                         esp_err_t status) noexcept
{
    adapter->completion_status.store(static_cast<std::uint32_t>(status),
                                     std::memory_order_release);
}

bool IRAM_ATTR give_completion(Adapter *adapter) noexcept
{
    if (portCHECK_IF_IN_ISR() != pdFALSE) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(adapter->complete,
                                    &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    }
    (void)xSemaphoreGive(adapter->complete);
    return false;
}

bool IRAM_ATTR signal_timed_completion(
    Adapter *adapter, esp_err_t status, CallbackKind kind,
    CallbackTimingStart timing) noexcept
{
    publish_completion_status(adapter, status);
    callback_timing_finish(adapter, kind, timing);
    return give_completion(adapter);
}

bool IRAM_ATTR dma2d_complete_callback(dma2d_channel_handle_t,
                                       dma2d_event_data_t *,
                                       void *user_data)
{
    const CallbackTimingStart timing = callback_timing_start(
        static_cast<Adapter *>(user_data));
    auto *adapter = static_cast<Adapter *>(user_data);
    if (adapter == nullptr || adapter->complete == nullptr) {
        return false;
    }
    (void)complete_from_active(adapter);
    return signal_timed_completion(adapter, ESP_OK, CallbackKind::Eof, timing);
}

bool IRAM_ATTR dma2d_job_picked_callback(
    std::uint32_t channel_count,
    const dma2d_trans_channel_info_t *channels,
    void *user_data)
{
    auto *adapter = static_cast<Adapter *>(user_data);
    const CallbackTimingStart timing = callback_timing_start(adapter);
    if (adapter == nullptr || channels == nullptr || channel_count != 2U) {
        if (adapter != nullptr) {
            (void)terminalize_ambiguous(adapter);
            return signal_timed_completion(adapter, ESP_ERR_INVALID_ARG,
                                           CallbackKind::OnJobPicked, timing);
        }
        return false;
    }
    if (!activate_from_queue(adapter)) {
        const State state = adapter->state.load(std::memory_order_acquire);
        if (state == State::RetainedAmbiguous) {
            return signal_timed_completion(adapter, ESP_ERR_TIMEOUT,
                                           CallbackKind::OnJobPicked, timing);
        }
        (void)terminalize_ambiguous(adapter);
        return signal_timed_completion(adapter, ESP_ERR_INVALID_STATE,
                                       CallbackKind::OnJobPicked, timing);
    }
    dma2d_channel_handle_t tx = nullptr;
    dma2d_channel_handle_t rx = nullptr;
    for (std::uint32_t index = 0U; index < channel_count; ++index) {
        if (channels[index].dir == DMA2D_CHANNEL_DIRECTION_TX) {
            tx = channels[index].chan;
        } else if (channels[index].dir == DMA2D_CHANNEL_DIRECTION_RX) {
            rx = channels[index].chan;
        }
    }
    if (tx == nullptr || rx == nullptr) {
        (void)terminalize_ambiguous(adapter);
        return signal_timed_completion(adapter, ESP_ERR_INVALID_ARG,
                                       CallbackKind::OnJobPicked, timing);
    }

    const dma2d_trigger_t tx_trigger{
        .periph = DMA2D_TRIG_PERIPH_M2M,
        .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX,
    };
    const dma2d_trigger_t rx_trigger{
        .periph = DMA2D_TRIG_PERIPH_M2M,
        .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX,
    };
    esp_err_t result = dma2d_connect(tx, &tx_trigger);
    if (result == ESP_OK) {
        result = dma2d_connect(rx, &rx_trigger);
    }
    if (result == ESP_OK) {
        dma2d_rx_event_callbacks_t callbacks{
            .on_recv_eof = dma2d_complete_callback,
            .on_desc_done = nullptr,
            .on_desc_empty = nullptr,
        };
        result = dma2d_register_rx_event_callbacks(rx, &callbacks, adapter);
    }
    if (result == ESP_OK) {
        result = dma2d_set_desc_addr(
            tx, reinterpret_cast<intptr_t>(adapter->tx_descriptor));
    }
    if (result == ESP_OK) {
        result = dma2d_set_desc_addr(
            rx, reinterpret_cast<intptr_t>(adapter->rx_descriptor));
    }
    if (result == ESP_OK) {
        result = dma2d_start(tx);
    }
    if (result == ESP_OK) {
        result = dma2d_start(rx);
    }
    if (result != ESP_OK) {
        (void)terminalize_ambiguous(adapter);
        return signal_timed_completion(adapter, result,
                                       CallbackKind::OnJobPicked, timing);
    }
    callback_timing_finish(adapter, CallbackKind::OnJobPicked, timing);
    return false;
}

void initialize_descriptor(dma2d_descriptor_t *descriptor, void *buffer,
                           std::size_t picture_width,
                           std::size_t picture_height,
                           std::size_t block_x) noexcept
{
    *descriptor = {};
    descriptor->vb_size = kChunkRows;
    descriptor->hb_length = kSourceWidthPixels;
    descriptor->dma2d_en = 1U;
    descriptor->suc_eof = 1U;
    descriptor->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
    descriptor->va_size = picture_height;
    descriptor->ha_length = picture_width;
    descriptor->pbyte = DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
    descriptor->x = block_x;
    descriptor->y = 0U;
    descriptor->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    descriptor->buffer = buffer;
    descriptor->next = nullptr;
}

void free_context(Adapter *adapter) noexcept
{
    if (adapter == nullptr) {
        return;
    }
    heap_caps_free(adapter->tx_descriptor);
    heap_caps_free(adapter->rx_descriptor);
    heap_caps_free(adapter->transaction);
    adapter->~Adapter();
    heap_caps_free(adapter);
}

} // namespace

esp_err_t create(Adapter **ret_adapter) noexcept
{
    if (ret_adapter == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_adapter = nullptr;
    void *memory = heap_caps_malloc(sizeof(Adapter),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto *adapter = static_cast<Adapter *>(memory);
    if (adapter == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    new (adapter) Adapter{};
    adapter->complete = xSemaphoreCreateBinaryStatic(
        &adapter->semaphore_storage);
    if (adapter->complete == nullptr) {
        free_context(adapter);
        return ESP_ERR_NO_MEM;
    }
    adapter->transaction = static_cast<dma2d_trans_t *>(heap_caps_calloc(
        1U, dma2d_get_trans_elm_size(),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    adapter->tx_descriptor = static_cast<dma2d_descriptor_t *>(
        heap_caps_aligned_calloc(kDescriptorAlignment, 1U,
                                 kDescriptorStorageBytes,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                                     MALLOC_CAP_8BIT));
    adapter->rx_descriptor = static_cast<dma2d_descriptor_t *>(
        heap_caps_aligned_calloc(kDescriptorAlignment, 1U,
                                 kDescriptorStorageBytes,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                                     MALLOC_CAP_8BIT));
    if (adapter->transaction == nullptr || adapter->tx_descriptor == nullptr ||
        adapter->rx_descriptor == nullptr) {
        free_context(adapter);
        return ESP_ERR_NO_MEM;
    }
    const dma2d_pool_config_t pool_config{.pool_id = 0U, .intr_priority = 0U};
    const esp_err_t pool_result =
        dma2d_acquire_pool(&pool_config, &adapter->pool);
    if (pool_result != ESP_OK) {
        free_context(adapter);
        return pool_result;
    }
    adapter->transaction_config = dma2d_trans_config_t{
        .tx_channel_num = 1U,
        .rx_channel_num = 1U,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING,
        .specified_tx_channel_mask = 0U,
        .specified_rx_channel_mask = 0U,
        .on_job_picked = dma2d_job_picked_callback,
        .user_config = adapter,
    };
    *ret_adapter = adapter;
    return ESP_OK;
}

esp_err_t copy_strided(Adapter *adapter, const std::uint8_t *source,
                       std::uint8_t *destination,
                       std::size_t dst_x_pixels, CopyTiming *timing) noexcept
{
    const std::uint64_t total_start = timing == nullptr ? 0U :
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (adapter == nullptr || source == nullptr || destination == nullptr ||
        (dst_x_pixels != kEvenXOffsetPixels &&
         dst_x_pixels != kOddXOffsetPixels) ||
        (reinterpret_cast<std::uintptr_t>(source) & 0xFU) != 0U ||
        (reinterpret_cast<std::uintptr_t>(destination) & 0x3U) != 0U ||
        adapter->state.load(std::memory_order_acquire) != State::Idle) {
        return ESP_ERR_INVALID_ARG;
    }
    while (xSemaphoreTake(adapter->complete, 0U) == pdTRUE) {
    }
    if (timing != nullptr) {
        reset_timing(adapter);
        adapter->timing_active.store(true, std::memory_order_release);
    }
    initialize_descriptor(adapter->tx_descriptor,
                          const_cast<std::uint8_t *>(source),
                          kSourceWidthPixels, kChunkRows, 0U);
    initialize_descriptor(adapter->rx_descriptor, destination,
                          kDestinationVirtualWidthPixels, kChunkRows,
                          dst_x_pixels);
    if (esp_cache_msync(const_cast<std::uint8_t *>(source), kSourceBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK ||
        esp_cache_msync(destination, kDestinationSpanBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                            ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK ||
        esp_cache_msync(adapter->tx_descriptor, kDescriptorStorageBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK ||
        esp_cache_msync(adapter->rx_descriptor, kDescriptorStorageBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK) {
        adapter->timing_active.store(false, std::memory_order_release);
        return ESP_FAIL;
    }
    adapter->completion_status.store(ESP_OK, std::memory_order_release);
    adapter->state.store(State::Queued, std::memory_order_release);
    const esp_err_t enqueue_result = dma2d_enqueue(
        adapter->pool, &adapter->transaction_config, adapter->transaction);
    if (enqueue_result != ESP_OK) {
        adapter->state.store(State::Idle, std::memory_order_release);
        adapter->timing_active.store(false, std::memory_order_release);
        return enqueue_result;
    }
    if (timing != nullptr) {
        adapter->wait_active.store(true, std::memory_order_release);
    }
    const std::uint64_t wait_start = timing == nullptr ? 0U :
        static_cast<std::uint64_t>(esp_timer_get_time());
    const BaseType_t wait_result = xSemaphoreTake(adapter->complete,
                                                  kCompletionTimeout);
    if (timing != nullptr) {
        adapter->wait_active.store(false, std::memory_order_release);
    }
    const std::uint64_t wait_end = timing == nullptr ? 0U :
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (wait_result != pdTRUE) {
        /* The pinned v5.5.4 cancellation path has no transaction identity
         * and races the DMA2D ISR.  Leave every callback-reachable object
         * retained instead. */
        (void)terminalize_ambiguous(adapter);
        adapter->timing_active.store(false, std::memory_order_release);
        return ESP_ERR_TIMEOUT;
    }
    if (adapter->state.load(std::memory_order_acquire) != State::Completed) {
        /* A setup failure wakes the task without ever granting ownership of
         * the buffers back to the caller. */
        (void)terminalize_ambiguous(adapter);
        adapter->timing_active.store(false, std::memory_order_release);
        return static_cast<esp_err_t>(adapter->completion_status.load(
            std::memory_order_acquire));
    }
    if (esp_cache_msync(adapter->rx_descriptor, kDescriptorStorageBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK ||
        adapter->rx_descriptor->err_eof != 0U) {
        adapter->state.store(State::Failed, std::memory_order_release);
        adapter->timing_active.store(false, std::memory_order_release);
        return ESP_FAIL;
    }
    if (esp_cache_msync(destination, kDestinationSpanBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) {
        adapter->state.store(State::Failed, std::memory_order_release);
        adapter->timing_active.store(false, std::memory_order_release);
        return ESP_FAIL;
    }
    const auto status = static_cast<esp_err_t>(
        adapter->completion_status.load(std::memory_order_acquire));
    if (status != ESP_OK || !return_to_idle(adapter)) {
        (void)terminalize_ambiguous(adapter);
        adapter->timing_active.store(false, std::memory_order_release);
        return status == ESP_OK ? ESP_FAIL : status;
    }
    const std::uint64_t total_end = timing == nullptr ? 0U :
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (timing != nullptr) {
        copy_timing(adapter, timing, total_end - total_start,
                    wait_end - wait_start);
        adapter->timing_active.store(false, std::memory_order_release);
    }
    return status;
}

esp_err_t destroy(Adapter *adapter) noexcept
{
    if (adapter == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const State state = adapter->state.load(std::memory_order_acquire);
    if (state != State::Idle && state != State::Failed) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = dma2d_release_pool(adapter->pool);
    if (result != ESP_OK) {
        return result;
    }
    adapter->pool = nullptr;
    free_context(adapter);
    return ESP_OK;
}

bool lifetime_must_be_retained(const Adapter *adapter) noexcept
{
    return adapter != nullptr &&
           adapter->state.load(std::memory_order_acquire) ==
               State::RetainedAmbiguous;
}

} // namespace p4_nano_dma2d_copy
