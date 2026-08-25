/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_ppa_rotation.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "np2video_golden.h"

namespace {

constexpr std::size_t kSampleCount =
    p4_nano_ppa_rotation::kMeasuredSamples;
constexpr std::size_t kAlignment =
    p4_nano_ppa_rotation::kRequiredAlignmentBytes;

struct TimingStats final {
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0U;
    std::uint64_t total = 0U;
    std::size_t count = 0U;
};

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

void add_sample(TimingStats *stats, std::uint64_t sample) noexcept
{
    if (stats == nullptr) {
        return;
    }
    stats->minimum = std::min(stats->minimum, sample);
    stats->maximum = std::max(stats->maximum, sample);
    stats->total += sample;
    ++stats->count;
}

std::uint64_t percentile(const std::array<std::uint64_t, kSampleCount> &input,
                         std::size_t percentile_rank)
{
    auto sorted = input;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index =
        (percentile_rank * (sorted.size() - 1U) + 50U) / 100U;
    return sorted[index];
}

void report_memory(const char *phase)
{
    std::printf("P4_NANO_PPA_ROTATION_MEMORY phase=%s free_spiram=%" PRIu32
                " largest_spiram=%" PRIu32 "\n",
                phase,
                static_cast<std::uint32_t>(heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM)),
                static_cast<std::uint32_t>(heap_caps_get_largest_free_block(
                    MALLOC_CAP_SPIRAM)));
}

void print_failure(const char *reason)
{
    std::printf("P4_NANO_PPA_ROTATION_RESULT=FAIL reason=%s\n", reason);
}

} // namespace

namespace p4_nano_ppa_rotation {

esp_err_t run(const Input &input)
{
    const bool source_geometry =
        input.source != nullptr && input.source_bytes == kSourceBytes &&
        input.source_width == kSourceWidth &&
        input.source_height == kSourceHeight &&
        input.source_pitch_bytes == kSourcePitchBytes && input.source_bpp == 16U;
    const bool source_external =
        input.source != nullptr && esp_ptr_external_ram(input.source);
    const bool excluded_buffers_present =
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes != 0U && input.native_framebuffer != nullptr &&
        input.native_framebuffer_bytes != 0U;
    /* The held source is intentionally one of the presentation slots; only
     * the native framebuffer must be excluded from the read-only input. */
    const bool source_disjoint =
        source_external && excluded_buffers_present &&
        !ranges_overlap(input.source, input.source_bytes,
                        input.native_framebuffer,
                        input.native_framebuffer_bytes);
    const std::uint32_t source_crc_before = source_external
        ? p4_nano_display::crc32(input.source, input.source_bytes)
        : 0U;

    std::printf("P4_NANO_PPA_ROTATION_CONFIG source=%p source_geometry=%" PRIu32
                "x%" PRIu32 " output_geometry=%" PRIu32 "x%" PRIu32
                " source_bytes=%zu output_bytes=%zu "
                "source_external=%d source_mod64=%zu expected_source_crc=0x%08"
                PRIx32 " source_crc=0x%08" PRIx32
                " expected_crc=0x%08" PRIx32
                " burst_bytes=128 blocking=1\n",
                static_cast<const void *>(input.source), kSourceWidth,
                kSourceHeight, kOutputWidth, kOutputHeight, kSourceBytes,
                kOutputBytes, source_external ? 1 : 0,
                source_external
                    ? reinterpret_cast<std::uintptr_t>(input.source) % 64U
                    : 0U,
                kExpectedSourceCrc, source_crc_before, kExpectedOutputCrc);
    report_memory("before_output");

    if (!source_geometry || !source_external ||
        source_crc_before != kExpectedSourceCrc || !source_disjoint) {
        print_failure(!source_geometry ? "source_geometry" :
                      !source_external ? "source_not_external" :
                      source_crc_before != kExpectedSourceCrc ? "source_crc" :
                      "source_alias");
        return ESP_ERR_INVALID_STATE;
    }

    auto *output = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(
        kAlignment, kOutputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (output == nullptr) {
        print_failure("output_alloc");
        return ESP_ERR_NO_MEM;
    }
    const bool output_external = esp_ptr_external_ram(output);
    const bool output_aligned =
        reinterpret_cast<std::uintptr_t>(output) % kAlignment == 0U;
    const bool output_size_aligned = kOutputBytes % kAlignment == 0U;
    const bool output_disjoint =
        output_external && excluded_buffers_present &&
        !ranges_overlap(output, kOutputBytes, input.source, input.source_bytes) &&
        !ranges_overlap(output, kOutputBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(output, kOutputBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(output, kOutputBytes, input.native_framebuffer,
                        input.native_framebuffer_bytes);
    std::printf("P4_NANO_PPA_ROTATION_OUTPUT ptr=%p external=%d mod64=%zu "
                "bytes=%zu size_mod64=%zu disjoint=%d\n",
                static_cast<void *>(output), output_external ? 1 : 0,
                reinterpret_cast<std::uintptr_t>(output) % kAlignment,
                kOutputBytes, kOutputBytes % kAlignment,
                output_disjoint ? 1 : 0);
    report_memory("after_output");
    if (!output_external || !output_aligned || !output_size_aligned ||
        !output_disjoint) {
        heap_caps_free(output);
        print_failure(!output_external ? "output_not_external" :
                      !output_aligned ? "output_alignment" :
                      !output_size_aligned ? "output_size_alignment" :
                      "output_alias");
        return ESP_ERR_INVALID_STATE;
    }

    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    esp_err_t result = ppa_register_client(&client_config, &client);
    if (result != ESP_OK || client == nullptr) {
        heap_caps_free(output);
        print_failure("ppa_register");
        return result == ESP_OK ? ESP_FAIL : result;
    }

    const ppa_srm_oper_config_t operation{
        .in = {
            .buffer = input.source,
            .pic_w = kSourceWidth,
            .pic_h = kSourceHeight,
            .block_w = kSourceWidth,
            .block_h = kSourceHeight,
            .block_offset_x = 0U,
            .block_offset_y = 0U,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = output,
            .buffer_size = static_cast<std::uint32_t>(kOutputBytes),
            .pic_w = kOutputWidth,
            .pic_h = kOutputHeight,
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

    auto invoke = [&]() { return ppa_do_scale_rotate_mirror(client, &operation); };
    bool all_operations_succeeded = true;
    for (std::uint32_t index = 0U; index < kWarmupSamples; ++index) {
        if (invoke() != ESP_OK) {
            all_operations_succeeded = false;
            break;
        }
    }

    std::array<std::uint64_t, kSampleCount> samples{};
    TimingStats stats{};
    if (all_operations_succeeded) {
        for (std::size_t index = 0U; index < kSampleCount; ++index) {
            const std::int64_t start = esp_timer_get_time();
            const esp_err_t operation_result = invoke();
            const std::int64_t end = esp_timer_get_time();
            if (operation_result != ESP_OK || end <= start) {
                all_operations_succeeded = false;
                break;
            }
            samples[index] = static_cast<std::uint64_t>(end - start);
            add_sample(&stats, samples[index]);
        }
    }

    bool final_operation_succeeded = false;
    if (all_operations_succeeded) {
        final_operation_succeeded = invoke() == ESP_OK;
    }

    const std::uint32_t source_crc_after =
        p4_nano_display::crc32(input.source, input.source_bytes);
    const std::uint32_t output_crc =
        p4_nano_display::crc32(reinterpret_cast<const std::uint8_t *>(output),
                                kOutputBytes);
    const bool source_immutable = source_crc_before == source_crc_after;
    const bool byte_exact =
        final_operation_succeeded && reference_matches(
            reinterpret_cast<const std::uint16_t *>(input.source), output);
    const bool crc_exact = output_crc == kExpectedOutputCrc;
    const bool timing_valid = stats.count == kSampleCount;
    const esp_err_t unregister_result = ppa_unregister_client(client);
    heap_caps_free(output);
    report_memory("after_output_free");

    const double logical_payload_mib_s =
        timing_valid && stats.total != 0U
            ? (static_cast<double>(kSourceBytes + kOutputBytes) * 1'000'000.0 /
               static_cast<double>(stats.total / kSampleCount)) /
                  (1024.0 * 1024.0)
            : 0.0;
    std::printf("P4_NANO_PPA_ROTATION_TIMING count=%zu "
                "ppa_rotation_blocking_wall_us=%" PRIu64
                " min_us=%" PRIu64
                " average_us=%" PRIu64 " p50_us=%" PRIu64
                " p95_us=%" PRIu64 " p99_us=%" PRIu64
                " max_us=%" PRIu64 " logical_payload_mib_s=%.3f\n",
                stats.count, stats.count == 0U ? 0U : stats.total / stats.count,
                stats.count == 0U ? 0U : stats.minimum,
                stats.count == 0U ? 0U : stats.total / stats.count,
                percentile(samples, 50U), percentile(samples, 95U),
                percentile(samples, 99U), stats.count == 0U ? 0U : stats.maximum,
                logical_payload_mib_s);
    std::printf("P4_NANO_PPA_ROTATION_CORRECTNESS source_crc_before=0x%08"
                PRIx32 " source_crc_after=0x%08" PRIx32
                " output_crc=0x%08" PRIx32 " expected_crc=0x%08" PRIx32
                " byte_exact=%d source_immutable=%d final_operation=%d\n",
                source_crc_before, source_crc_after, output_crc,
                kExpectedOutputCrc, byte_exact ? 1 : 0,
                source_immutable ? 1 : 0, final_operation_succeeded ? 1 : 0);
    std::printf("P4_NANO_PPA_ROTATION_SAMPLE_COUNTS warmup=%u measured=%u "
                "final_validation=%u\n",
                static_cast<unsigned>(kWarmupSamples),
                static_cast<unsigned>(kMeasuredSamples),
                static_cast<unsigned>(kFinalValidationSamples));

    const bool pass = all_operations_succeeded && final_operation_succeeded &&
                      timing_valid && source_immutable && byte_exact &&
                      crc_exact && unregister_result == ESP_OK;
    std::printf("P4_NANO_PPA_ROTATION_RESULT=%s\n", pass ? "PASS" : "FAIL");
    return pass ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_ppa_rotation
