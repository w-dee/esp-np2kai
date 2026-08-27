/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_exact2x_grouped_store.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
constexpr TickType_t kSchedulerHealthDelayTicks = 1U;

static_assert(kTileBytes == 102400U);
static_assert(kTileDestinationBytes == 409600U);
static_assert(kTileCount == 5U);
static_assert(kDestinationBytes == 2048000U);
static_assert(kRequiredAlignmentBytes == 64U);
static_assert(kWarmupSamples == 8U);
static_assert(kMeasuredSamples == 128U);
static_assert(kFinalValidationSamples == 1U);
static_assert(kSchedulerHealthDelayTicks > 0U);

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

struct PhaseStats final {
    MetricStats kernel;
    MetricStats cache_sync;
    MetricStats service;
    std::uint32_t original_crc_before = 0U;
    std::uint32_t original_crc_after = 0U;
    std::uint32_t rotated_crc = 0U;
    std::uint32_t destination_crc = 0U;
    bool source_immutable = false;
    bool byte_exact = false;
    bool final_validation = false;
};

struct TileBuffers final {
    std::uint8_t *tile_a = nullptr;
    std::uint8_t *tile_b = nullptr;
};

struct Allocations final {
    std::uint8_t *destination = nullptr;
    std::uint8_t *control_snapshot = nullptr;
    TileBuffers tiles{};
};

std::array<PhaseStats, 2U> s_phase_stats{};

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

void reset_stats(PhaseStats *stats) noexcept
{
    if (stats == nullptr) {
        return;
    }
    stats->kernel.reset();
    stats->cache_sync.reset();
    stats->service.reset();
    stats->original_crc_before = 0U;
    stats->original_crc_after = 0U;
    stats->rotated_crc = 0U;
    stats->destination_crc = 0U;
    stats->source_immutable = false;
    stats->byte_exact = false;
    stats->final_validation = false;
}

std::uint64_t percentile(const MetricStats &stats, std::size_t numerator) noexcept
{
    if (stats.stored == 0U) {
        return 0U;
    }
    const std::size_t index = std::min(
        stats.stored - 1U, (stats.stored * numerator + 99U) / 100U - 1U);
    return stats.samples[index];
}

std::uint64_t average(const MetricStats &stats) noexcept
{
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < stats.stored; ++index) {
        total += stats.samples[index];
    }
    return stats.stored == 0U ? 0U : total / stats.stored;
}

void print_metric(const char *helper, const char *metric,
                  MetricStats *stats)
{
    if (helper == nullptr || metric == nullptr || stats == nullptr) {
        return;
    }
    std::sort(stats->samples.begin(), stats->samples.begin() + stats->stored);
    const std::size_t count = stats->stored;
    const std::uint64_t min_us = count == 0U ? 0U : stats->samples.front();
    const std::uint64_t max_us = count == 0U ? 0U : stats->samples[count - 1U];
    const std::uint64_t p50_us = percentile(*stats, 50U);
    const std::uint64_t p95_us = percentile(*stats, 95U);
    const std::uint64_t p99_us = percentile(*stats, 99U);
    const bool ordered = min_us <= p50_us && p50_us <= p95_us &&
        p95_us <= p99_us && p99_us <= max_us;
    std::printf("P4_NANO_EXACT2X_GROUPED_%s helper=%s count=%zu"
                " min_us=%" PRIu64 " average_us=%" PRIu64
                " p50_us=%" PRIu64 " p95_us=%" PRIu64
                " p99_us=%" PRIu64 " max_us=%" PRIu64 " order=%s\n",
                metric, helper, count, min_us, average(*stats), p50_us,
                p95_us, p99_us, max_us, ordered ? "PASS" : "FAIL");
}

bool normalize_destination(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
}

bool sync_destination(std::uint8_t *destination,
                      std::uint64_t *elapsed_us) noexcept
{
    if (destination == nullptr || elapsed_us == nullptr) {
        return false;
    }
    const std::uint64_t start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const bool result = esp_cache_msync(
        destination, kDestinationBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
    *elapsed_us = static_cast<std::uint64_t>(esp_timer_get_time()) - start;
    return result;
}

bool prepare_internal_tile(ppa_client_handle_t client,
                           const std::uint8_t *original,
                           std::uint8_t *tile, std::size_t tile_index) noexcept
{
    if (client == nullptr || original == nullptr || tile == nullptr) {
        return false;
    }
    const ppa_srm_oper_config_t operation = exact2x::make_tile_operation(
        original, tile, tile_index, PPA_TRANS_MODE_BLOCKING, nullptr);
    return ppa_do_scale_rotate_mirror(client, &operation) == ESP_OK;
}

bool run_pie_frame(bool grouped, ppa_client_handle_t client,
                   const std::uint8_t *original, TileBuffers *tiles,
                   std::uint8_t *destination, std::uint64_t *kernel_us,
                   std::uint32_t *rotated_crc) noexcept
{
    if (client == nullptr || original == nullptr || tiles == nullptr ||
        destination == nullptr || kernel_us == nullptr) {
        return false;
    }
    *kernel_us = 0U;
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        std::uint8_t *tile = (tile_index & 1U) == 0U ? tiles->tile_a :
                                                        tiles->tile_b;
        /* PPA preparation is deliberately before the PIE timer boundary. */
        if (!prepare_internal_tile(client, original, tile, tile_index)) {
            return false;
        }
        auto *tile_destination = reinterpret_cast<std::uint16_t *>(
            destination + tile_index * kTileDestinationBytes);
        const auto *tile_source = reinterpret_cast<const std::uint16_t *>(tile);
        const std::uint64_t start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        if (grouped) {
            p4_nano_display::exact2x_pie_tile128_grouped64_aligned(
                tile_source, tile_destination);
        } else {
            p4_nano_display::exact2x_pie_tile128_aligned(
                tile_source, tile_destination);
        }
        const std::uint64_t end =
            static_cast<std::uint64_t>(esp_timer_get_time());
        if (end < start) {
            return false;
        }
        *kernel_us += end - start;
        if (rotated_crc != nullptr) {
            crc = exact2x::crc32_update(crc, tile, kTileBytes);
        }
    }
    if (rotated_crc != nullptr) {
        *rotated_crc = crc ^ 0xffffffffU;
    }
    return true;
}

bool run_phase(const char *helper, bool grouped, ppa_client_handle_t client,
               const std::uint8_t *original, TileBuffers *tiles,
               std::uint8_t *destination, PhaseStats *stats) noexcept
{
    if (helper == nullptr || client == nullptr || original == nullptr ||
        tiles == nullptr || destination == nullptr || stats == nullptr) {
        return false;
    }
    reset_stats(stats);
    stats->original_crc_before = p4_nano_display::crc32(
        original, exact2x::kOriginalSourceBytes);
    for (std::size_t index = 0U;
         index < kWarmupSamples + kMeasuredSamples; ++index) {
        if (!normalize_destination(destination)) {
            return false;
        }
        std::uint64_t kernel_us = 0U;
        if (!run_pie_frame(grouped, client, original, tiles, destination,
                           &kernel_us, nullptr)) {
            return false;
        }
        std::uint64_t cache_us = 0U;
        if (!sync_destination(destination, &cache_us)) {
            return false;
        }
        if (index >= kWarmupSamples) {
            stats->kernel.add(kernel_us);
            stats->cache_sync.add(cache_us);
            stats->service.add(kernel_us + cache_us);
        }
        if ((index + 1U) % 64U == 0U) {
            vTaskDelay(kSchedulerHealthDelayTicks);
        }
    }

    std::uint32_t final_rotated_crc = 0U;
    std::uint64_t final_kernel_us = 0U;
    std::uint64_t final_cache_us = 0U;
    if (!normalize_destination(destination) ||
        !run_pie_frame(grouped, client, original, tiles, destination,
                       &final_kernel_us, &final_rotated_crc) ||
        !sync_destination(destination, &final_cache_us)) {
        return false;
    }
    stats->final_validation = true;
    stats->rotated_crc = final_rotated_crc;
    stats->original_crc_after = p4_nano_display::crc32(
        original, exact2x::kOriginalSourceBytes);
    stats->destination_crc = p4_nano_display::crc32(
        destination, kDestinationBytes);
    stats->source_immutable =
        stats->original_crc_before == stats->original_crc_after;
    stats->byte_exact = stats->source_immutable &&
        stats->original_crc_before == kExpectedOriginalCrc &&
        stats->rotated_crc == kExpectedRotatedCrc &&
        stats->destination_crc == kExpectedDestinationCrc &&
        exact2x::expected_frame_matches(
            reinterpret_cast<const std::uint16_t *>(original),
            reinterpret_cast<const std::uint16_t *>(destination));
    print_metric(helper, "KERNEL", &stats->kernel);
    print_metric(helper, "CACHE_SYNC", &stats->cache_sync);
    print_metric(helper, "SERVICE", &stats->service);
    const bool result = stats->kernel.stored == kMeasuredSamples &&
        stats->cache_sync.stored == kMeasuredSamples &&
        stats->service.stored == kMeasuredSamples && stats->byte_exact &&
        stats->final_validation;
    std::printf("P4_NANO_EXACT2X_GROUPED_CORRECTNESS helper=%s"
                " original_crc_before=0x%08" PRIx32
                " original_crc_after=0x%08" PRIx32
                " rotated_crc=0x%08" PRIx32 " output_crc=0x%08" PRIx32
                " source_immutable=%d byte_exact=%d final_validation=%d"
                " samples=%zu result=%s\n",
                helper, stats->original_crc_before, stats->original_crc_after,
                stats->rotated_crc, stats->destination_crc,
                stats->source_immutable ? 1 : 0, stats->byte_exact ? 1 : 0,
                stats->final_validation ? 1 : 0, stats->kernel.stored,
                result ? "PASS" : "FAIL");
    return result;
}

void print_timer_control() noexcept
{
    constexpr std::size_t kSamples = 32U;
    std::uint64_t total = 0U;
    std::uint64_t maximum = 0U;
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        std::uint64_t elapsed = 0U;
        for (std::size_t tile = 0U; tile < kTileCount; ++tile) {
            const std::uint64_t start =
                static_cast<std::uint64_t>(esp_timer_get_time());
            const std::uint64_t end =
                static_cast<std::uint64_t>(esp_timer_get_time());
            elapsed += end - start;
        }
        total += elapsed;
        maximum = std::max(maximum, elapsed);
    }
    std::printf("P4_NANO_EXACT2X_GROUPED_TIMER_CONTROL calls_per_frame=10"
                " samples=%zu average_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                kSamples, total / kSamples, maximum);
}

void free_allocations(Allocations *allocations) noexcept
{
    if (allocations == nullptr) {
        return;
    }
    heap_caps_free(allocations->destination);
    heap_caps_free(allocations->control_snapshot);
    heap_caps_free(allocations->tiles.tile_a);
    heap_caps_free(allocations->tiles.tile_b);
    *allocations = {};
}

bool allocate(const p4_nano_exact2x_grouped_store::Input &input,
              Allocations *allocations) noexcept
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
    allocations->tiles.tile_a = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kTileBytes,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    allocations->tiles.tile_b = static_cast<std::uint8_t *>(
        heap_caps_aligned_alloc(kRequiredAlignmentBytes, kTileBytes,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const bool valid = allocations->destination != nullptr &&
        allocations->control_snapshot != nullptr &&
        allocations->tiles.tile_a != nullptr && allocations->tiles.tile_b != nullptr &&
        esp_ptr_external_ram(allocations->destination) &&
        esp_ptr_external_ram(allocations->control_snapshot) &&
        esp_ptr_internal(allocations->tiles.tile_a) &&
        esp_ptr_internal(allocations->tiles.tile_b) &&
        allocations->tiles.tile_a != allocations->tiles.tile_b &&
        reinterpret_cast<std::uintptr_t>(allocations->destination) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->control_snapshot) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->tiles.tile_a) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(allocations->tiles.tile_b) % 64U == 0U &&
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
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->control_snapshot, kDestinationBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(allocations->tiles.tile_a, kTileBytes,
                        allocations->tiles.tile_b, kTileBytes) &&
        !ranges_overlap(allocations->tiles.tile_a, kTileBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->tiles.tile_b, kTileBytes,
                        input.original_source, exact2x::kOriginalSourceBytes) &&
        !ranges_overlap(allocations->tiles.tile_a, kTileBytes,
                        allocations->destination, kDestinationBytes) &&
        !ranges_overlap(allocations->tiles.tile_b, kTileBytes,
                        allocations->destination, kDestinationBytes) &&
        !ranges_overlap(allocations->tiles.tile_a, kTileBytes,
                        allocations->control_snapshot, kDestinationBytes) &&
        !ranges_overlap(allocations->tiles.tile_b, kTileBytes,
                        allocations->control_snapshot, kDestinationBytes);
    std::printf("P4_NANO_EXACT2X_GROUPED_ALLOCATION destination=%p"
                " control_snapshot=%p tile_a=%p tile_b=%p"
                " destination_bytes=%zu tile_bytes=%zu alignment=64"
                " destination_mode=separate_psram tile_memory=internal"
                " disjoint=%s\n",
                static_cast<void *>(allocations->destination),
                static_cast<void *>(allocations->control_snapshot),
                static_cast<void *>(allocations->tiles.tile_a),
                static_cast<void *>(allocations->tiles.tile_b),
                kDestinationBytes, kTileBytes, valid ? "PASS" : "FAIL");
    return valid;
}

const char *kernel_class(double reduction_percent) noexcept
{
    return reduction_percent < 3.0 ? "G0" :
        reduction_percent < 7.0 ? "G1" : "G2";
}

void print_comparison(const char *metric, const MetricStats &current,
                      const MetricStats &grouped)
{
    const double current_average = static_cast<double>(average(current));
    const double grouped_average = static_cast<double>(average(grouped));
    const double delta = current_average - grouped_average;
    const double reduction = current_average > 0.0
        ? delta / current_average * 100.0 : 0.0;
    const double speedup = grouped_average > 0.0
        ? current_average / grouped_average : 0.0;
    std::printf("P4_NANO_EXACT2X_GROUPED_COMPARISON metric=%s"
                " current_average_us=%.3f grouped_average_us=%.3f"
                " delta_us=%.3f reduction_percent=%.3f speedup=%.6f"
                " current_p95_us=%" PRIu64 " grouped_p95_us=%" PRIu64
                " current_p99_us=%" PRIu64 " grouped_p99_us=%" PRIu64
                " current_max_us=%" PRIu64 " grouped_max_us=%" PRIu64
                " p95_delta_us=%" PRId64 " p99_delta_us=%" PRId64
                " max_delta_us=%" PRId64
                " order=PASS\n",
                metric, current_average, grouped_average, delta, reduction,
                speedup, percentile(current, 95U), percentile(grouped, 95U),
                percentile(current, 99U), percentile(grouped, 99U),
                current.stored == 0U ? 0U : current.samples[current.stored - 1U],
                grouped.stored == 0U ? 0U : grouped.samples[grouped.stored - 1U],
                static_cast<std::int64_t>(percentile(grouped, 95U)) -
                    static_cast<std::int64_t>(percentile(current, 95U)),
                static_cast<std::int64_t>(percentile(grouped, 99U)) -
                    static_cast<std::int64_t>(percentile(current, 99U)),
                static_cast<std::int64_t>(grouped.stored == 0U ? 0U :
                    grouped.samples[grouped.stored - 1U]) -
                    static_cast<std::int64_t>(current.stored == 0U ? 0U :
                        current.samples[current.stored - 1U]));
}

} // namespace

namespace p4_nano_exact2x_grouped_store {

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
        std::printf("P4_NANO_EXACT2X_GROUPED_RESULT=FAIL reason=input\n");
        return ESP_ERR_INVALID_ARG;
    }

    Allocations allocations{};
    if (!allocate(input, &allocations)) {
        free_allocations(&allocations);
        std::printf("P4_NANO_EXACT2X_GROUPED_RESULT=FAIL reason=allocation\n");
        return ESP_ERR_NO_MEM;
    }
    std::printf("P4_NANO_EXACT2X_GROUPED_CONFIG display_profile=lower2"
                " dpi_source=PLL_F240M requested_dpi_mhz=34.285713"
                " predicted_refresh_hz=29.426767 lane_mbps=500 lanes=2"
                " htotal=880 vtotal=1324 pixel_format=RGB565"
                " framebuffer_count=1 dma2d=0 source=internal"
                " destination=separate_psram tile_width=128 tile_count=5"
                " internal_buffers=2 control_helper=current"
                " candidate_helper=grouped64 scanout=active\n");
    std::printf("P4_NANO_EXACT2X_GROUPED_MEASUREMENT_CONTRACT"
                " ppa_prep_timed=0 timed_rotated_crc=0 timed_output_crc=0"
                " timed_pixel_validation=0 timed_memcmp=0"
                " final_validation_crc=1 final_pixel_validation=1"
                " final_control_candidate_memcmp=1"
                " warmup=8 measured=128 final_validation=1"
                " scheduler_delay_cadence=every_64_frames"
                " timer_calls_per_frame=10 service=kernel_plus_cache_sync\n");
    std::printf("P10K-D0 CONTROLLED VARIABLE = PIE STORE GROUPING ONLY\n");
    print_timer_control();

    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    const esp_err_t register_result = ppa_register_client(&client_config, &client);
    if (register_result != ESP_OK || client == nullptr) {
        free_allocations(&allocations);
        std::printf("P4_NANO_EXACT2X_GROUPED_RESULT=FAIL reason=ppa_register\n");
        return register_result == ESP_OK ? ESP_FAIL : register_result;
    }
    PhaseStats &current = s_phase_stats[0];
    PhaseStats &grouped = s_phase_stats[1];
    reset_stats(&current);
    reset_stats(&grouped);
    const bool current_ok = run_phase(
        "current", false, client, input.original_source, &allocations.tiles,
        allocations.destination, &current);
    if (current_ok) {
        std::memcpy(allocations.control_snapshot, allocations.destination,
                    kDestinationBytes);
    }
    std::printf("P4_NANO_EXACT2X_GROUPED_PHASE helper=current result=%s\n",
                current_ok ? "PASS" : "FAIL");
    const bool grouped_ok = current_ok && run_phase(
        "grouped64", true, client, input.original_source, &allocations.tiles,
        allocations.destination, &grouped);
    const bool equivalent = grouped_ok &&
        std::memcmp(allocations.control_snapshot, allocations.destination,
                    kDestinationBytes) == 0;
    std::printf("P4_NANO_EXACT2X_GROUPED_EQUIVALENCE reference=current"
                " byte_exact=%d result=%s\n", equivalent ? 1 : 0,
                equivalent ? "PASS" : "FAIL");
    std::printf("P4_NANO_EXACT2X_GROUPED_PHASE helper=grouped64 result=%s\n",
                grouped_ok ? "PASS" : "FAIL");

    const bool correctness_ok = current_ok && grouped_ok && equivalent;
    double kernel_reduction = 0.0;
    double service_regression = 0.0;
    bool service_guard = false;
    if (correctness_ok) {
        print_comparison("KERNEL", current.kernel, grouped.kernel);
        print_comparison("CACHE_SYNC", current.cache_sync, grouped.cache_sync);
        print_comparison("SERVICE", current.service, grouped.service);
        const double current_kernel = static_cast<double>(average(current.kernel));
        const double grouped_kernel = static_cast<double>(average(grouped.kernel));
        kernel_reduction = current_kernel > 0.0
            ? (current_kernel - grouped_kernel) / current_kernel * 100.0 : 0.0;
        const double current_service = static_cast<double>(average(current.service));
        const double grouped_service = static_cast<double>(average(grouped.service));
        service_regression = current_service > 0.0
            ? (grouped_service - current_service) / current_service * 100.0 : 0.0;
        service_guard = service_regression <= 1.0;
    }
    const char *integrated_gate = !correctness_ok ? "D" :
        (kernel_reduction >= 3.0 && service_guard) ? "A" :
        kernel_reduction < 3.0 ? "B" : "C";
    std::printf("P4_NANO_EXACT2X_GROUPED_CLASSIFICATION"
                " kernel=%s kernel_reduction_percent=%.3f"
                " direction=%s"
                " service_regression_percent=%.3f service_guard=%s"
                " integrated_gate=%s\n",
                kernel_class(kernel_reduction), kernel_reduction,
                kernel_reduction < 0.0 ? "REGRESSION" : "IMPROVEMENT",
                service_regression, service_guard ? "PASS" : "FAIL",
                integrated_gate);
    const esp_err_t unregister_result = ppa_unregister_client(client);
    const bool result = current_ok && grouped_ok && equivalent &&
        unregister_result == ESP_OK;
    std::printf("P4_NANO_EXACT2X_GROUPED_RESULT=%s\n",
                result ? "PASS" : "FAIL");
    free_allocations(&allocations);
    return result ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_exact2x_grouped_store
