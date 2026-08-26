/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"

#include <algorithm>
#include <array>
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
#include "freertos/task.h"

#include "p4_nano_display/p4_nano_display_exact2x.hpp"
#include "p4_nano_display/p4_nano_display_pattern.hpp"

namespace {

using namespace p4_nano_exact2x_internal_source;

static_assert(kOriginalSourceBytes == 512000U);
static_assert(kRotatedSourceBytes == 512000U);
static_assert(kDestinationBytes == 2048000U);
static_assert(kTileBytes == 102400U);
static_assert(kTileDestinationBytes == 409600U);
static_assert(kTileCount * kTileDestinationBytes == kDestinationBytes);

struct PhaseStats final {
    std::array<std::uint64_t, kMeasuredSamples> kernel{};
    std::array<std::uint64_t, kMeasuredSamples> cache{};
    std::array<std::uint64_t, kMeasuredSamples> service{};
    std::size_t stored = 0U;
    std::uint32_t original_crc_before = 0U;
    std::uint32_t original_crc_after = 0U;
    std::uint32_t rotated_crc = 0U;
    std::uint32_t destination_crc = 0U;
    bool source_immutable = false;
    bool byte_exact = false;
    bool final_validation = false;
    bool cache_ok = true;
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
    stats->kernel.fill(0U);
    stats->cache.fill(0U);
    stats->service.fill(0U);
    stats->stored = 0U;
    stats->original_crc_before = 0U;
    stats->original_crc_after = 0U;
    stats->rotated_crc = 0U;
    stats->destination_crc = 0U;
    stats->source_immutable = false;
    stats->byte_exact = false;
    stats->final_validation = false;
    stats->cache_ok = true;
}

template <std::size_t N>
void print_metric(const char *source, const char *metric,
                  std::array<std::uint64_t, N> &samples,
                  std::size_t count)
{
    std::sort(samples.begin(), samples.begin() + count);
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        total += samples[index];
    }
    const auto percentile = [&samples, count](std::size_t numerator) {
        if (count == 0U) {
            return std::uint64_t{0U};
        }
        const std::size_t index =
            std::min(count - 1U, (count * numerator + 99U) / 100U - 1U);
        return samples[index];
    };
    std::printf("P4_NANO_EXACT2X_SOURCE_%s source=%s count=%zu min_us=%" PRIu64
                " average_us=%" PRIu64 " p50_us=%" PRIu64
                " p95_us=%" PRIu64 " p99_us=%" PRIu64
                " max_us=%" PRIu64 "\n",
                metric, source, count, count == 0U ? 0U : samples[0],
                count == 0U ? 0U : total / count, percentile(50U),
                percentile(95U), percentile(99U),
                count == 0U ? 0U : samples[count - 1U]);
}

std::uint32_t crc32_update_impl(std::uint32_t crc, const std::uint8_t *bytes,
                                std::size_t length) noexcept
{
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

bool expected_frame_matches_impl(const std::uint16_t *original,
                                  const std::uint16_t *destination) noexcept
{
    if (original == nullptr || destination == nullptr) {
        return false;
    }
    for (std::size_t source_y = 0U; source_y < 400U; ++source_y) {
        for (std::size_t source_x = 0U; source_x < 640U; ++source_x) {
            const std::uint16_t pixel = original[source_y * 640U + source_x];
            const std::size_t output_x = source_y;
            const std::size_t output_y = 639U - source_x;
            for (std::size_t oy = 0U; oy < 2U; ++oy) {
                for (std::size_t ox = 0U; ox < 2U; ++ox) {
                    const std::size_t offset =
                        (2U * output_y + oy) * 800U +
                        2U * output_x + ox;
                    if (destination[offset] != pixel) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

ppa_srm_oper_config_t make_operation_impl(const std::uint8_t *original,
                                           std::uint8_t *tile,
                                           std::size_t tile_index,
                                           ppa_trans_mode_t mode,
                                           void *user_data)
{
    const std::uint32_t source_x =
        640U - static_cast<std::uint32_t>(tile_index + 1U) * 128U;
    return ppa_srm_oper_config_t{
        .in = {
            .buffer = original,
            .pic_w = 640U,
            .pic_h = 400U,
            .block_w = 128U,
            .block_h = 400U,
            .block_offset_x = source_x,
            .block_offset_y = 0U,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = tile,
            .buffer_size = static_cast<std::uint32_t>(kTileBytes),
            .pic_w = 400U,
            .pic_h = 128U,
            .block_offset_x = 0U,
            .block_offset_y = 0U,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0F,
        .scale_y = 1.0F,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0U,
        .mode = mode,
        .user_data = user_data,
    };
}

bool prepare_candidate_tile(ppa_client_handle_t client,
                             const std::uint8_t *original,
                             std::uint8_t *tile, std::size_t tile_index)
{
    const ppa_srm_oper_config_t operation =
        make_operation_impl(original, tile, tile_index,
                            PPA_TRANS_MODE_BLOCKING, nullptr);
    return ppa_do_scale_rotate_mirror(client, &operation) == ESP_OK;
}

bool normalize_destination(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
}

bool sync_destination(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                               ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}

bool run_pie_frame(bool candidate, ppa_client_handle_t client,
                   const std::uint8_t *original,
                   const std::uint8_t *control_source,
                   std::uint8_t *tile_a, std::uint8_t *tile_b,
                   std::uint8_t *destination, std::uint64_t *kernel_us,
                   std::uint32_t *final_rotated_crc)
{
    if (kernel_us == nullptr) {
        return false;
    }
    *kernel_us = 0U;
    std::uint32_t rotated_crc = 0xffffffffU;
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        std::uint8_t *internal_tile =
            (tile_index & 1U) == 0U ? tile_a : tile_b;
        if (candidate && !prepare_candidate_tile(client, original,
                                                  internal_tile, tile_index)) {
            return false;
        }
        const auto *source = candidate
            ? reinterpret_cast<const std::uint16_t *>(internal_tile)
            : reinterpret_cast<const std::uint16_t *>(
                  control_source + tile_index * kTileBytes);
        auto *tile_destination = reinterpret_cast<std::uint16_t *>(
            destination + tile_index * kTileDestinationBytes);
        const std::uint64_t start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        p4_nano_display::exact2x_pie_tile128_aligned(source,
                                                      tile_destination);
        const std::uint64_t end =
            static_cast<std::uint64_t>(esp_timer_get_time());
        if (end < start) {
            return false;
        }
        *kernel_us += end - start;
        if (candidate && final_rotated_crc != nullptr) {
            rotated_crc = crc32_update_impl(rotated_crc, internal_tile, kTileBytes);
        }
    }
    if (candidate && final_rotated_crc != nullptr) {
        *final_rotated_crc = rotated_crc ^ 0xffffffffU;
    }
    return true;
}

bool run_phase(const char *source_name, bool candidate,
               ppa_client_handle_t client, const std::uint8_t *original,
               const std::uint8_t *control_source, std::uint8_t *tile_a,
               std::uint8_t *tile_b, std::uint8_t *destination,
               PhaseStats *stats)
{
    if (source_name == nullptr || original == nullptr || destination == nullptr ||
        stats == nullptr || (!candidate && control_source == nullptr)) {
        return false;
    }
    reset_stats(stats);
    stats->original_crc_before = p4_nano_display::crc32(
        original, kOriginalSourceBytes);
    if (stats->original_crc_before != kExpectedOriginalCrc) {
        stats->cache_ok = false;
    }
    if (candidate) {
        if (client == nullptr || tile_a == nullptr || tile_b == nullptr) {
            return false;
        }
    } else if (p4_nano_display::crc32(control_source, kRotatedSourceBytes) !=
                   kExpectedRotatedCrc ||
               esp_cache_msync(const_cast<std::uint8_t *>(control_source),
                               kRotatedSourceBytes,
                               ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                   ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
        stats->cache_ok = false;
    }

    for (std::size_t index = 0U;
         stats->cache_ok &&
         index < kWarmupSamples + kMeasuredSamples; ++index) {
        if (!normalize_destination(destination) ||
            (!candidate && esp_cache_msync(
                 const_cast<std::uint8_t *>(control_source),
                 kRotatedSourceBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK)) {
            stats->cache_ok = false;
            break;
        }
        std::uint64_t kernel_us = 0U;
        if (!run_pie_frame(candidate, client, original, control_source, tile_a,
                           tile_b, destination, &kernel_us, nullptr)) {
            stats->cache_ok = false;
            break;
        }
        const std::uint64_t cache_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const bool cache_ok = sync_destination(destination);
        const std::uint64_t cache_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - cache_start;
        if (!cache_ok) {
            stats->cache_ok = false;
            break;
        }
        if (index >= kWarmupSamples) {
            const std::size_t sample = index - kWarmupSamples;
            stats->kernel[sample] = kernel_us;
            stats->cache[sample] = cache_us;
            stats->service[sample] = kernel_us + cache_us;
            stats->stored = sample + 1U;
        }
        if ((index + 1U) % 64U == 0U) {
            vTaskDelay(1);
        }
    }

    std::uint32_t final_rotated_crc = 0U;
    std::uint64_t final_kernel_us = 0U;
    if (stats->cache_ok && normalize_destination(destination) &&
        run_pie_frame(candidate, client, original, control_source, tile_a,
                      tile_b, destination, &final_kernel_us,
                      &final_rotated_crc) &&
        sync_destination(destination)) {
        stats->final_validation = true;
        stats->rotated_crc = candidate
            ? final_rotated_crc
            : p4_nano_display::crc32(control_source, kRotatedSourceBytes);
    }
    stats->original_crc_after = p4_nano_display::crc32(
        original, kOriginalSourceBytes);
    stats->destination_crc = p4_nano_display::crc32(
        destination, kDestinationBytes);
    stats->source_immutable =
        stats->original_crc_before == stats->original_crc_after;
    stats->byte_exact = stats->final_validation &&
        stats->rotated_crc == kExpectedRotatedCrc &&
        stats->destination_crc == kExpectedDestinationCrc &&
        expected_frame_matches_impl(reinterpret_cast<const std::uint16_t *>(original),
                                reinterpret_cast<const std::uint16_t *>(
                                    destination));
    print_metric(source_name, "KERNEL", stats->kernel, stats->stored);
    print_metric(source_name, "CACHE_SYNC", stats->cache, stats->stored);
    print_metric(source_name, "SERVICE", stats->service, stats->stored);
    const bool result = stats->cache_ok && stats->stored == kMeasuredSamples &&
        stats->source_immutable && stats->byte_exact &&
        stats->final_validation;
    std::printf("P4_NANO_EXACT2X_SOURCE_CORRECTNESS source=%s"
                " original_crc_before=0x%08" PRIx32
                " original_crc_after=0x%08" PRIx32
                " rotated_crc=0x%08" PRIx32
                " output_crc=0x%08" PRIx32
                " source_immutable=%d byte_exact=%d final_validation=%d"
                " samples=%zu result=%s\n",
                source_name, stats->original_crc_before,
                stats->original_crc_after, stats->rotated_crc,
                stats->destination_crc, stats->source_immutable ? 1 : 0,
                stats->byte_exact ? 1 : 0, stats->final_validation ? 1 : 0,
                stats->stored, result ? "PASS" : "FAIL");
    return result;
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
    std::printf("P4_NANO_EXACT2X_INTERNAL_TIMER_CONTROL calls_per_frame=10"
                " samples=%zu average_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                kSamples, total / kSamples, maximum);
}

} // namespace

namespace p4_nano_exact2x_internal_source {

ppa_srm_oper_config_t make_tile_operation(const std::uint8_t *original,
                                          std::uint8_t *tile,
                                          std::size_t tile_index,
                                          ppa_trans_mode_t mode,
                                          void *user_data) noexcept
{
    return make_operation_impl(original, tile, tile_index, mode, user_data);
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *bytes,
                           std::size_t length) noexcept
{
    return crc32_update_impl(crc, bytes, length);
}

bool expected_frame_matches(const std::uint16_t *original,
                            const std::uint16_t *destination) noexcept
{
    return expected_frame_matches_impl(original, destination);
}

esp_err_t run(const Input &input)
{
    const bool input_valid = input.original_source != nullptr &&
        input.original_source_bytes == kOriginalSourceBytes &&
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes == kOriginalSourceBytes &&
        input.active_framebuffer != nullptr &&
        input.active_framebuffer_bytes == kDestinationBytes &&
        esp_ptr_external_ram(input.original_source);
    if (!input_valid) {
        std::printf("P4_NANO_EXACT2X_SOURCE_RESULT=FAIL reason=input\n");
        return ESP_ERR_INVALID_ARG;
    }

    auto *control_source = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kRotatedSourceBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *destination = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *tile_a = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kTileBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *tile_b = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kTileBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const bool allocations_valid = control_source != nullptr &&
        destination != nullptr && tile_a != nullptr && tile_b != nullptr &&
        esp_ptr_external_ram(control_source) && esp_ptr_external_ram(destination) &&
        esp_ptr_internal(tile_a) && esp_ptr_internal(tile_b) && tile_a != tile_b &&
        reinterpret_cast<std::uintptr_t>(control_source) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(destination) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(tile_a) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(tile_b) % 64U == 0U &&
        !ranges_overlap(tile_a, kTileBytes, tile_b, kTileBytes) &&
        !ranges_overlap(tile_a, kTileBytes, input.original_source,
                        kOriginalSourceBytes) &&
        !ranges_overlap(tile_b, kTileBytes, input.original_source,
                        kOriginalSourceBytes) &&
        !ranges_overlap(tile_a, kTileBytes, control_source,
                        kRotatedSourceBytes) &&
        !ranges_overlap(tile_b, kTileBytes, control_source,
                        kRotatedSourceBytes) &&
        !ranges_overlap(tile_a, kTileBytes, destination,
                        kDestinationBytes) &&
        !ranges_overlap(tile_b, kTileBytes, destination,
                        kDestinationBytes) &&
        !ranges_overlap(tile_a, kTileBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(tile_b, kTileBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(tile_a, kTileBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(tile_b, kTileBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(tile_a, kTileBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes) &&
        !ranges_overlap(tile_b, kTileBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes) &&
        !ranges_overlap(control_source, kRotatedSourceBytes,
                        input.original_source, kOriginalSourceBytes) &&
        !ranges_overlap(control_source, kRotatedSourceBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(control_source, kRotatedSourceBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(control_source, kRotatedSourceBytes,
                        input.active_framebuffer, input.active_framebuffer_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.original_source,
                        kOriginalSourceBytes) &&
        !ranges_overlap(destination, kDestinationBytes,
                        input.active_framebuffer, kDestinationBytes) &&
        !ranges_overlap(destination, kDestinationBytes,
                        input.presentation_slot0, input.presentation_slot_bytes) &&
        !ranges_overlap(destination, kDestinationBytes,
                        input.presentation_slot1, input.presentation_slot_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, control_source,
                        kRotatedSourceBytes);
    std::printf("P4_NANO_EXACT2X_SOURCE_ALLOCATION control_source=%p"
                " destination=%p tile_a=%p tile_b=%p alignment=64"
                " control_external=%d destination_external=%d"
                " tile_a_internal=%d tile_b_internal=%d disjoint=%s\n",
                static_cast<void *>(control_source), static_cast<void *>(destination),
                static_cast<void *>(tile_a), static_cast<void *>(tile_b),
                control_source != nullptr && esp_ptr_external_ram(control_source),
                destination != nullptr && esp_ptr_external_ram(destination),
                tile_a != nullptr && esp_ptr_internal(tile_a),
                tile_b != nullptr && esp_ptr_internal(tile_b),
                allocations_valid ? "PASS" : "FAIL");
    if (!allocations_valid) {
        heap_caps_free(control_source);
        heap_caps_free(destination);
        heap_caps_free(tile_a);
        heap_caps_free(tile_b);
        std::printf("P4_NANO_EXACT2X_SOURCE_RESULT=FAIL reason=allocation\n");
        return ESP_ERR_NO_MEM;
    }

    const auto *original = input.original_source;
    for (std::size_t source_y = 0U; source_y < 400U; ++source_y) {
        for (std::size_t source_x = 0U; source_x < 640U; ++source_x) {
            reinterpret_cast<std::uint16_t *>(control_source)[
                (639U - source_x) * 400U + source_y] =
                reinterpret_cast<const std::uint16_t *>(original)[
                    source_y * 640U + source_x];
        }
    }

    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    esp_err_t result = ppa_register_client(&client_config, &client);
    if (result != ESP_OK || client == nullptr) {
        heap_caps_free(control_source);
        heap_caps_free(destination);
        heap_caps_free(tile_a);
        heap_caps_free(tile_b);
        std::printf("P4_NANO_EXACT2X_SOURCE_RESULT=FAIL reason=ppa_register\n");
        return result == ESP_OK ? ESP_FAIL : result;
    }

    std::printf("P4_NANO_EXACT2X_SOURCE_MODE control=psram"
                " candidate=internal destination=separate_psram"
                " tile_count=5 tile_source_bytes=102400"
                " tile_destination_bytes=409600 scanout=background\n");
    print_timer_control();
    const bool control_ok = run_phase(
        "psram", false, client, original, control_source, tile_a, tile_b,
        destination, &s_phase_stats[0]);
    const bool candidate_ok = run_phase(
        "internal", true, client, original, control_source, tile_a, tile_b,
        destination, &s_phase_stats[1]);
    const esp_err_t unregister_result = ppa_unregister_client(client);
    const bool result_ok = control_ok && candidate_ok &&
        unregister_result == ESP_OK;
    std::printf("P4_NANO_EXACT2X_SOURCE_RESULT=%s\n",
                result_ok ? "PASS" : "FAIL");
    heap_caps_free(control_source);
    heap_caps_free(destination);
    heap_caps_free(tile_a);
    heap_caps_free(tile_b);
    return result_ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_exact2x_internal_source
