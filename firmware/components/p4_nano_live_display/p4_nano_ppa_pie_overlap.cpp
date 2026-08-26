/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_ppa_pie_overlap.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "p4_nano_display/p4_nano_display.hpp"
#include "p4_nano_display/p4_nano_display_exact2x.hpp"
#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"

namespace {

namespace exact2x = p4_nano_exact2x_internal_source;

constexpr std::size_t kTileBytes = exact2x::kTileBytes;
constexpr std::size_t kTileDestinationBytes = exact2x::kTileDestinationBytes;
constexpr std::size_t kTileCount = exact2x::kTileCount;
constexpr std::size_t kDestinationBytes = exact2x::kDestinationBytes;
constexpr std::size_t kRequiredAlignmentBytes = exact2x::kRequiredAlignmentBytes;
constexpr std::size_t kWarmupSamples = exact2x::kWarmupSamples;
constexpr std::size_t kMeasuredSamples = exact2x::kMeasuredSamples;
constexpr std::size_t kFinalValidationSamples = exact2x::kFinalValidationSamples;
constexpr std::uint32_t kExpectedOriginalCrc = exact2x::kExpectedOriginalCrc;
constexpr std::uint32_t kExpectedRotatedCrc = exact2x::kExpectedRotatedCrc;
constexpr std::uint32_t kExpectedDestinationCrc = exact2x::kExpectedDestinationCrc;
constexpr TickType_t kPpaWaitTicks = pdMS_TO_TICKS(5000U);
static_assert(kPpaWaitTicks > 0);
static_assert(kTileCount == 5U);
static_assert(kTileBytes == 102400U);
static_assert(kTileDestinationBytes == 409600U);
static_assert(kDestinationBytes == 2048000U);
static_assert(kRequiredAlignmentBytes == 64U);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

bool ranges_overlap(const void *first, std::size_t first_size,
                    const void *second, std::size_t second_size) noexcept
{
    if (first == nullptr || second == nullptr || first_size == 0U ||
        second_size == 0U) {
        return false;
    }
    const auto first_start = reinterpret_cast<std::uintptr_t>(first);
    const auto second_start = reinterpret_cast<std::uintptr_t>(second);
    return first_start < second_start + second_size &&
           second_start < first_start + first_size;
}

struct MetricStats final {
    std::array<std::uint64_t, kMeasuredSamples> samples{};
    std::size_t stored = 0U;

    void reset() noexcept
    {
        samples.fill(0U);
        stored = 0U;
    }

    void add(std::uint64_t value) noexcept
    {
        if (stored < samples.size()) {
            samples[stored++] = value;
        }
    }
};

std::uint64_t percentile_from_sorted(const MetricStats &stats,
                                     std::size_t numerator) noexcept
{
    if (stats.stored == 0U) {
        return 0U;
    }
    const std::size_t index = std::min(
        stats.stored - 1U, (stats.stored * numerator + 99U) / 100U - 1U);
    return stats.samples[index];
}

void print_metric(const char *mode, const char *metric, MetricStats &stats)
{
    // Establish one deterministic order before deriving any distribution
    // statistic.  The printf call only consumes already-computed values.
    std::sort(stats.samples.begin(), stats.samples.begin() + stats.stored);
    const std::size_t count = stats.stored;
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        total += stats.samples[index];
    }
    const std::uint64_t minimum = count == 0U ? 0U : stats.samples.front();
    const std::uint64_t p50 = percentile_from_sorted(stats, 50U);
    const std::uint64_t p95 = percentile_from_sorted(stats, 95U);
    const std::uint64_t p99 = percentile_from_sorted(stats, 99U);
    const std::uint64_t maximum = count == 0U ? 0U : stats.samples[count - 1U];
    const bool ordered = minimum <= p50 && p50 <= p95 && p95 <= p99 &&
        p99 <= maximum;
    std::printf("P4_NANO_PPA_PIE_OVERLAP_METRIC mode=%s metric=%s count=%zu"
                " min_us=%" PRIu64 " average_us=%" PRIu64
                " p50_us=%" PRIu64 " p95_us=%" PRIu64
                " p99_us=%" PRIu64 " max_us=%" PRIu64 " order=%s\n",
                mode, metric, count, minimum, count == 0U ? 0U : total / count,
                p50, p95, p99, maximum, ordered ? "PASS" : "FAIL");
}

void print_burst_metric(const char *burst, const char *metric,
                        MetricStats &stats)
{
    std::sort(stats.samples.begin(), stats.samples.begin() + stats.stored);
    const std::size_t count = stats.stored;
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        total += stats.samples[index];
    }
    const std::uint64_t minimum = count == 0U ? 0U : stats.samples.front();
    const std::uint64_t p50 = percentile_from_sorted(stats, 50U);
    const std::uint64_t p95 = percentile_from_sorted(stats, 95U);
    const std::uint64_t p99 = percentile_from_sorted(stats, 99U);
    const std::uint64_t maximum = count == 0U ? 0U : stats.samples[count - 1U];
    const bool ordered = minimum <= p50 && p50 <= p95 && p95 <= p99 &&
        p99 <= maximum;
    std::printf("P4_NANO_PPA_PIE_BURST_METRIC burst=%s metric=%s count=%zu"
                " min_us=%" PRIu64 " average_us=%" PRIu64
                " p50_us=%" PRIu64 " p95_us=%" PRIu64
                " p99_us=%" PRIu64 " max_us=%" PRIu64 " order=%s\n",
                burst, metric, count, minimum, count == 0U ? 0U : total / count,
                p50, p95, p99, maximum, ordered ? "PASS" : "FAIL");
}

struct CompletionContext final {
    StaticSemaphore_t storage{};
    SemaphoreHandle_t done = nullptr;
    std::atomic<std::uint32_t> callback_count{0U};
    std::atomic<std::uint32_t> callback_failures{0U};
    // Queue depth is one, so this is at most one.  It is incremented before
    // submission and cleared only after task-side semaphore observation.
    std::atomic<std::uint32_t> in_flight{0U};
    std::atomic<bool> cleanup_failed{false};
    std::atomic<bool> lifetime_must_be_retained{false};
};

CompletionContext s_completion_context{};

constexpr std::size_t kCleanupWaitAttempts = 1U;

bool ppa_done_callback(ppa_client_handle_t, ppa_event_data_t *, void *user_data)
{
    auto *context = static_cast<CompletionContext *>(user_data);
    if (context == nullptr || context->done == nullptr) {
        return false;
    }
    context->callback_count.fetch_add(1U, std::memory_order_relaxed);
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xSemaphoreGiveFromISR(context->done, &higher_priority_task_woken) !=
        pdTRUE) {
        context->callback_failures.fetch_add(1U, std::memory_order_relaxed);
    }
    return higher_priority_task_woken == pdTRUE;
}

enum class BufferState : std::uint8_t {
    Free,
    PpaInFlight,
    PpaReady,
    PieConsuming,
};

struct TileBuffers final {
    std::uint8_t *tile[2] = {nullptr, nullptr};
    BufferState state[2] = {BufferState::Free, BufferState::Free};
};

struct FrameMetrics final {
    std::uint64_t total_us = 0U;
    std::uint64_t first_ppa_latency_us = 0U;
    std::uint64_t ppa_completion_observed_us = 0U;
    std::uint64_t pie_active_us = 0U;
    std::uint64_t cache_sync_us = 0U;
    std::uint64_t ppa_wait_exposed_us = 0U;
    std::uint64_t ppa_wait_first_us = 0U;
    std::uint64_t ppa_wait_later_aggregate_us = 0U;
    std::uint64_t ppa_api_call_wall_us = 0U;
    std::uint32_t produced_mask = 0U;
    std::uint32_t completed_mask = 0U;
    std::uint32_t consumed_mask = 0U;
    std::uint32_t written_mask = 0U;
    std::uint32_t rotated_crc = 0U;
    std::uint32_t callback_count = 0U;
    bool ok = false;
};

struct PhaseStats final {
    MetricStats total;
    MetricStats first_ppa_latency;
    MetricStats ppa_completion_observed;
    MetricStats pie_active;
    MetricStats cache_sync;
    MetricStats ppa_wait_exposed;
    MetricStats ppa_api_call_wall;
    std::uint32_t original_crc_before = 0U;
    std::uint32_t original_crc_after = 0U;
    std::uint32_t rotated_crc = 0U;
    std::uint32_t destination_crc = 0U;
    bool source_immutable = false;
    bool byte_exact = false;
    bool final_validation = false;
};

struct BurstPhaseStats final {
    MetricStats total;
    MetricStats first_ppa_latency;
    MetricStats ppa_completion_observed;
    MetricStats pie_active;
    MetricStats cache_sync;
    MetricStats ppa_wait_exposed;
    MetricStats ppa_api_call_wall;
    MetricStats ppa_wait_first;
    MetricStats ppa_wait_later_aggregate;
    std::uint32_t original_crc_before = 0U;
    std::uint32_t original_crc_after = 0U;
    std::uint32_t rotated_crc = 0U;
    std::uint32_t destination_crc = 0U;
    std::uint32_t callbacks_seen = 0U;
    bool source_immutable = false;
    bool byte_exact = false;
    bool final_validation = false;
};

struct BurstCandidate final {
    const char *name;
    ppa_data_burst_length_t length;
};

constexpr std::array<BurstCandidate, 3U> kBurstCandidates{{
    {"128", PPA_DATA_BURST_LENGTH_128},
    {"64", PPA_DATA_BURST_LENGTH_64},
    {"32", PPA_DATA_BURST_LENGTH_32},
}};
static_assert(kBurstCandidates.size() == 3U);
static_assert(kBurstCandidates[0].length == PPA_DATA_BURST_LENGTH_128);
static_assert(kBurstCandidates[1].length == PPA_DATA_BURST_LENGTH_64);
static_assert(kBurstCandidates[2].length == PPA_DATA_BURST_LENGTH_32);

std::array<PhaseStats, 2U> s_phase_stats{};
std::array<BurstPhaseStats, 3U> s_burst_phase_stats{};

bool normalize_destination(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
}

bool sync_destination(std::uint8_t *destination, std::uint64_t *elapsed_us) noexcept
{
    if (elapsed_us == nullptr) {
        return false;
    }
    const std::uint64_t start = static_cast<std::uint64_t>(esp_timer_get_time());
    const bool result = esp_cache_msync(
        destination, kDestinationBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
    *elapsed_us = static_cast<std::uint64_t>(esp_timer_get_time()) - start;
    return result;
}

bool observe_ppa_completion(CompletionContext *context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    const std::uint32_t previous =
        context->in_flight.fetch_sub(1U, std::memory_order_acq_rel);
    if (previous == 0U) {
        context->in_flight.fetch_add(1U, std::memory_order_release);
        return false;
    }
    return true;
}

bool wait_for_ppa(CompletionContext *context, std::uint64_t *wait_us) noexcept
{
    if (context == nullptr || context->done == nullptr || wait_us == nullptr) {
        return false;
    }
    const std::uint64_t start = static_cast<std::uint64_t>(esp_timer_get_time());
    const BaseType_t result = xSemaphoreTake(context->done, kPpaWaitTicks);
    *wait_us = static_cast<std::uint64_t>(esp_timer_get_time()) - start;
    if (result != pdTRUE) {
        return false;
    }
    return observe_ppa_completion(context);
}

bool submit_ppa(ppa_client_handle_t client, CompletionContext *context,
                const std::uint8_t *source, std::uint8_t *tile,
                std::size_t tile_index, ppa_trans_mode_t mode,
                std::uint64_t *api_call_wall_us) noexcept
{
    if (client == nullptr || context == nullptr || context->done == nullptr ||
        tile == nullptr || api_call_wall_us == nullptr ||
        xSemaphoreTake(context->done, 0U) == pdTRUE) {
        return false;
    }
    const ppa_srm_oper_config_t operation = exact2x::make_tile_operation(
        source, tile, tile_index, mode, context);
    context->in_flight.fetch_add(1U, std::memory_order_release);
    const std::uint64_t start = static_cast<std::uint64_t>(esp_timer_get_time());
    const esp_err_t result = ppa_do_scale_rotate_mirror(client, &operation);
    *api_call_wall_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - start;
    if (result != ESP_OK) {
        context->in_flight.fetch_sub(1U, std::memory_order_acq_rel);
    }
    return result == ESP_OK;
}

bool drain_outstanding_ppa(CompletionContext *context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    for (std::size_t attempt = 0U;
         attempt < kCleanupWaitAttempts &&
         context->in_flight.load(std::memory_order_acquire) != 0U;
         ++attempt) {
        std::uint64_t wait_us = 0U;
        (void)wait_for_ppa(context, &wait_us);
    }
    return context->in_flight.load(std::memory_order_acquire) == 0U;
}

bool validate_masks(const FrameMetrics &metrics) noexcept
{
    constexpr std::uint32_t kAllTiles = (1U << kTileCount) - 1U;
    return metrics.produced_mask == kAllTiles &&
           metrics.completed_mask == kAllTiles &&
           metrics.consumed_mask == kAllTiles &&
           metrics.written_mask == kAllTiles;
}

bool run_pie_tile(const std::uint8_t *source, std::uint8_t *destination,
                  std::uint64_t *elapsed_us) noexcept
{
    if (source == nullptr || destination == nullptr || elapsed_us == nullptr) {
        return false;
    }
    const std::uint64_t start = static_cast<std::uint64_t>(esp_timer_get_time());
    p4_nano_display::exact2x_pie_tile128_aligned(
        reinterpret_cast<const std::uint16_t *>(source),
        reinterpret_cast<std::uint16_t *>(destination));
    *elapsed_us = static_cast<std::uint64_t>(esp_timer_get_time()) - start;
    return true;
}

bool run_sequential_frame(ppa_client_handle_t client,
                          CompletionContext *context,
                          const std::uint8_t *source,
                          TileBuffers *buffers, std::uint8_t *destination,
                          // Timed frames pass nullptr; only final validation
                          // requests the expensive rotated-tile CRC.
                          std::uint32_t *final_rotated_crc,
                          FrameMetrics *metrics) noexcept
{
    if (client == nullptr || context == nullptr || source == nullptr ||
        buffers == nullptr || destination == nullptr || metrics == nullptr) {
        return false;
    }
    *metrics = {};
    const std::uint64_t callback_start =
        context->callback_count.load(std::memory_order_relaxed);
    std::uint32_t rotated_crc = 0xffffffffU;
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        const std::size_t buffer_index = tile_index & 1U;
        if (buffers->state[buffer_index] != BufferState::Free) {
            return false;
        }
        buffers->state[buffer_index] = BufferState::PpaInFlight;
        std::uint64_t api_call_us = 0U;
        const std::uint64_t ppa_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        if (!submit_ppa(client, context, source, buffers->tile[buffer_index],
                        tile_index, PPA_TRANS_MODE_BLOCKING, &api_call_us)) {
            return false;
        }
        std::uint64_t callback_wait_us = 0U;
        if (!wait_for_ppa(context, &callback_wait_us)) {
            return false;
        }
        const std::uint64_t ppa_elapsed =
            static_cast<std::uint64_t>(esp_timer_get_time()) - ppa_start;
        metrics->ppa_completion_observed_us += ppa_elapsed;
        if (tile_index == 0U) {
            metrics->first_ppa_latency_us = ppa_elapsed;
        }
        metrics->ppa_wait_exposed_us += callback_wait_us;
        metrics->ppa_api_call_wall_us += api_call_us;
        metrics->produced_mask |= 1U << tile_index;
        metrics->completed_mask |= 1U << tile_index;
        buffers->state[buffer_index] = BufferState::PpaReady;
        if (final_rotated_crc != nullptr) {
            rotated_crc = exact2x::crc32_update(
                rotated_crc, buffers->tile[buffer_index], kTileBytes);
        }
        buffers->state[buffer_index] = BufferState::PieConsuming;
        std::uint64_t pie_us = 0U;
        auto *tile_destination = destination + tile_index * kTileDestinationBytes;
        if (!run_pie_tile(buffers->tile[buffer_index], tile_destination,
                          &pie_us)) {
            return false;
        }
        metrics->pie_active_us += pie_us;
        metrics->consumed_mask |= 1U << tile_index;
        metrics->written_mask |= 1U << tile_index;
        buffers->state[buffer_index] = BufferState::Free;
    }
    if (final_rotated_crc != nullptr) {
        *final_rotated_crc = rotated_crc ^ 0xffffffffU;
        metrics->rotated_crc = *final_rotated_crc;
    }
    metrics->callback_count = context->callback_count.load(
        std::memory_order_relaxed) - static_cast<std::uint32_t>(callback_start);
    metrics->ok = validate_masks(*metrics) && metrics->callback_count == kTileCount;
    return metrics->ok;
}

bool run_overlap_frame(ppa_client_handle_t client,
                       CompletionContext *context,
                       const std::uint8_t *source,
                       TileBuffers *buffers, std::uint8_t *destination,
                       // Timed frames pass nullptr; only final validation
                       // requests the expensive rotated-tile CRC.
                       std::uint32_t *final_rotated_crc,
                       FrameMetrics *metrics) noexcept
{
    if (client == nullptr || context == nullptr || source == nullptr ||
        buffers == nullptr || destination == nullptr || metrics == nullptr) {
        return false;
    }
    *metrics = {};
    const std::uint64_t callback_start =
        context->callback_count.load(std::memory_order_relaxed);
    std::uint32_t rotated_crc = 0xffffffffU;
    std::size_t current_buffer = 0U;
    std::uint64_t api_call_us = 0U;
    const std::uint64_t first_submit_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (!submit_ppa(client, context, source, buffers->tile[current_buffer], 0U,
                    PPA_TRANS_MODE_NON_BLOCKING, &api_call_us)) {
        return false;
    }
    metrics->ppa_api_call_wall_us += api_call_us;
    metrics->produced_mask |= 1U;
    buffers->state[current_buffer] = BufferState::PpaInFlight;
    std::uint64_t wait_us = 0U;
    if (!wait_for_ppa(context, &wait_us)) {
        return false;
    }
    metrics->ppa_wait_exposed_us += wait_us;
    metrics->ppa_wait_first_us = wait_us;
    metrics->first_ppa_latency_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - first_submit_start;
    metrics->ppa_completion_observed_us += metrics->first_ppa_latency_us;
    metrics->completed_mask |= 1U;
    buffers->state[current_buffer] = BufferState::PpaReady;
    if (final_rotated_crc != nullptr) {
        rotated_crc = exact2x::crc32_update(
            rotated_crc, buffers->tile[current_buffer], kTileBytes);
    }

    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        const std::size_t next_buffer = current_buffer ^ 1U;
        std::uint64_t ppa_start = 0U;
        if (tile_index + 1U < kTileCount) {
            if (buffers->state[next_buffer] != BufferState::Free) {
                return false;
            }
            buffers->state[next_buffer] = BufferState::PpaInFlight;
            ppa_start = static_cast<std::uint64_t>(esp_timer_get_time());
            if (!submit_ppa(client, context, source,
                            buffers->tile[next_buffer], tile_index + 1U,
                            PPA_TRANS_MODE_NON_BLOCKING, &api_call_us)) {
                return false;
            }
            metrics->ppa_api_call_wall_us += api_call_us;
            metrics->produced_mask |= 1U << (tile_index + 1U);
        }

        if (buffers->state[current_buffer] != BufferState::PpaReady) {
            return false;
        }
        buffers->state[current_buffer] = BufferState::PieConsuming;
        std::uint64_t pie_us = 0U;
        if (!run_pie_tile(
                buffers->tile[current_buffer],
                destination + tile_index * kTileDestinationBytes, &pie_us)) {
            return false;
        }
        metrics->pie_active_us += pie_us;
        metrics->consumed_mask |= 1U << tile_index;
        metrics->written_mask |= 1U << tile_index;
        buffers->state[current_buffer] = BufferState::Free;

        if (tile_index + 1U < kTileCount) {
            if (!wait_for_ppa(context, &wait_us)) {
                return false;
            }
            metrics->ppa_wait_exposed_us += wait_us;
            metrics->ppa_wait_later_aggregate_us += wait_us;
            metrics->ppa_completion_observed_us +=
                static_cast<std::uint64_t>(esp_timer_get_time()) - ppa_start;
            metrics->completed_mask |= 1U << (tile_index + 1U);
            buffers->state[next_buffer] = BufferState::PpaReady;
            if (final_rotated_crc != nullptr) {
                rotated_crc = exact2x::crc32_update(
                    rotated_crc, buffers->tile[next_buffer], kTileBytes);
            }
            current_buffer = next_buffer;
        }
    }
    if (final_rotated_crc != nullptr) {
        *final_rotated_crc = rotated_crc ^ 0xffffffffU;
        metrics->rotated_crc = *final_rotated_crc;
    }
    metrics->callback_count = context->callback_count.load(
        std::memory_order_relaxed) - static_cast<std::uint32_t>(callback_start);
    metrics->ok = validate_masks(*metrics) && metrics->callback_count == kTileCount;
    return metrics->ok;
}

bool validate_frame(const std::uint8_t *original, const std::uint8_t *destination,
                    const FrameMetrics &metrics,
                    std::uint32_t *destination_crc) noexcept
{
    if (original == nullptr || destination == nullptr || destination_crc == nullptr) {
        return false;
    }
    *destination_crc = p4_nano_display::crc32(destination, kDestinationBytes);
    return metrics.rotated_crc == kExpectedRotatedCrc &&
           *destination_crc == kExpectedDestinationCrc &&
           exact2x::expected_frame_matches(
               reinterpret_cast<const std::uint16_t *>(original),
               reinterpret_cast<const std::uint16_t *>(destination));
}

void print_timer_control() noexcept
{
    constexpr std::size_t kSamples = 32U;
    std::uint64_t total = 0U;
    std::uint64_t maximum = 0U;
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        std::uint64_t elapsed = 0U;
        for (std::size_t bracket = 0U; bracket < kTileCount; ++bracket) {
            const std::uint64_t start =
                static_cast<std::uint64_t>(esp_timer_get_time());
            const std::uint64_t end =
                static_cast<std::uint64_t>(esp_timer_get_time());
            elapsed += end - start;
        }
        total += elapsed;
        maximum = std::max(maximum, elapsed);
    }
    std::printf("P4_NANO_PPA_PIE_OVERLAP_TIMER_CONTROL calls_per_frame=10"
                " samples=%zu average_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                kSamples, total / kSamples, maximum);
}

bool run_phase(const char *mode, bool overlap, ppa_client_handle_t client,
               CompletionContext *context, const std::uint8_t *source,
               TileBuffers *buffers, std::uint8_t *destination,
               PhaseStats *stats) noexcept
{
    if (mode == nullptr || context == nullptr || source == nullptr ||
        buffers == nullptr || destination == nullptr || stats == nullptr) {
        return false;
    }
    stats->total.reset();
    stats->first_ppa_latency.reset();
    stats->ppa_completion_observed.reset();
    stats->pie_active.reset();
    stats->cache_sync.reset();
    stats->ppa_wait_exposed.reset();
    stats->ppa_api_call_wall.reset();
    stats->original_crc_before = p4_nano_display::crc32(
        source, exact2x::kOriginalSourceBytes);
    if (stats->original_crc_before != kExpectedOriginalCrc) {
        return false;
    }
    std::printf("P4_NANO_PPA_PIE_OVERLAP_PHASE mode=%s\n", mode);
    const std::uint64_t callback_before =
        context->callback_count.load(std::memory_order_relaxed);
    for (std::size_t index = 0U;
         index < kWarmupSamples + kMeasuredSamples; ++index) {
        if (!normalize_destination(destination)) {
            return false;
        }
        FrameMetrics metrics{};
        const std::uint64_t start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const bool frame_ok = overlap
            ? run_overlap_frame(client, context, source, buffers, destination,
                                nullptr, &metrics)
            : run_sequential_frame(client, context, source, buffers, destination,
                                   nullptr, &metrics);
        if (!frame_ok) {
            return false;
        }
        if (!sync_destination(destination, &metrics.cache_sync_us)) {
            return false;
        }
        metrics.total_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - start;
        if (index >= kWarmupSamples) {
            stats->total.add(metrics.total_us);
            stats->first_ppa_latency.add(metrics.first_ppa_latency_us);
            stats->ppa_completion_observed.add(metrics.ppa_completion_observed_us);
            stats->pie_active.add(metrics.pie_active_us);
            stats->cache_sync.add(metrics.cache_sync_us);
            stats->ppa_wait_exposed.add(metrics.ppa_wait_exposed_us);
            stats->ppa_api_call_wall.add(metrics.ppa_api_call_wall_us);
        }
        if ((index + 1U) % 64U == 0U) {
            vTaskDelay(1);
        }
    }
    if (context->callback_failures.load(std::memory_order_relaxed) != 0U ||
        context->callback_count.load(std::memory_order_relaxed) - callback_before !=
            (kWarmupSamples + kMeasuredSamples) * kTileCount) {
        return false;
    }
    std::uint32_t destination_crc = 0U;
    FrameMetrics final_metrics{};
    if (!normalize_destination(destination) ||
        (overlap
             ? !run_overlap_frame(client, context, source, buffers, destination,
                                  &final_metrics.rotated_crc, &final_metrics)
             : !run_sequential_frame(client, context, source, buffers,
                                     destination, &final_metrics.rotated_crc,
                                     &final_metrics)) ||
        !sync_destination(destination, &final_metrics.cache_sync_us) ||
        !validate_frame(source, destination, final_metrics, &destination_crc)) {
        return false;
    }
    stats->original_crc_after = p4_nano_display::crc32(
        source, exact2x::kOriginalSourceBytes);
    stats->source_immutable = stats->original_crc_before == stats->original_crc_after;
    stats->rotated_crc = final_metrics.rotated_crc;
    stats->destination_crc = destination_crc;
    stats->final_validation = true;
    stats->byte_exact = stats->source_immutable &&
        stats->rotated_crc == kExpectedRotatedCrc &&
        stats->destination_crc == kExpectedDestinationCrc;
    print_metric(mode, "TOTAL_FRAME_SERVICE", stats->total);
    print_metric(mode, "FIRST_PPA_LATENCY", stats->first_ppa_latency);
    print_metric(mode, "PPA_COMPLETION_OBSERVED_LATENCY",
                 stats->ppa_completion_observed);
    print_metric(mode, "PIE_ACTIVE", stats->pie_active);
    print_metric(mode, "CACHE_SYNC", stats->cache_sync);
    print_metric(mode, "PPA_WAIT_EXPOSED", stats->ppa_wait_exposed);
    print_metric(mode, "PPA_API_CALL_WALL", stats->ppa_api_call_wall);
    std::printf("P4_NANO_PPA_PIE_OVERLAP_CORRECTNESS mode=%s"
                " original_crc_before=0x%08" PRIx32
                " original_crc_after=0x%08" PRIx32
                " rotated_crc=0x%08" PRIx32 " output_crc=0x%08" PRIx32
                " source_immutable=%d byte_exact=%d final_validation=%d"
                " produced_mask=0x%02" PRIx32
                " completed_mask=0x%02" PRIx32
                " consumed_mask=0x%02" PRIx32
                " written_mask=0x%02" PRIx32 " result=%s\n",
                mode, stats->original_crc_before, stats->original_crc_after,
                stats->rotated_crc, stats->destination_crc,
                stats->source_immutable ? 1 : 0, stats->byte_exact ? 1 : 0,
                stats->final_validation ? 1 : 0, final_metrics.produced_mask,
                final_metrics.completed_mask, final_metrics.consumed_mask,
                final_metrics.written_mask,
                stats->byte_exact ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_PIE_OVERLAP_PHASE_RESULT mode=%s result=%s\n",
                mode, stats->byte_exact ? "PASS" : "FAIL");
    return stats->byte_exact;
}

struct BurstAllocations final {
    std::uint8_t *destination = nullptr;
    std::uint8_t *control_snapshot = nullptr;
    TileBuffers buffers{};
};

void free_burst_allocations(BurstAllocations *allocations) noexcept
{
    if (allocations == nullptr) {
        return;
    }
    heap_caps_free(allocations->destination);
    heap_caps_free(allocations->control_snapshot);
    heap_caps_free(allocations->buffers.tile[0]);
    heap_caps_free(allocations->buffers.tile[1]);
    *allocations = {};
}

bool allocate_burst_allocations(
    const p4_nano_ppa_pie_overlap::Input &input,
    BurstAllocations *allocations) noexcept
{
    if (allocations == nullptr) {
        return false;
    }
    *allocations = {};
    allocations->destination = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kDestinationBytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocations->control_snapshot = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kDestinationBytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocations->buffers.tile[0] = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kTileBytes,
                                MALLOC_CAP_INTERNAL |
                                MALLOC_CAP_8BIT));
    allocations->buffers.tile[1] = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kTileBytes,
                                MALLOC_CAP_INTERNAL |
                                MALLOC_CAP_8BIT));
    const bool valid = allocations->destination != nullptr &&
        allocations->control_snapshot != nullptr &&
        allocations->buffers.tile[0] != nullptr &&
        allocations->buffers.tile[1] != nullptr &&
        esp_ptr_external_ram(allocations->destination) &&
        esp_ptr_external_ram(allocations->control_snapshot) &&
        esp_ptr_internal(allocations->buffers.tile[0]) &&
        esp_ptr_internal(allocations->buffers.tile[1]) &&
        allocations->buffers.tile[0] != allocations->buffers.tile[1] &&
        reinterpret_cast<std::uintptr_t>(allocations->destination) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->control_snapshot) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->buffers.tile[0]) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->buffers.tile[1]) % 64U == 0U &&
        !ranges_overlap(allocations->destination, kDestinationBytes,
                        allocations->control_snapshot, kDestinationBytes) &&
        !ranges_overlap(allocations->destination, kDestinationBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->destination, kDestinationBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(allocations->destination, kDestinationBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->destination, kDestinationBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        allocations->buffers.tile[1], kTileBytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->buffers.tile[1], kTileBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(allocations->buffers.tile[1], kTileBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->buffers.tile[1], kTileBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->buffers.tile[1], kTileBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->buffers.tile[0], kTileBytes,
                        allocations->control_snapshot, kDestinationBytes) &&
        !ranges_overlap(allocations->buffers.tile[1], kTileBytes,
                        allocations->control_snapshot, kDestinationBytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.presentation_slot1, input.presentation_slot_bytes);
    std::printf("P4_NANO_PPA_BURST_ALLOCATION destination=%p"
                " control_snapshot=%p tile_a=%p tile_b=%p"
                " destination_bytes=%zu tile_bytes=%zu alignment=64"
                " destination_mode=benchmark_psram_destination disjoint=%s\n",
                static_cast<void *>(allocations->destination),
                static_cast<void *>(allocations->control_snapshot),
                static_cast<void *>(allocations->buffers.tile[0]),
                static_cast<void *>(allocations->buffers.tile[1]),
                kDestinationBytes, kTileBytes, valid ? "PASS" : "FAIL");
    if (!valid) {
        free_burst_allocations(allocations);
        return false;
    }
    return true;
}

bool run_burst_phase(const BurstCandidate &candidate,
                     ppa_client_handle_t client, CompletionContext *context,
                     const std::uint8_t *source, TileBuffers *buffers,
                     std::uint8_t *destination,
                     BurstPhaseStats *stats) noexcept
{
    if (candidate.name == nullptr || client == nullptr || context == nullptr ||
        source == nullptr || buffers == nullptr || destination == nullptr ||
        stats == nullptr) {
        return false;
    }
    stats->total.reset();
    stats->first_ppa_latency.reset();
    stats->ppa_completion_observed.reset();
    stats->pie_active.reset();
    stats->cache_sync.reset();
    stats->ppa_wait_exposed.reset();
    stats->ppa_api_call_wall.reset();
    stats->ppa_wait_first.reset();
    stats->ppa_wait_later_aggregate.reset();
    stats->original_crc_before = p4_nano_display::crc32(
        source, exact2x::kOriginalSourceBytes);
    if (stats->original_crc_before != kExpectedOriginalCrc) {
        return false;
    }
    std::printf("P4_NANO_PPA_PIE_BURST_PHASE burst=%s\n", candidate.name);
    std::printf("P4_NANO_PPA_PIE_BURST_MEASUREMENT_CONTRACT burst=%s"
                " timed_rotated_crc=0 timed_output_crc=0"
                " timed_pixel_validation=0 final_validation_crc=1"
                " final_pixel_validation=1\n", candidate.name);
    const std::uint32_t callback_before = context->callback_count.load(
        std::memory_order_relaxed);
    for (std::size_t index = 0U; index < kWarmupSamples + kMeasuredSamples;
         ++index) {
        if (!normalize_destination(destination)) {
            return false;
        }
        FrameMetrics metrics{};
        const std::uint64_t start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        if (!run_overlap_frame(client, context, source, buffers, destination,
                               nullptr, &metrics)) {
            return false;
        }
        if (!sync_destination(destination, &metrics.cache_sync_us)) {
            return false;
        }
        if (metrics.ppa_wait_exposed_us != metrics.ppa_wait_first_us +
                metrics.ppa_wait_later_aggregate_us) {
            return false;
        }
        metrics.total_us = static_cast<std::uint64_t>(esp_timer_get_time()) -
            start;
        if (index >= kWarmupSamples) {
            stats->total.add(metrics.total_us);
            stats->first_ppa_latency.add(metrics.first_ppa_latency_us);
            stats->ppa_completion_observed.add(metrics.ppa_completion_observed_us);
            stats->pie_active.add(metrics.pie_active_us);
            stats->cache_sync.add(metrics.cache_sync_us);
            stats->ppa_wait_exposed.add(metrics.ppa_wait_exposed_us);
            stats->ppa_api_call_wall.add(metrics.ppa_api_call_wall_us);
            stats->ppa_wait_first.add(metrics.ppa_wait_first_us);
            stats->ppa_wait_later_aggregate.add(
                metrics.ppa_wait_later_aggregate_us);
        }
        if ((index + 1U) % 64U == 0U) {
            vTaskDelay(1);
        }
    }
    const std::uint32_t timed_callbacks = context->callback_count.load(
        std::memory_order_relaxed) - callback_before;
    if (context->callback_failures.load(std::memory_order_relaxed) != 0U ||
        timed_callbacks != (kWarmupSamples + kMeasuredSamples) * kTileCount) {
        return false;
    }
    FrameMetrics final_metrics{};
    std::uint32_t destination_crc = 0U;
    if (!normalize_destination(destination) ||
        !run_overlap_frame(client, context, source, buffers, destination,
                           &final_metrics.rotated_crc, &final_metrics) ||
        !sync_destination(destination, &final_metrics.cache_sync_us) ||
        !validate_frame(source, destination, final_metrics, &destination_crc)) {
        return false;
    }
    stats->original_crc_after = p4_nano_display::crc32(
        source, exact2x::kOriginalSourceBytes);
    stats->source_immutable = stats->original_crc_before == stats->original_crc_after;
    stats->rotated_crc = final_metrics.rotated_crc;
    stats->destination_crc = destination_crc;
    stats->final_validation = true;
    stats->byte_exact = stats->source_immutable &&
        stats->rotated_crc == kExpectedRotatedCrc &&
        stats->destination_crc == kExpectedDestinationCrc;
    stats->callbacks_seen = context->callback_count.load(std::memory_order_relaxed) -
        callback_before;
    print_burst_metric(candidate.name, "TOTAL_FRAME_SERVICE", stats->total);
    print_burst_metric(candidate.name, "FIRST_PPA_LATENCY", stats->first_ppa_latency);
    print_burst_metric(candidate.name, "PPA_COMPLETION_OBSERVED_LATENCY",
                       stats->ppa_completion_observed);
    print_burst_metric(candidate.name, "PIE_ACTIVE", stats->pie_active);
    print_burst_metric(candidate.name, "CACHE_SYNC", stats->cache_sync);
    print_burst_metric(candidate.name, "PPA_WAIT_EXPOSED", stats->ppa_wait_exposed);
    print_burst_metric(candidate.name, "PPA_API_CALL_WALL",
                       stats->ppa_api_call_wall);
    print_burst_metric(candidate.name, "PPA_WAIT_FIRST", stats->ppa_wait_first);
    print_burst_metric(candidate.name, "PPA_WAIT_LATER_AGGREGATE",
                       stats->ppa_wait_later_aggregate);
    std::printf("P4_NANO_PPA_PIE_BURST_CORRECTNESS burst=%s"
                " original_crc_before=0x%08" PRIx32
                " original_crc_after=0x%08" PRIx32
                " rotated_crc=0x%08" PRIx32 " output_crc=0x%08" PRIx32
                " source_immutable=%d byte_exact=%d final_validation=%d"
                " produced_mask=0x%02" PRIx32
                " completed_mask=0x%02" PRIx32
                " consumed_mask=0x%02" PRIx32
                " written_mask=0x%02" PRIx32 " result=%s\n",
                candidate.name, stats->original_crc_before,
                stats->original_crc_after, stats->rotated_crc,
                stats->destination_crc, stats->source_immutable ? 1 : 0,
                stats->byte_exact ? 1 : 0, stats->final_validation ? 1 : 0,
                final_metrics.produced_mask, final_metrics.completed_mask,
                final_metrics.consumed_mask, final_metrics.written_mask,
                stats->byte_exact ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_PIE_BURST_CALLBACKS burst=%s seen=%" PRIu32
                " expected=685 failures=%" PRIu32 " result=%s\n",
                candidate.name, stats->callbacks_seen,
                context->callback_failures.load(std::memory_order_relaxed),
                stats->callbacks_seen == 685U &&
                        context->callback_failures.load(std::memory_order_relaxed) == 0U
                    ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_PIE_BURST_PHASE_RESULT burst=%s result=%s\n",
                candidate.name, stats->byte_exact ? "PASS" : "FAIL");
    return stats->byte_exact;
}

const char *burst_pie_class(double reduction_percent) noexcept
{
    return reduction_percent < 1.0 ? "B0" :
        reduction_percent < 3.0 ? "B1" :
        reduction_percent < 7.0 ? "B2" : "B3";
}

const char *burst_p99_class(const BurstPhaseStats &stats) noexcept
{
    if (stats.total.stored == 0U) {
        return "C";
    }
    const std::uint64_t p99 = percentile_from_sorted(stats.total, 99U);
    return p99 < 30000U ? "A" : p99 < 33333U ? "B" : "C";
}

unsigned burst_p99_rank(const BurstPhaseStats &stats) noexcept
{
    const char *classification = burst_p99_class(stats);
    return std::strcmp(classification, "A") == 0 ? 2U :
        std::strcmp(classification, "B") == 0 ? 1U : 0U;
}

std::uint64_t metric_average(const MetricStats &stats)
{
    if (stats.stored == 0U) {
        return 0U;
    }
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < stats.stored; ++index) {
        total += stats.samples[index];
    }
    return total / stats.stored;
}

} // namespace

namespace p4_nano_ppa_pie_overlap {

bool transaction_lifetime_must_be_retained() noexcept
{
    return s_completion_context.lifetime_must_be_retained.load(
        std::memory_order_acquire);
}

esp_err_t run(const Input &input)
{
    const bool input_valid = input.original_source != nullptr &&
        input.original_source_bytes == exact2x::kOriginalSourceBytes &&
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes == exact2x::kOriginalSourceBytes &&
        input.active_framebuffer != nullptr &&
        input.active_framebuffer_bytes == kDestinationBytes &&
        esp_ptr_external_ram(input.original_source);
    if (!input_valid) {
        std::printf("P4_NANO_PPA_PIE_OVERLAP_RESULT=FAIL reason=input\n");
        return ESP_ERR_INVALID_ARG;
    }
    auto *destination = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    TileBuffers buffers{
        .tile = {
            static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
                kRequiredAlignmentBytes, kTileBytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
                kRequiredAlignmentBytes, kTileBytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        },
    };
    auto *control_snapshot = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const bool allocations_valid = destination != nullptr &&
        control_snapshot != nullptr && buffers.tile[0] != nullptr &&
        buffers.tile[1] != nullptr && esp_ptr_external_ram(destination) &&
        esp_ptr_external_ram(control_snapshot) &&
        esp_ptr_internal(buffers.tile[0]) && esp_ptr_internal(buffers.tile[1]) &&
        buffers.tile[0] != buffers.tile[1] &&
        reinterpret_cast<std::uintptr_t>(destination) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(control_snapshot) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(buffers.tile[0]) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(buffers.tile[1]) % 64U == 0U &&
        !ranges_overlap(destination, kDestinationBytes,
                        control_snapshot, kDestinationBytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.original_source,
                        exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, buffers.tile[1], kTileBytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, input.original_source,
                        exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(buffers.tile[1], kTileBytes, input.original_source,
                        exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes) &&
        !ranges_overlap(buffers.tile[1], kTileBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(buffers.tile[1], kTileBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(buffers.tile[1], kTileBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(buffers.tile[0], kTileBytes, control_snapshot,
                        kDestinationBytes) &&
        !ranges_overlap(buffers.tile[1], kTileBytes, control_snapshot,
                        kDestinationBytes) &&
        !ranges_overlap(control_snapshot, kDestinationBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(control_snapshot, kDestinationBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(control_snapshot, kDestinationBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(control_snapshot, kDestinationBytes,
                        input.presentation_slot1, input.presentation_slot_bytes);
    std::printf("P4_NANO_PPA_PIE_OVERLAP_ALLOCATION destination=%p"
                " control_snapshot=%p tile_a=%p tile_b=%p"
                " destination_bytes=%zu tile_bytes=%zu alignment=64"
                " destination_mode=benchmark_psram_destination disjoint=%s\n",
                static_cast<void *>(destination),
                static_cast<void *>(control_snapshot),
                static_cast<void *>(buffers.tile[0]),
                static_cast<void *>(buffers.tile[1]), kDestinationBytes,
                kTileBytes, allocations_valid ? "PASS" : "FAIL");
    if (!allocations_valid) {
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
        return ESP_ERR_NO_MEM;
    }

    CompletionContext *context = &s_completion_context;
    if (context->cleanup_failed.load(std::memory_order_acquire) ||
        context->in_flight.load(std::memory_order_acquire) != 0U) {
        std::printf("P4_NANO_PPA_PIE_OVERLAP_CLEANUP=FAIL_IN_FLIGHT\n");
        std::printf("P4_NANO_PPA_PIE_OVERLAP_RESULT=FAIL\n");
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
        return ESP_ERR_INVALID_STATE;
    }
    if (context->done == nullptr) {
        context->done = xSemaphoreCreateBinaryStatic(&context->storage);
    }
    if (context->done == nullptr) {
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreTake(context->done, 0U);
    context->callback_count.store(0U, std::memory_order_relaxed);
    context->callback_failures.store(0U, std::memory_order_relaxed);
    context->lifetime_must_be_retained.store(false, std::memory_order_release);
    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    esp_err_t result = ppa_register_client(&client_config, &client);
    if (result != ESP_OK || client == nullptr) {
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
        return result == ESP_OK ? ESP_FAIL : result;
    }
    const ppa_event_callbacks_t callbacks{.on_trans_done = ppa_done_callback};
    result = ppa_client_register_event_callbacks(client, &callbacks);
    if (result != ESP_OK) {
        (void)ppa_unregister_client(client);
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
        return result;
    }

    std::printf("P4_NANO_PPA_PIE_OVERLAP_CONFIG display_profile=lower2"
                " dpi_source=PLL_F240M requested_dpi_mhz=34.285713196"
                " predicted_refresh_hz=29.426767349 lane_mbps=500"
                " tile_width=128 tile_count=5 internal_buffers=2"
                " ppa_mode_candidate=non_blocking ppa_queue_depth=1"
                " framebuffer_count=1 dma2d=0"
                " destination_mode=benchmark_psram_destination\n");
    std::printf("P4_NANO_PPA_PIE_OVERLAP_MEASUREMENT_CONTRACT"
                " timed_rotated_crc=0 timed_output_crc=0"
                " timed_pixel_validation=0 final_validation_crc=1"
                " final_pixel_validation=1\n");
    print_timer_control();
    const bool control_ok = run_phase(
        "sequential", false, client, context, input.original_source, &buffers,
        destination, &s_phase_stats[0]);
    if (!control_ok) {
        result = ESP_FAIL;
    } else {
        std::memcpy(control_snapshot, destination, kDestinationBytes);
        const bool candidate_ok = run_phase(
            "overlap", true, client, context, input.original_source, &buffers,
            destination, &s_phase_stats[1]);
        if (!candidate_ok || std::memcmp(control_snapshot, destination,
                                         kDestinationBytes) != 0) {
            result = ESP_FAIL;
        }
    }
    const std::uint32_t callback_failures =
        context->callback_failures.load(std::memory_order_relaxed);
    const std::uint32_t callbacks_seen =
        context->callback_count.load(std::memory_order_relaxed);
    const bool extra_completion = xSemaphoreTake(context->done, 0U) == pdTRUE;
    if (extra_completion && !observe_ppa_completion(context)) {
        result = ESP_FAIL;
    }
    if (callback_failures != 0U || extra_completion) {
        result = ESP_FAIL;
    }
    std::printf("P4_NANO_PPA_PIE_OVERLAP_CPU_HEADROOM"
                " wall_margin_is_not_free_cpu=1"
                " blocking_api_wall_is_not_submit_overhead=1\n");
    if (result == ESP_OK) {
        const double sequential = static_cast<double>(metric_average(
            s_phase_stats[0].total));
        const double overlap = static_cast<double>(metric_average(
            s_phase_stats[1].total));
        const double saved = sequential - overlap;
        const double reduction = sequential > 0.0 ? saved / sequential * 100.0 : 0.0;
        const double speedup = overlap > 0.0 ? sequential / overlap : 0.0;
        const double sequential_margin = 33.333 - sequential / 1000.0;
        const double overlap_margin = 33.333 - overlap / 1000.0;
        const std::uint64_t overlap_pie = metric_average(s_phase_stats[1].pie_active);
        const std::uint64_t overlap_wait = metric_average(s_phase_stats[1].ppa_wait_exposed);
        const std::uint64_t overlap_cache = metric_average(s_phase_stats[1].cache_sync);
        std::printf("P4_NANO_PPA_PIE_OVERLAP_COMPARISON sequential_avg_us=%.3f"
                    " overlap_avg_us=%.3f absolute_saved_ms=%.3f"
                    " service_reduction_percent=%.3f service_speedup=%.6f"
                    " overlap_pie_active_avg_us=%" PRIu64
                    " overlap_wait_exposed_avg_us=%" PRIu64
                    " overlap_cache_avg_us=%" PRIu64
                    " sequential_wall_margin_30fps_ms=%.3f"
                    " overlap_wall_margin_30fps_ms=%.3f\n",
                    sequential, overlap, saved / 1000.0, reduction, speedup,
                    overlap_pie, overlap_wait, overlap_cache,
                    sequential_margin, overlap_margin);
        const char *improvement_class = reduction < 3.0 ? "O0" :
            reduction < 10.0 ? "O1" : reduction < 20.0 ? "O2" : "O3";
        const char *absolute_class = overlap <= 30000.0 ? "A" :
            overlap <= 33333.0 ? "B" : "C";
        std::printf("P4_NANO_PPA_PIE_OVERLAP_CLASSIFICATION improvement=%s"
                    " absolute=%s wall_margin_33fps_ms=%.3f\n",
                    improvement_class, absolute_class,
                    (33333.0 - overlap) / 1000.0);
        const double floor = static_cast<double>(metric_average(
            s_phase_stats[0].first_ppa_latency)) +
            static_cast<double>(metric_average(s_phase_stats[0].pie_active)) +
            static_cast<double>(metric_average(s_phase_stats[0].cache_sync));
        if (sequential > floor && sequential - floor > 0.0) {
            std::printf("P4_NANO_PPA_PIE_OVERLAP_PIPELINE_FLOOR measured_single_frame_floor_ms=%.3f"
                        " efficiency=%.3f\n",
                        floor / 1000.0,
                        (sequential - overlap) / (sequential - floor));
        } else {
            std::printf("P4_NANO_PPA_PIE_OVERLAP_PIPELINE_FLOOR"
                        " measured_single_frame_floor=NOT_RELIABLY_DERIVABLE\n");
        }
    }
    std::printf("P4_NANO_PPA_PIE_OVERLAP_CALLBACKS seen=%" PRIu32
                " failures=%" PRIu32 " result=%s\n",
                callbacks_seen, callback_failures,
                result == ESP_OK ? "PASS" : "FAIL");
    // Failure policy: if task-side completion cannot be observed within the
    // bounded recovery wait, deliberately retain the client, callback context,
    // PPA-visible tiles, and caller-held source rather than free DMA memory.
    const bool transactions_quiescent = drain_outstanding_ppa(context);
    esp_err_t unregister_result = ESP_ERR_INVALID_STATE;
    if (transactions_quiescent) {
        unregister_result = ppa_unregister_client(client);
    }
    const bool cleanup_pass = transactions_quiescent &&
        unregister_result == ESP_OK;
    if (!cleanup_pass) {
        context->cleanup_failed.store(true, std::memory_order_release);
        context->lifetime_must_be_retained.store(true, std::memory_order_release);
        result = result == ESP_OK ? ESP_FAIL : result;
    } else {
        heap_caps_free(destination);
        heap_caps_free(control_snapshot);
        heap_caps_free(buffers.tile[0]);
        heap_caps_free(buffers.tile[1]);
    }
    const char *cleanup_marker = !transactions_quiescent
        ? "FAIL_IN_FLIGHT"
        : unregister_result == ESP_OK ? "PASS" : "FAIL_UNREGISTER";
    std::printf("P4_NANO_PPA_PIE_OVERLAP_CLEANUP=%s\n", cleanup_marker);
    std::printf("P4_NANO_PPA_PIE_OVERLAP_RESULT=%s\n",
                result == ESP_OK ? "PASS" : "FAIL");
    return result;
}

esp_err_t run_burst_sweep(const Input &input)
{
    const bool input_valid = input.original_source != nullptr &&
        input.original_source_bytes == exact2x::kOriginalSourceBytes &&
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes == exact2x::kOriginalSourceBytes &&
        input.active_framebuffer != nullptr &&
        input.active_framebuffer_bytes == kDestinationBytes &&
        esp_ptr_external_ram(input.original_source);
    if (!input_valid) {
        std::printf("P4_NANO_PPA_BURST_RESULT=FAIL reason=input\n");
        return ESP_ERR_INVALID_ARG;
    }

    BurstAllocations allocations{};
    if (!allocate_burst_allocations(input, &allocations)) {
        std::printf("P4_NANO_PPA_BURST_RESULT=FAIL reason=allocation\n");
        return ESP_ERR_NO_MEM;
    }
    CompletionContext *context = &s_completion_context;
    if (context->cleanup_failed.load(std::memory_order_acquire) ||
        context->in_flight.load(std::memory_order_acquire) != 0U) {
        std::printf("P4_NANO_PPA_BURST_CLEANUP=FAIL_IN_FLIGHT\n");
        std::printf("P4_NANO_PPA_BURST_RESULT=FAIL\n");
        free_burst_allocations(&allocations);
        return ESP_ERR_INVALID_STATE;
    }
    if (context->done == nullptr) {
        context->done = xSemaphoreCreateBinaryStatic(&context->storage);
    }
    if (context->done == nullptr) {
        free_burst_allocations(&allocations);
        return ESP_ERR_NO_MEM;
    }

    std::printf("P4_NANO_PPA_BURST_CONFIG display_profile=lower2"
                " dpi_source=PLL_F240M requested_dpi_mhz=34.285713196"
                " predicted_refresh_hz=29.426767349 lane_mbps=500"
                " tile_width=128 tile_count=5 buffers=2 queue_depth=1"
                " bursts=128,64,32 destination_mode=benchmark_psram_destination"
                " ppa_operation=SRM pie=exact2x_q0_q1 scanout=active"
                " framebuffer_count=1 dma2d=0 task_context=unchanged\n");
    std::printf("P4_NANO_PPA_PIE_BURST_MEASUREMENT_CONTRACT"
                " timed_rotated_crc=0 timed_output_crc=0"
                " timed_pixel_validation=0 final_validation_crc=1"
                " final_pixel_validation=1 expected_callbacks_per_burst=685"
                " total_expected_callbacks=2055\n");
    std::printf("P4_NANO_PPA_PIE_BURST_CPU_HEADROOM"
                " wall_margin_is_not_free_cpu=1"
                " blocking_api_wall_is_not_submit_overhead=1"
                " semaphore_blocked_wait_may_be_used_by_runnable_task=1\n");
    print_timer_control();

    esp_err_t result = ESP_OK;
    std::uint32_t total_callbacks = 0U;
    for (std::size_t phase_index = 0U;
         phase_index < kBurstCandidates.size(); ++phase_index) {
        // Reset only after the previous client has been unregistered and all
        // completion tokens have been consumed.
        if (context->in_flight.load(std::memory_order_acquire) != 0U ||
            context->cleanup_failed.load(std::memory_order_acquire) ||
            context->lifetime_must_be_retained.load(std::memory_order_acquire) ||
            xSemaphoreTake(context->done, 0U) == pdTRUE) {
            result = ESP_ERR_INVALID_STATE;
            std::printf("P4_NANO_PPA_PIE_BURST_PHASE_RESULT burst=%s result=FAIL\n",
                        kBurstCandidates[phase_index].name);
            break;
        }
        context->in_flight.store(0U, std::memory_order_release);
        context->cleanup_failed.store(false, std::memory_order_release);
        context->callback_count.store(0U, std::memory_order_relaxed);
        context->callback_failures.store(0U, std::memory_order_relaxed);
        context->lifetime_must_be_retained.store(false, std::memory_order_release);

        const ppa_client_config_t client_config{
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1U,
            .data_burst_length = kBurstCandidates[phase_index].length,
        };
        ppa_client_handle_t client = nullptr;
        esp_err_t phase_result = ppa_register_client(&client_config, &client);
        if (phase_result == ESP_OK && client != nullptr) {
            const ppa_event_callbacks_t callbacks{.on_trans_done = ppa_done_callback};
            phase_result = ppa_client_register_event_callbacks(client, &callbacks);
        } else if (phase_result == ESP_OK) {
            phase_result = ESP_FAIL;
        }
        if (phase_result != ESP_OK) {
            if (client != nullptr) {
                const esp_err_t unregister_result = ppa_unregister_client(client);
                if (unregister_result != ESP_OK) {
                    context->cleanup_failed.store(true, std::memory_order_release);
                    context->lifetime_must_be_retained.store(
                        true, std::memory_order_release);
                    std::printf("P4_NANO_PPA_BURST_CLEANUP burst=%s"
                                " result=FAIL_UNREGISTER quiescent=0"
                                " unregistered=0\n",
                                kBurstCandidates[phase_index].name);
                }
            }
            std::printf("P4_NANO_PPA_PIE_BURST_PHASE_RESULT burst=%s result=FAIL\n",
                        kBurstCandidates[phase_index].name);
            result = phase_result;
            break;
        }

        std::printf("P4_NANO_PPA_BURST_CLIENT burst=%s data_burst_length=%s"
                    " max_pending_trans_num=1 lifecycle=REGISTERED\n",
                    kBurstCandidates[phase_index].name,
                    kBurstCandidates[phase_index].name);
        const bool phase_ok = run_burst_phase(
            kBurstCandidates[phase_index], client, context,
            input.original_source, &allocations.buffers,
            allocations.destination, &s_burst_phase_stats[phase_index]);
        const std::uint32_t callback_failures =
            context->callback_failures.load(std::memory_order_relaxed);
        const bool extra_completion = xSemaphoreTake(context->done, 0U) == pdTRUE;
        if (extra_completion) {
            phase_result = ESP_FAIL;
        }
        const bool transactions_quiescent = drain_outstanding_ppa(context);
        esp_err_t unregister_result = ESP_ERR_INVALID_STATE;
        if (transactions_quiescent) {
            unregister_result = ppa_unregister_client(client);
        }
        const bool cleanup_pass = transactions_quiescent &&
            unregister_result == ESP_OK;
        std::printf("P4_NANO_PPA_BURST_CLEANUP burst=%s result=%s"
                    " quiescent=%d unregistered=%d\n",
                    kBurstCandidates[phase_index].name,
                    cleanup_pass ? "PASS" :
                        (!transactions_quiescent ? "FAIL_IN_FLIGHT" :
                         "FAIL_UNREGISTER"),
                    transactions_quiescent ? 1 : 0,
                    unregister_result == ESP_OK ? 1 : 0);
        if (!cleanup_pass) {
            context->cleanup_failed.store(true, std::memory_order_release);
            context->lifetime_must_be_retained.store(true, std::memory_order_release);
            result = phase_result == ESP_OK ? ESP_FAIL : phase_result;
            break;
        }
        if (!phase_ok || callback_failures != 0U || extra_completion ||
            s_burst_phase_stats[phase_index].callbacks_seen != 685U) {
            result = phase_result == ESP_OK ? ESP_FAIL : phase_result;
            break;
        }
        total_callbacks += s_burst_phase_stats[phase_index].callbacks_seen;
        if (phase_index == 0U) {
            std::memcpy(allocations.control_snapshot, allocations.destination,
                        kDestinationBytes);
        } else {
            const bool equivalent = std::memcmp(
                allocations.control_snapshot, allocations.destination,
                kDestinationBytes) == 0;
            std::printf("P4_NANO_PPA_BURST_EQUIVALENCE burst=%s reference=128"
                        " byte_exact=%d result=%s\n",
                        kBurstCandidates[phase_index].name, equivalent ? 1 : 0,
                        equivalent ? "PASS" : "FAIL");
            if (!equivalent) {
                result = ESP_FAIL;
                break;
            }
        }
    }

    if (result == ESP_OK) {
        const BurstPhaseStats &reference = s_burst_phase_stats[0];
        for (std::size_t index = 1U; index < kBurstCandidates.size(); ++index) {
            const BurstPhaseStats &candidate = s_burst_phase_stats[index];
            const double reference_total = static_cast<double>(
                metric_average(reference.total));
            const double candidate_total = static_cast<double>(
                metric_average(candidate.total));
            const double reference_pie = static_cast<double>(
                metric_average(reference.pie_active));
            const double candidate_pie = static_cast<double>(
                metric_average(candidate.pie_active));
            const double total_reduction = reference_total > 0.0
                ? (reference_total - candidate_total) / reference_total * 100.0
                : 0.0;
            const double pie_reduction = reference_pie > 0.0
                ? (reference_pie - candidate_pie) / reference_pie * 100.0
                : 0.0;
            std::printf("P4_NANO_PPA_BURST_COMPARISON burst=%s reference=128"
                        " total_delta_us=%.3f total_reduction_percent=%.3f"
                        " pie_delta_us=%.3f pie_reduction_percent=%.3f"
                        " pie_class=%s total_p99_class=%s\n",
                        kBurstCandidates[index].name,
                        reference_total - candidate_total, total_reduction,
                        reference_pie - candidate_pie, pie_reduction,
                        burst_pie_class(pie_reduction), burst_p99_class(candidate));
        }
        std::size_t preferred_index = 0U;
        double preferred_reduction = 0.0;
        std::uint64_t preferred_wait = 0U;
        bool preferred_found = false;
        for (std::size_t index = 0U; index < kBurstCandidates.size(); ++index) {
            const BurstPhaseStats &candidate = s_burst_phase_stats[index];
            if (candidate.total.stored == 0U ||
                std::strcmp(burst_p99_class(candidate), "C") == 0) {
                continue;
            }
            const double reference_pie = static_cast<double>(
                metric_average(reference.pie_active));
            const double candidate_pie = static_cast<double>(
                metric_average(candidate.pie_active));
            const double reduction = reference_pie > 0.0
                ? (reference_pie - candidate_pie) / reference_pie * 100.0
                : 0.0;
            const std::uint64_t candidate_wait = metric_average(
                candidate.ppa_wait_exposed);
            const double reduction_delta = reduction - preferred_reduction;
            const bool reduction_is_better = reduction_delta > 0.001;
            const bool reduction_is_similar = reduction_delta >= -0.001 &&
                reduction_delta <= 0.001;
            const bool p99_is_better = preferred_found &&
                burst_p99_rank(candidate) >
                    burst_p99_rank(s_burst_phase_stats[preferred_index]);
            const bool p99_is_similar = preferred_found &&
                burst_p99_rank(candidate) ==
                    burst_p99_rank(s_burst_phase_stats[preferred_index]);
            const bool wait_is_better = p99_is_similar &&
                candidate_wait < preferred_wait;
            if (!preferred_found || reduction_is_better ||
                (reduction_is_similar && (p99_is_better || wait_is_better))) {
                preferred_found = true;
                preferred_index = index;
                preferred_reduction = reduction;
                preferred_wait = candidate_wait;
            }
        }
        std::printf("P4_NANO_PPA_BURST_PREFERRED preferred_burst=%s"
                    " reason=%s pie_reduction_percent=%.3f\n",
                    preferred_found ? kBurstCandidates[preferred_index].name : "none",
                    preferred_found ? "max_robust_pie_reduction" :
                        "no_phase_with_p99_under_33_333ms",
                    preferred_reduction);
    }
    std::printf("P4_NANO_PPA_BURST_CALLBACKS total=%" PRIu32
                " expected=2055 failures=%" PRIu32 " result=%s\n",
                total_callbacks,
                context->callback_failures.load(std::memory_order_relaxed),
                result == ESP_OK && total_callbacks == 2055U ? "PASS" : "FAIL");

    if (context->lifetime_must_be_retained.load(std::memory_order_acquire)) {
        std::printf("P4_NANO_PPA_BURST_SOURCE_LIFETIME=RETAINED\n");
    } else {
        free_burst_allocations(&allocations);
    }
    std::printf("P4_NANO_PPA_BURST_RESULT=%s\n",
                result == ESP_OK ? "PASS" : "FAIL");
    return result;
}

} // namespace p4_nano_ppa_pie_overlap
