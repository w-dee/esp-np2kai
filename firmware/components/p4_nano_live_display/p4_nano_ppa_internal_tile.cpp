/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_ppa_internal_tile.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "np2video_golden.h"
#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_live_display/p4_nano_ppa_rotation_reference.hpp"

namespace {

using p4_nano_ppa_internal_tile::kExpectedOutputCrc;
using p4_nano_ppa_internal_tile::kExpectedSourceCrc;
using p4_nano_ppa_internal_tile::kLargestTileBytes;
using p4_nano_ppa_internal_tile::kLargestTileWidth;
using p4_nano_ppa_internal_tile::kMeasuredSamples;
using p4_nano_ppa_internal_tile::kOutputHeight;
using p4_nano_ppa_internal_tile::kOutputBytes;
using p4_nano_ppa_internal_tile::kOutputWidth;
using p4_nano_ppa_internal_tile::kRequiredAlignmentBytes;
using p4_nano_ppa_internal_tile::kSourceBytes;
using p4_nano_ppa_internal_tile::kSourceHeight;
using p4_nano_ppa_internal_tile::kSourceWidth;
using p4_nano_ppa_internal_tile::kWarmupSamples;

static_assert(kSourceBytes == 512000U);
static_assert(kLargestTileBytes == 102400U);
static_assert(p4_nano_ppa_internal_tile::tile_bytes(32U) == 25600U);
static_assert(p4_nano_ppa_internal_tile::tile_bytes(64U) == 51200U);
static_assert(p4_nano_ppa_internal_tile::tile_bytes(128U) == 102400U);
static_assert(p4_nano_ppa_internal_tile::tile_count(32U) == 20U);
static_assert(p4_nano_ppa_internal_tile::tile_count(64U) == 10U);
static_assert(p4_nano_ppa_internal_tile::tile_count(128U) == 5U);

struct PhaseStats final {
    std::array<std::uint64_t, kMeasuredSamples> samples{};
    std::size_t stored = 0U;
    std::uint64_t total = 0U;
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0U;
};

/* These arrays are reused for the three sequential width phases.  Keeping
 * them static avoids placing bounded measurement storage on the app task
 * stack. */
std::array<PhaseStats, 3U> s_phase_stats{};

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

std::uint64_t percentile(PhaseStats &stats, std::size_t numerator)
{
    if (stats.stored == 0U) {
        return 0U;
    }
    std::sort(stats.samples.begin(), stats.samples.begin() + stats.stored);
    const std::size_t index =
        (stats.stored * numerator + 99U) / 100U - 1U;
    return stats.samples[index];
}

void print_failure(const char *reason)
{
    std::printf("P4_NANO_PPA_INTERNAL_TILE_RESULT=FAIL reason=%s\n", reason);
}

ppa_srm_oper_config_t make_operation(const std::uint8_t *source,
                                      std::uint8_t *destination,
                                      std::uint32_t tile_width,
                                      std::uint32_t tile_index)
{
    const std::uint32_t source_x =
        kSourceWidth - (tile_index + 1U) * tile_width;
    return ppa_srm_oper_config_t{
        .in = {
            .buffer = source,
            .pic_w = kSourceWidth,
            .pic_h = kSourceHeight,
            .block_w = tile_width,
            .block_h = kSourceHeight,
            .block_offset_x = source_x,
            .block_offset_y = 0U,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = destination,
            .buffer_size = static_cast<std::uint32_t>(kLargestTileBytes),
            .pic_w = kOutputWidth,
            .pic_h = tile_width,
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
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = nullptr,
    };
}

esp_err_t run_aggregate(ppa_client_handle_t client, const std::uint8_t *source,
                        std::uint8_t *tile_a, std::uint8_t *tile_b,
                        std::uint32_t tile_width)
{
    for (std::uint32_t tile_index = 0U;
         tile_index < p4_nano_ppa_internal_tile::tile_count(tile_width);
         ++tile_index) {
        std::uint8_t *destination =
            (tile_index & 1U) == 0U ? tile_a : tile_b;
        const ppa_srm_oper_config_t operation = make_operation(
            source, destination, tile_width, tile_index);
        const esp_err_t result =
            ppa_do_scale_rotate_mirror(client, &operation);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

bool run_final_validation(ppa_client_handle_t client, const std::uint8_t *source,
                          std::uint8_t *tile_a, std::uint8_t *tile_b,
                          std::uint32_t tile_width, std::uint8_t *verification)
{
    std::memset(verification, 0, kOutputBytes);
    for (std::uint32_t tile_index = 0U;
         tile_index < p4_nano_ppa_internal_tile::tile_count(tile_width);
         ++tile_index) {
        std::uint8_t *destination =
            (tile_index & 1U) == 0U ? tile_a : tile_b;
        const ppa_srm_oper_config_t operation = make_operation(
            source, destination, tile_width, tile_index);
        if (ppa_do_scale_rotate_mirror(client, &operation) != ESP_OK) {
            return false;
        }
        const std::size_t destination_offset =
            static_cast<std::size_t>(tile_index) * tile_width *
            static_cast<std::size_t>(kOutputWidth) * sizeof(std::uint16_t);
        std::memcpy(verification + destination_offset, destination,
                    p4_nano_ppa_internal_tile::tile_bytes(tile_width));
    }
    return p4_nano_ppa_rotation::reference_matches(
        reinterpret_cast<const std::uint16_t *>(source),
        reinterpret_cast<const std::uint16_t *>(verification));
}

void print_phase_config(std::uint32_t tile_width)
{
    std::printf("P4_NANO_PPA_TILE_CONFIG tile_width=%" PRIu32
                " tile_count=%" PRIu32 " tile_bytes=%zu buffers=2"
                " memory=internal alignment=%zu\n",
                tile_width,
                p4_nano_ppa_internal_tile::tile_count(tile_width),
                p4_nano_ppa_internal_tile::tile_bytes(tile_width),
                kRequiredAlignmentBytes);
}

} // namespace

namespace p4_nano_ppa_internal_tile {

esp_err_t run(const Input &input)
{
    const bool source_geometry =
        input.source != nullptr && input.source_bytes == kSourceBytes &&
        input.source_width == kSourceWidth &&
        input.source_height == kSourceHeight &&
        input.source_pitch_bytes == kSourceWidth * sizeof(std::uint16_t) &&
        input.source_bpp == 16U;
    const bool source_external =
        input.source != nullptr && esp_ptr_external_ram(input.source);
    const bool excluded_present =
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes != 0U && input.native_framebuffer != nullptr &&
        input.native_framebuffer_bytes != 0U;
    const std::uint32_t source_crc_before = source_external
        ? p4_nano_display::crc32(input.source, input.source_bytes) : 0U;
    const bool source_disjoint =
        source_external && excluded_present &&
        !ranges_overlap(input.source, input.source_bytes,
                        input.native_framebuffer, input.native_framebuffer_bytes);

    std::printf("P4_NANO_PPA_TILE_START source=%p source_geometry=%" PRIu32
                "x%" PRIu32 " output_geometry=%" PRIu32 "x%" PRIu32
                " source_bytes=%zu source_external=%d source_crc=0x%08" PRIx32
                " expected_source_crc=0x%08" PRIx32 " scanout=active\n",
                static_cast<const void *>(input.source), kSourceWidth,
                kSourceHeight, kOutputWidth, kOutputHeight, kSourceBytes,
                source_external ? 1 : 0, source_crc_before,
                kExpectedSourceCrc);
    if (!source_geometry || !source_external ||
        source_crc_before != kExpectedSourceCrc || !source_disjoint) {
        print_failure(!source_geometry ? "source_geometry" :
                      !source_external ? "source_not_external" :
                      source_crc_before != kExpectedSourceCrc ? "source_crc" :
                      "source_alias");
        return ESP_ERR_INVALID_STATE;
    }

    auto *tile_a = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kLargestTileBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *tile_b = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kLargestTileBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const bool tile_a_internal = tile_a != nullptr && esp_ptr_internal(tile_a);
    const bool tile_b_internal = tile_b != nullptr && esp_ptr_internal(tile_b);
    const std::size_t tile_a_mod = tile_a == nullptr ? 0U :
        reinterpret_cast<std::uintptr_t>(tile_a) % kRequiredAlignmentBytes;
    const std::size_t tile_b_mod = tile_b == nullptr ? 0U :
        reinterpret_cast<std::uintptr_t>(tile_b) % kRequiredAlignmentBytes;
    const bool allocation_valid =
        tile_a_internal && tile_b_internal && tile_a != tile_b &&
        tile_a_mod == 0U && tile_b_mod == 0U &&
        !ranges_overlap(tile_a, kLargestTileBytes, tile_b, kLargestTileBytes);
    const std::size_t tile_a_bytes = tile_a == nullptr
        ? 0U : heap_caps_get_allocated_size(tile_a);
    const std::size_t tile_b_bytes = tile_b == nullptr
        ? 0U : heap_caps_get_allocated_size(tile_b);
    std::printf("P4_NANO_PPA_TILE_ALLOCATION buffer_a=%p buffer_b=%p"
                " requested_a_bytes=%zu buffer_a_actual_bytes=%zu"
                " requested_b_bytes=%zu buffer_b_actual_bytes=%zu"
                " memory=internal"
                " required_alignment=%zu buffer_a_mod=%zu buffer_b_mod=%zu"
                " buffer_a_internal=%d buffer_b_internal=%d result=%s\n",
                static_cast<void *>(tile_a), static_cast<void *>(tile_b),
                kLargestTileBytes, tile_a_bytes, kLargestTileBytes, tile_b_bytes,
                kRequiredAlignmentBytes,
                tile_a_mod, tile_b_mod, tile_a_internal ? 1 : 0,
                tile_b_internal ? 1 : 0, allocation_valid ? "PASS" : "FAIL");
    if (!allocation_valid) {
        heap_caps_free(tile_a);
        heap_caps_free(tile_b);
        print_failure("internal_tile_allocation");
        return ESP_ERR_NO_MEM;
    }

    auto *verification = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignmentBytes, kOutputBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (verification == nullptr || !esp_ptr_external_ram(verification)) {
        heap_caps_free(tile_a);
        heap_caps_free(tile_b);
        heap_caps_free(verification);
        print_failure("verification_alloc");
        return ESP_ERR_NO_MEM;
    }

    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    esp_err_t result = ppa_register_client(&client_config, &client);
    if (result != ESP_OK || client == nullptr) {
        heap_caps_free(tile_a);
        heap_caps_free(tile_b);
        heap_caps_free(verification);
        print_failure("ppa_register");
        return result == ESP_OK ? ESP_FAIL : result;
    }

    bool all_ok = true;
    for (std::size_t phase = 0U; phase < 3U; ++phase) {
        const std::uint32_t tile_width = kTileWidths[phase];
        PhaseStats &stats = s_phase_stats[phase];
        stats = PhaseStats{};
        print_phase_config(tile_width);

        for (std::uint32_t sample = 0U; sample < kWarmupSamples; ++sample) {
            if (run_aggregate(client, input.source, tile_a, tile_b,
                              tile_width) != ESP_OK) {
                all_ok = false;
                break;
            }
        }
        if (!all_ok) {
            break;
        }
        for (std::uint32_t sample = 0U; sample < kMeasuredSamples; ++sample) {
            const std::int64_t start = esp_timer_get_time();
            const esp_err_t aggregate_result = run_aggregate(
                client, input.source, tile_a, tile_b, tile_width);
            const std::int64_t end = esp_timer_get_time();
            if (aggregate_result != ESP_OK || end <= start) {
                all_ok = false;
                break;
            }
            const std::uint64_t elapsed = static_cast<std::uint64_t>(end - start);
            stats.samples[stats.stored++] = elapsed;
            stats.total += elapsed;
            stats.minimum = std::min(stats.minimum, elapsed);
            stats.maximum = std::max(stats.maximum, elapsed);
            if (stats.stored % 64U == 0U) {
                vTaskDelay(1);
            }
        }
        if (!all_ok || stats.stored != kMeasuredSamples) {
            all_ok = false;
            break;
        }
        const std::uint64_t average = stats.total / stats.stored;
        std::printf("P4_NANO_PPA_TILE_SAMPLE_COUNTS tile_width=%" PRIu32
                    " warmup=%u measured=%u final_validation=%u aggregate=1\n",
                    tile_width, static_cast<unsigned>(kWarmupSamples),
                    static_cast<unsigned>(kMeasuredSamples), 1U);
        std::printf("P4_NANO_PPA_TILE_SERVICE tile_width=%" PRIu32
                    " count=%zu min_us=%" PRIu64 " average_us=%" PRIu64
                    " p50_us=%" PRIu64 " p95_us=%" PRIu64
                    " p99_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                    tile_width, stats.stored, stats.minimum, average,
                    percentile(stats, 50U), percentile(stats, 95U),
                    percentile(stats, 99U), stats.maximum);

        const bool final_ok = run_final_validation(
            client, input.source, tile_a, tile_b, tile_width, verification);
        const std::uint32_t source_crc_after = p4_nano_display::crc32(
            input.source, input.source_bytes);
        const std::uint32_t output_crc = p4_nano_display::crc32(
            verification, kOutputBytes);
        const bool source_immutable = source_crc_before == source_crc_after;
        const bool byte_exact = final_ok &&
            output_crc == kExpectedOutputCrc;
        std::printf("P4_NANO_PPA_TILE_CORRECTNESS tile_width=%" PRIu32
                    " source_crc_before=0x%08" PRIx32
                    " source_crc_after=0x%08" PRIx32
                    " output_crc=0x%08" PRIx32
                    " source_immutable=%d byte_exact=%d final_validation=%u"
                    " samples=%u result=%s\n",
                    tile_width, source_crc_before, source_crc_after, output_crc,
                    source_immutable ? 1 : 0, byte_exact ? 1 : 0,
                    1U, static_cast<unsigned>(kMeasuredSamples),
                    (source_immutable && byte_exact) ? "PASS" : "FAIL");
        const bool phase_ok = source_immutable && byte_exact;
        std::printf("P4_NANO_PPA_TILE_RESULT tile_width=%" PRIu32
                    " result=%s\n", tile_width, phase_ok ? "PASS" : "FAIL");
        all_ok = all_ok && phase_ok;
        if (!all_ok) {
            break;
        }
    }

    const esp_err_t unregister_result = ppa_unregister_client(client);
    heap_caps_free(tile_a);
    heap_caps_free(tile_b);
    heap_caps_free(verification);
    all_ok = all_ok && unregister_result == ESP_OK;
    std::printf("P4_NANO_PPA_INTERNAL_TILE_RESULT=%s\n",
                all_ok ? "PASS" : "FAIL");
    return all_ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_ppa_internal_tile
