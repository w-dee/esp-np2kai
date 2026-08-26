/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_live_display.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include "sdkconfig.h"

#include "esp_chip_info.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <compiler.h>
#include "np2_presentation.h"
#include "np2video_runner/np2video_runner.h"
#include "np2video_motion_oracle.h"
#include <taskmng.h>
#include "p4_nano_board/p4_nano_board.hpp"
#include "p4_nano_display/p4_nano_display.hpp"
#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_display/p4_nano_display_transform.hpp"
#if defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
#include "p4_nano_display/p4_nano_display_exact2x.hpp"
#endif
#include "p4_nano_live_display/p4_nano_live_display_contract.hpp"
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_compute_control.hpp"
#endif
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_psram_read_control.hpp"
#endif
#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
#define P4_NANO_OVERLAP_TRACE_ACTIVE 1
#include "p4_nano_live_display/p4_nano_overlap_trace.hpp"
#endif
#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_psram_bandwidth.hpp"
#endif
#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_ppa_rotation.hpp"
#endif
#if defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_ppa_internal_tile.hpp"
#endif
#if defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE)
#include "driver/ppa.h"
#include "p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"
#endif
#if defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_ppa_pie_overlap.hpp"
#endif
#include "p4_nano_live_display_session/session.hpp"
#include "scrnmng.h"

#include "np2video_golden.h"

namespace {

constexpr std::size_t kSlotBytes =
    p4_nano_live_display::kPresentationSlotBytes;
constexpr std::size_t kSlotCount =
    p4_nano_live_display::kPresentationSlotCount;
/* CONFIG_FREERTOS_HZ may be 100, where pdMS_TO_TICKS(1) is zero.  The
 * no-frame path must block for at least one scheduler tick so CPU0 IDLE0 can
 * run; this is provisional benchmark liveness policy, not final presentation
 * pacing. */
constexpr TickType_t kConsumerPollDelayTicks = 1;
static_assert(kConsumerPollDelayTicks > 0);
#if defined(CONFIG_FREERTOS_HZ) && CONFIG_FREERTOS_HZ == 100
static_assert(pdMS_TO_TICKS(1) == 0,
              "one millisecond is zero ticks at the benchmark's 100 Hz rate");
#endif
constexpr std::uint64_t kVisibleHoldUs = 30ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kProducerWatchdogUs = 120ULL * 1000ULL * 1000ULL;

struct LiveState {
    p4_nano_live_display_session::Session session{};
    np2video_runner_result producer_result{};
    std::atomic<bool> producer_done{false};
    bool immutable_checked = false;
    bool immutable_pass = false;
    std::uint32_t final_source_generation = 0;
    std::uint32_t final_source_update_sequence = 0;
    std::uint64_t final_published_sequence = 0;
    std::uint32_t final_source_crc = 0;
    std::uint32_t final_native_crc = 0;
    std::uint32_t timeout_reported = 0;
    std::uint32_t current_source_crc = 0;
    std::int64_t transform_start_us = 0;
    std::uint64_t min_transform_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_transform_us = 0;
    std::uint64_t total_transform_us = 0;
};

void report_memory(const char *phase)
{
    std::printf("P4_NANO_LIVE_MEMORY phase=%s psram_total=%" PRIu32
                " free_spiram=%" PRIu32 " largest_spiram=%" PRIu32 "\n",
                phase,
                static_cast<std::uint32_t>(esp_psram_get_size()),
                static_cast<std::uint32_t>(heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM)),
                static_cast<std::uint32_t>(heap_caps_get_largest_free_block(
                    MALLOC_CAP_SPIRAM)));
}

bool ranges_overlap(const void *first, std::size_t first_size,
                    const void *second, std::size_t second_size)
{
    const auto first_start = reinterpret_cast<std::uintptr_t>(first);
    const auto second_start = reinterpret_cast<std::uintptr_t>(second);
    return first_start < second_start + second_size &&
           second_start < first_start + first_size;
}

bool runner_output(void *, const char *data, std::size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }
    const bool complete = std::fwrite(data, 1, length, stdout) == length;
    std::fflush(stdout);
    return complete;
}

bool runner_ready(void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state == nullptr ||
        state->session.attach_source() != ESP_OK) {
        return false;
    }
    std::printf("P4_NANO_LIVE_PUBLISHER_READY slots=%zu slot_bytes=%zu\n",
                kSlotCount, kSlotBytes);
    return true;
}

void runner_stopping(void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state != nullptr) {
        state->session.detach_source();
    }
}

void runner_complete(const np2video_runner_result *result, void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state == nullptr || result == nullptr) {
        return;
    }
    state->producer_result = *result;
    state->producer_done.store(true, std::memory_order_release);
}

bool validate_frame(const np2_presentation_frame_view &view)
{
    return view.ptr != nullptr && esp_ptr_external_ram(view.ptr) &&
           view.width == np2video_golden_width &&
           view.height == np2video_golden_height &&
           view.bpp == np2video_golden_bpp &&
           view.pixel_format == NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE &&
           view.pitch == np2video_golden_pitch &&
           view.width == p4_nano_live_display::kSourceWidth &&
           view.height == p4_nano_live_display::kSourceHeight &&
           view.pitch == p4_nano_live_display::kSourceWidth *
                             sizeof(std::uint16_t);
}

bool normal_frame_observer(
    const p4_nano_live_display_session::FrameObservation &observation,
    void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state == nullptr || observation.source.ptr == nullptr) {
        return false;
    }
    if (!esp_ptr_external_ram(observation.source.ptr)) {
        return false;
    }
    if (observation.stage ==
        p4_nano_live_display_session::FrameStage::Acquired) {
        state->transform_start_us = esp_timer_get_time();
        state->current_source_crc = p4_nano_display::crc32(
            observation.source.ptr,
            p4_nano_live_display_session::kSourceBytes);
        return true;
    }

    const std::uint32_t after_crc = p4_nano_display::crc32(
        observation.source.ptr, p4_nano_live_display_session::kSourceBytes);
    const std::uint64_t transform_us = static_cast<std::uint64_t>(
        esp_timer_get_time() - state->transform_start_us);
    if (transform_us < state->min_transform_us) {
        state->min_transform_us = transform_us;
    }
    if (transform_us > state->max_transform_us) {
        state->max_transform_us = transform_us;
    }
    state->total_transform_us += transform_us;
    if (!state->immutable_checked) {
        state->immutable_checked = true;
        state->immutable_pass = state->current_source_crc == after_crc;
        std::printf("P4_NANO_LIVE_FRAME_IMMUTABLE=%s before=0x%08" PRIx32
                    " after=0x%08" PRIx32 "\n",
                    state->immutable_pass ? "PASS" : "FAIL",
                    state->current_source_crc, after_crc);
    }
    if (!state->immutable_pass || observation.native_framebuffer == nullptr) {
        return false;
    }
    state->final_source_generation = observation.source.source_generation;
    state->final_source_update_sequence =
        observation.source.source_update_sequence;
    state->final_published_sequence = observation.source.published_sequence;
    state->final_source_crc = state->current_source_crc;
    state->final_native_crc = p4_nano_display::crc32(
        reinterpret_cast<const std::uint8_t *>(observation.native_framebuffer),
        observation.native_framebuffer_bytes);
    return true;
}

#if defined(P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE)

constexpr std::uint32_t kMotionMaximumAcquisitions = 64U;
constexpr std::uint32_t kMotionDistinctTarget = 16U;
constexpr std::uint64_t kMotionWatchdogUs = 120ULL * 1000ULL * 1000ULL;
constexpr int kMotionProducerCore = 1;
constexpr std::uint32_t kMotionProducerPriority =
    static_cast<std::uint32_t>(tskIDLE_PRIORITY + 3);

struct MotionState {
    np2_presentation_publisher publisher{};
    np2_presentation_slot_storage slots[kSlotCount]{};
    p4_nano_display::DisplaySession display{};
    np2video_runner_result producer_result{};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> publish_failed{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> scene_ready{false};
    std::uint32_t scene_generation = 0U;
    std::uint32_t scene_update_sequence = 0U;
    std::uint32_t acquired = 0U;
    std::uint32_t clean = 0U;
    std::uint32_t distinct = 0U;
    std::uint32_t repeated = 0U;
    std::uint32_t transitional = 0U;
    std::uint32_t invalid_position = 0U;
    std::uint32_t native_pass = 0U;
    std::uint32_t native_fail = 0U;
    std::uint32_t submitted = 0U;
    std::uint32_t released = 0U;
    std::uint64_t last_published_sequence = 0U;
    std::uint64_t first_published_sequence = 0U;
    std::uint64_t final_published_sequence = 0U;
    std::uint32_t first_source_update_sequence = 0U;
    std::uint32_t final_source_update_sequence = 0U;
    std::uint32_t last_source_update_sequence = 0U;
    bool source_sequence_initialized = false;
    bool generation_initialized = false;
    bool immutable_pass = true;
    bool visible = false;
    bool hook_registered = false;
    bool slots_initialized = false;
    bool failed = false;
    const char *failure_reason = nullptr;
    bool positions[NP2VIDEO_MOTION_BAR_MAX_POS -
                   NP2VIDEO_MOTION_BAR_MIN_POS + 1U]{};
};

bool motion_runner_output(void *, const char *data, std::size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }
    const bool complete = std::fwrite(data, 1, length, stdout) == length;
    std::fflush(stdout);
    return complete;
}

void motion_publish_hook(const SCRNMNG_PUBLISH_VIEW *view, void *context)
{
    auto *state = static_cast<MotionState *>(context);
    if (state == nullptr || view == nullptr) {
        return;
    }
    const np2_presentation_source_view source{
        .ptr = view->ptr,
        .width = static_cast<std::uint32_t>(view->width),
        .height = static_cast<std::uint32_t>(view->height),
        .pitch = view->pitch,
        .bpp = view->bpp,
        .pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE,
        .source_generation = view->surface_generation,
        .source_update_sequence = view->surface_update_sequence,
    };
    const np2_presentation_status status = np2_presentation_submit(
        &state->publisher, &source);
    if (status == NP2_PRESENTATION_OK) {
        ++state->submitted;
    } else if (status != NP2_PRESENTATION_DROPPED) {
        state->publish_failed.store(true, std::memory_order_release);
        state->failure_reason = "PUBLISHER_ERROR";
    }
}

bool motion_runner_ready(void *context)
{
    auto *state = static_cast<MotionState *>(context);
    if (state == nullptr || !state->slots_initialized) {
        return false;
    }
    scrnmng_set_publish_hook(motion_publish_hook, state);
    state->hook_registered = true;
    std::printf("MOTION_PUBLISHER_READY slots=%zu slot_bytes=%zu\n",
                kSlotCount, kSlotBytes);
    return true;
}

void motion_scene_ready(std::uint32_t generation,
                        std::uint32_t update_sequence, void *context)
{
    auto *state = static_cast<MotionState *>(context);
    if (state == nullptr) {
        return;
    }
    state->scene_generation = generation;
    state->scene_update_sequence = update_sequence;
    state->scene_ready.store(true, std::memory_order_release);
}

void motion_runner_stopping(void *context)
{
    auto *state = static_cast<MotionState *>(context);
    if (state != nullptr && state->hook_registered) {
        scrnmng_set_publish_hook(nullptr, nullptr);
        state->hook_registered = false;
    }
}

void motion_runner_complete(const np2video_runner_result *result, void *context)
{
    auto *state = static_cast<MotionState *>(context);
    if (state != nullptr && result != nullptr) {
        state->producer_result = *result;
        state->producer_done.store(true, std::memory_order_release);
    }
}

bool motion_stop_requested(void *context)
{
    auto *state = static_cast<MotionState *>(context);
    return state != nullptr && state->stop_requested.load(
        std::memory_order_acquire);
}

void motion_release(MotionState *state, np2_presentation_token *token)
{
    if (state == nullptr || token == nullptr || token->lease == 0U) {
        return;
    }
    if (np2_presentation_release(&state->publisher, token) ==
        NP2_PRESENTATION_OK) {
        ++state->released;
    } else {
        state->publish_failed.store(true, std::memory_order_release);
        state->failure_reason = "LEASE_ERROR";
    }
}

bool motion_guest_same(const np2video_motion_guest_sample &before,
                       const np2video_motion_guest_sample &after)
{
    return before.status == NP2VIDEO_MOTION_GUEST_VALID &&
           after.status == NP2VIDEO_MOTION_GUEST_VALID &&
           before.bar_pos == after.bar_pos &&
           before.x_start == after.x_start && before.x_end == after.x_end &&
           before.bar_color == after.bar_color;
}

int motion_consume_one(MotionState *state)
{
    np2_presentation_frame_view view{};
    np2_presentation_token token{};
    np2video_motion_guest_sample guest_before{};
    np2video_motion_guest_sample guest_after{};
    np2video_motion_native_sample native_sample{};
    const np2_presentation_status acquire = np2_presentation_acquire(
        &state->publisher, &view, &token);
    if (acquire == NP2_PRESENTATION_NO_FRAME) {
        return 0;
    }
    if (acquire != NP2_PRESENTATION_OK) {
        state->failure_reason = "PRESENTATION_SEQUENCE_FROZEN";
        state->failed = true;
        state->publish_failed.store(true, std::memory_order_release);
        return -1;
    }
    ++state->acquired;
    if (state->acquired > kMotionMaximumAcquisitions) {
        state->failure_reason = "INSUFFICIENT_CLEAN_MOTION_SAMPLES";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (!validate_frame(view)) {
        state->failure_reason = "INVALID_GEOMETRY";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (!state->generation_initialized) {
        state->scene_generation = view.source_generation;
        state->generation_initialized = true;
    } else if (view.source_generation != state->scene_generation) {
        state->failure_reason = "STALE_GENERATION";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (view.source_update_sequence <= state->scene_update_sequence ||
        (state->source_sequence_initialized &&
         view.source_update_sequence <= state->last_source_update_sequence) ||
        (state->last_published_sequence != 0U &&
         view.published_sequence <= state->last_published_sequence)) {
        state->failure_reason = "PRESENTATION_SEQUENCE_FROZEN";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (state->first_published_sequence == 0U) {
        state->first_published_sequence = view.published_sequence;
        state->first_source_update_sequence = view.source_update_sequence;
    }
    state->last_published_sequence = view.published_sequence;
    state->last_source_update_sequence = view.source_update_sequence;
    state->source_sequence_initialized = true;

    const bool guest_clean = np2video_motion_guest_detect(
        view.ptr, view.pitch, &guest_before);
    if (!guest_clean) {
        ++state->transitional;
        switch (guest_before.status) {
            case NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS:
            case NP2VIDEO_MOTION_GUEST_WRONG_WIDTH:
            case NP2VIDEO_MOTION_GUEST_INVALID_ALIGNMENT:
            case NP2VIDEO_MOTION_GUEST_ROW_MISMATCH:
            case NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY:
                ++state->invalid_position;
                break;
            default:
                break;
        }
    }
    const auto source = std::span<const std::uint16_t>(
        reinterpret_cast<const std::uint16_t *>(view.ptr),
        p4_nano_display::kTransformSourcePixelCount);
    const auto destination = std::span<std::uint16_t>(
        state->display.framebuffer,
        p4_nano_display::kTransformDestinationPixelCount);
    if (!p4_nano_display::transform_to_native(
            source, destination,
            p4_nano_display::QuarterTurn::CounterClockwise)) {
        state->failure_reason = "TRANSFORM_ERROR";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (p4_nano_display::display_session_sync_framebuffer(&state->display) !=
        ESP_OK) {
        state->failure_reason = "CACHE_SYNC_ERROR";
        state->failed = true;
        motion_release(state, &token);
        return -1;
    }
    if (!state->visible) {
        if (p4_nano_board::display_backlight_set(
                p4_nano_board::kBacklightConservative) != ESP_OK) {
            state->failure_reason = "BACKLIGHT_ERROR";
            state->failed = true;
            motion_release(state, &token);
            return -1;
        }
        state->visible = true;
    }
    if (guest_clean) {
        if (!np2video_motion_native_detect(
                state->display.framebuffer, p4_nano_display::kNativeWidth,
                &guest_before, &native_sample)) {
            ++state->native_fail;
            state->failure_reason =
                native_sample.status == NP2VIDEO_MOTION_NATIVE_NO_BAR
                    ? "NATIVE_CONTENT_FROZEN" : "NATIVE_MAPPING_MISMATCH";
            state->failed = true;
        } else {
            ++state->native_pass;
        }
    }
    const bool guest_clean_after = np2video_motion_guest_detect(
        view.ptr, view.pitch, &guest_after);
    if (guest_clean && guest_clean_after &&
        !motion_guest_same(guest_before, guest_after)) {
        state->immutable_pass = false;
        state->failure_reason = "IMMUTABLE_SOURCE_CHANGED";
        state->failed = true;
    }
    if (guest_clean && !guest_clean_after) {
        ++state->transitional;
    }
    if (guest_clean && guest_clean_after && !state->failed) {
        ++state->clean;
        const std::size_t position_index =
            guest_before.bar_pos - NP2VIDEO_MOTION_BAR_MIN_POS;
        if (state->positions[position_index]) {
            ++state->repeated;
        } else {
            state->positions[position_index] = true;
            ++state->distinct;
            if (state->distinct <= kMotionDistinctTarget) {
                std::uint32_t native_y_start = 0U;
                std::uint32_t native_y_end = 0U;
                (void)np2video_motion_expected_native_band(
                    guest_before.bar_pos, &native_y_start, &native_y_end);
                std::printf(
                    "MOTION_SAMPLE index=%" PRIu32
                    " published_sequence=%" PRIu64
                    " source_update_sequence=%" PRIu32
                    " guest_bar_pos=%" PRIu32 " guest_x_start=%" PRIu32
                    " bar_color=0x%04" PRIx16
                    " expected_native_y_min=%" PRIu32
                    " expected_native_y_max=%" PRIu32
                    " native_result=PASS\n",
                    state->distinct, view.published_sequence,
                    view.source_update_sequence, guest_before.bar_pos,
                    guest_before.x_start, guest_before.bar_color,
                    native_y_start, native_y_end);
            }
        }
    }
    state->final_published_sequence = view.published_sequence;
    state->final_source_update_sequence = view.source_update_sequence;
    motion_release(state, &token);
    np2_host_taskmng_cooperate();
    if (state->failed) {
        return -1;
    }
    if (state->distinct >= kMotionDistinctTarget &&
        state->native_pass >= kMotionDistinctTarget) {
        state->stop_requested.store(true, std::memory_order_release);
    } else if (state->acquired >= kMotionMaximumAcquisitions) {
        state->failure_reason = "INSUFFICIENT_CLEAN_MOTION_SAMPLES";
        state->failed = true;
        state->stop_requested.store(true, std::memory_order_release);
        return -1;
    }
    return 1;
}

esp_err_t run_motion_validation()
{
    MotionState state;
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    std::printf("P4_NANO_LIVE_MOTION_VALIDATION rotation=CCW chip_revision=%d "
                "max_acquired=%" PRIu32 " distinct_target=%" PRIu32
                " profiler=OFF pause=OFF\n",
                chip_info.revision, kMotionMaximumAcquisitions,
                kMotionDistinctTarget);
    report_memory("before_motion_slots");
    for (std::size_t index = 0; index < kSlotCount; ++index) {
        state.slots[index].ptr = static_cast<std::uint8_t *>(heap_caps_calloc(
            1, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        state.slots[index].capacity = kSlotBytes;
        if (state.slots[index].ptr == nullptr) {
            for (std::size_t release = 0; release < index; ++release) {
                heap_caps_free(state.slots[release].ptr);
            }
            std::printf("MOTION_VALIDATION_RESULT=FAIL reason=slot_alloc\n");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!esp_ptr_external_ram(state.slots[0].ptr) ||
        !esp_ptr_external_ram(state.slots[1].ptr) ||
        ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.slots[1].ptr, kSlotBytes)) {
        std::printf("MOTION_VALIDATION_RESULT=FAIL reason=slot_layout\n");
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_INVALID_STATE;
    }
    if (np2_presentation_init(&state.publisher, state.slots) !=
        NP2_PRESENTATION_OK) {
        std::printf("MOTION_VALIDATION_RESULT=FAIL reason=publisher_init\n");
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_INVALID_STATE;
    }
    state.slots_initialized = true;
    esp_err_t result = p4_nano_display::display_session_initialize(
        &state.display);
    if (result != ESP_OK) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    if (!esp_ptr_external_ram(state.display.framebuffer)) {
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("MOTION_VALIDATION_RESULT=FAIL reason=destination_not_external\n");
        return ESP_ERR_INVALID_STATE;
    }
    (void)p4_nano_board::display_backlight_set(0U);
    const np2video_runner_config runner_config{
        .output = motion_runner_output,
        .output_context = nullptr,
        .ready = motion_runner_ready,
        .scene_ready = motion_scene_ready,
        .stopping = motion_runner_stopping,
        .complete = motion_runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
        .stop_requested = motion_stop_requested,
        .cooperate = nullptr,
        .pause_at_cooperate = nullptr,
        .task_scheduling_override = true,
        .task_core_id = kMotionProducerCore,
        .task_priority = kMotionProducerPriority,
    };
    result = np2video_runner_start_ex(&runner_config);
    if (result != ESP_OK) {
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("MOTION_VALIDATION_RESULT=FAIL reason=producer_start\n");
        return result;
    }
    const std::uint64_t start_us = static_cast<std::uint64_t>(esp_timer_get_time());
    while (!state.producer_done.load(std::memory_order_acquire) &&
           !state.stop_requested.load(std::memory_order_acquire)) {
        const int consumed = motion_consume_one(&state);
        if (consumed < 0) {
            state.stop_requested.store(true, std::memory_order_release);
        } else if (consumed == 0) {
            vTaskDelay(kConsumerPollDelayTicks);
        }
        if (static_cast<std::uint64_t>(esp_timer_get_time()) - start_us >
            kMotionWatchdogUs) {
            state.failure_reason = "MOTION_VALIDATION_TIMEOUT";
            state.failed = true;
            state.stop_requested.store(true, std::memory_order_release);
            break;
        }
    }
    state.stop_requested.store(true, std::memory_order_release);
    while (!state.producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
        if (static_cast<std::uint64_t>(esp_timer_get_time()) - start_us >
            kMotionWatchdogUs * 2U) {
            state.failure_reason = "PRODUCER_STOP_TIMEOUT";
            state.failed = true;
            break;
        }
    }
    for (;;) {
        np2_presentation_frame_view pending_view{};
        np2_presentation_token pending_token{};
        if (np2_presentation_acquire(&state.publisher, &pending_view,
                                     &pending_token) != NP2_PRESENTATION_OK) {
            break;
        }
        motion_release(&state, &pending_token);
    }
    if (state.producer_result.status != ESP_OK ||
        state.publish_failed.load(std::memory_order_acquire)) {
        state.failed = true;
        if (state.failure_reason == nullptr) {
            state.failure_reason = "PUBLISHER_ERROR";
        }
    }
    if (!state.failed && state.distinct < kMotionDistinctTarget) {
        if (state.invalid_position > 0U && state.clean == 0U) {
            state.failure_reason = "PRESENTATION_POSITION_INVALID";
        } else if (state.acquired == kMotionMaximumAcquisitions &&
            state.transitional > 0U) {
            state.failure_reason = "INSUFFICIENT_CLEAN_MOTION_SAMPLES";
        } else if (state.clean > 0U && state.distinct <= 1U) {
            state.failure_reason = "GUEST_MOTION_FROZEN";
        } else {
            state.failure_reason = "PRESENTATION_CONTENT_FROZEN";
        }
        state.failed = true;
    }
    if (p4_nano_board::display_backlight_set(0U) != ESP_OK) {
        state.failed = true;
        state.failure_reason = "BACKLIGHT_ERROR";
    }
    const std::uint32_t coalesced =
        np2_presentation_coalesced_count(&state.publisher);
    const std::uint32_t dropped =
        np2_presentation_dropped_count(&state.publisher);
    std::printf(
        "MOTION_VALIDATION_SUMMARY acquired=%" PRIu32 " clean=%" PRIu32
        " distinct=%" PRIu32 " repeated=%" PRIu32
        " transitional=%" PRIu32 " invalid_position=%" PRIu32
        " native_pass=%" PRIu32
        " native_fail=%" PRIu32 " submitted=%" PRIu32
        " released=%" PRIu32 " coalesced=%" PRIu32 " dropped=%" PRIu32
        " first_published_sequence=%" PRIu64
        " last_published_sequence=%" PRIu64
        " first_source_update_sequence=%" PRIu32
        " last_source_update_sequence=%" PRIu32 " result=%s reason=%s\n",
        state.acquired, state.clean, state.distinct, state.repeated,
        state.transitional, state.invalid_position, state.native_pass,
        state.native_fail, state.submitted, state.released, coalesced, dropped,
        state.first_published_sequence, state.final_published_sequence,
        state.first_source_update_sequence, state.final_source_update_sequence,
        state.failed ? "FAIL" : "PASS",
        state.failed ? (state.failure_reason == nullptr ? "UNKNOWN" :
                        state.failure_reason) : "NONE");
    std::printf("MOTION_VALIDATION_RESULT=%s%s%s\n",
                state.failed ? "FAIL reason=" : "PASS",
                state.failed && state.failure_reason != nullptr
                    ? state.failure_reason : "",
                state.failed ? "" : "");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state.display);
    heap_caps_free(state.slots[0].ptr);
    heap_caps_free(state.slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return state.failed ? ESP_FAIL : ESP_OK;
}

#endif

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)

constexpr std::uint32_t kBenchmarkWarmupTransforms = 8U;
constexpr std::uint32_t kBenchmarkMeasuredTransforms = 128U;
constexpr std::uint32_t kBenchmarkFinalValidationTransforms = 1U;
constexpr std::uint32_t kBenchmarkTotalTransforms =
    kBenchmarkWarmupTransforms + kBenchmarkMeasuredTransforms +
    kBenchmarkFinalValidationTransforms;
constexpr std::size_t kBenchmarkSubmitSampleCapacity = 8192U;
constexpr std::size_t kBenchmarkLatencySampleCapacity = 256U;
constexpr std::size_t kBenchmarkTimestampRingSize = 1024U;
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
/* P2 LIVE recorded 151 submit intervals for this 8+128+1 fixture.  Keep a
 * separate trace bound with >3x headroom rather than duplicating the much
 * larger percentile-sample array; overflow remains a benchmark failure. */
constexpr std::size_t kBenchmarkSubmitTraceCapacity = 512U;
constexpr std::size_t kBenchmarkTransformTraceCapacity =
    kBenchmarkTotalTransforms;
static_assert(kBenchmarkSubmitTraceCapacity >=
              kBenchmarkTotalTransforms * 3U);
static_assert(kBenchmarkTransformTraceCapacity == kBenchmarkTotalTransforms);
static_assert(NP2VIDEO_PCCORE_TRACE_CAPACITY >=
              kBenchmarkTotalTransforms * 3U);
static_assert(NP2_PCCORE_DRAW_TRACE_CAPACITY >=
              kBenchmarkTotalTransforms * 3U);
#endif
constexpr std::uint64_t kBenchmarkWatchdogUs = 120ULL * 1000ULL * 1000ULL;
constexpr int kBenchmarkProducerCore = 1;
/* Scheduler A/B candidate: the priority-0 baseline was deliberately
 * conservative before pccore_exec(TRUE) had a measured bound.  The physical
 * baseline measured a 266366 us maximum, well below the 5 s TWDT timeout, and
 * the producer still blocks for one RTOS tick after every returned call.
 * Priority 3 matches the existing default runner priority.  This remains a
 * controlled diagnostic candidate, not a final production policy. */
constexpr std::uint32_t kBenchmarkProducerPriority =
    static_cast<std::uint32_t>(tskIDLE_PRIORITY + 3);

constexpr bool benchmark_is_measured_sample(std::uint32_t transform_index)
{
    return transform_index >= kBenchmarkWarmupTransforms &&
           transform_index <
               kBenchmarkWarmupTransforms + kBenchmarkMeasuredTransforms;
}

constexpr std::size_t benchmark_measured_sample_count()
{
    std::size_t count = 0U;
    for (std::uint32_t index = 0U; index < kBenchmarkTotalTransforms; ++index) {
        if (benchmark_is_measured_sample(index)) {
            ++count;
        }
    }
    return count;
}

static_assert(benchmark_measured_sample_count() == kBenchmarkMeasuredTransforms);
static_assert(!benchmark_is_measured_sample(0U));
static_assert(benchmark_is_measured_sample(kBenchmarkWarmupTransforms));
static_assert(benchmark_is_measured_sample(
    kBenchmarkWarmupTransforms + kBenchmarkMeasuredTransforms - 1U));
static_assert(!benchmark_is_measured_sample(
    kBenchmarkWarmupTransforms + kBenchmarkMeasuredTransforms));
static_assert(!benchmark_is_measured_sample(kBenchmarkTotalTransforms - 1U));

struct BenchmarkTimestamp {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> submit_start_us{0};
};

#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
using BenchmarkOverlapAnalysis =
    p4_nano_overlap::Analysis<kBenchmarkMeasuredTransforms>;
using BenchmarkPccoreAnalysis =
    p4_nano_overlap::PccoreAnalysis<kBenchmarkMeasuredTransforms>;
using BenchmarkDrawAnalysis =
    p4_nano_overlap::DrawAnalysis<kBenchmarkMeasuredTransforms>;
using BenchmarkHierarchyAnalysis =
    p4_nano_overlap::HierarchyAnalysis<kBenchmarkMeasuredTransforms>;
using BenchmarkCpuNeventAnalysis =
    p4_nano_overlap::CpuNeventAnalysis<kBenchmarkMeasuredTransforms>;
#endif

struct BenchmarkState {
    np2_presentation_publisher publisher{};
    np2_presentation_slot_storage slots[kSlotCount]{};
    p4_nano_display::DisplaySession display{};
    np2video_runner_result producer_result{};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> scene_ready{false};
    std::atomic<std::uint32_t> scene_generation{0};
    std::atomic<std::uint32_t> scene_update_sequence{0};
    std::atomic<int> producer_core{-1};
    std::atomic<std::uint32_t> producer_priority{0};
    std::atomic<bool> producer_pause_requested{false};
    std::atomic<bool> producer_pause_acknowledged{false};
    std::atomic<std::uint32_t> producer_cooperate_calls{0};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> publish_failed{false};
    BenchmarkTimestamp timestamp_ring[kBenchmarkTimestampRingSize]{};
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
    /* CPU1 owns pccore/draw/submit trace writers; CPU0 owns transform trace.
     * benchmark_runner_complete publishes producer_done with release semantics,
     * and the consumer observes it with acquire before this analysis, so no
     * trace has a shared hot-path index, atomic, or lock. */
    std::array<p4_nano_overlap::SubmitInterval,
               kBenchmarkSubmitTraceCapacity>
        overlap_submit_trace{};
    std::array<p4_nano_overlap::TransformInterval,
               kBenchmarkTransformTraceCapacity>
        overlap_transform_trace{};
    np2video_pccore_trace pccore_trace{};
    np2_pccore_draw_trace draw_trace{};
    np2_pccore_cpu_nevent_trace cpu_nevent_trace{};
    std::size_t overlap_submit_trace_stored = 0U;
    std::size_t overlap_transform_trace_stored = 0U;
    bool overlap_submit_trace_overflow = false;
    bool overlap_transform_trace_overflow = false;
    bool overlap_trace_analyzed = false;
    BenchmarkOverlapAnalysis overlap_analysis{};
    BenchmarkPccoreAnalysis pccore_analysis{};
    BenchmarkDrawAnalysis draw_analysis{};
    BenchmarkHierarchyAnalysis hierarchy_analysis{};
    BenchmarkCpuNeventAnalysis cpu_nevent_analysis{};
    bool pccore_overlap_analyzed = false;
    bool draw_overlap_analyzed = false;
    bool cpu_nevent_overlap_analyzed = false;
#endif
    std::array<std::uint64_t, kBenchmarkSubmitSampleCapacity> submit_samples{};
    std::array<std::uint64_t, kBenchmarkLatencySampleCapacity> latency_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> transform_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> cache_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> service_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        isolated_transform_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        isolated_cache_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        isolated_service_samples{};
    std::size_t submit_stored = 0;
    std::size_t latency_stored = 0;
    std::size_t transform_stored = 0;
    std::size_t cache_stored = 0;
    std::size_t service_stored = 0;
    std::size_t isolated_transform_stored = 0;
    std::size_t isolated_cache_stored = 0;
    std::size_t isolated_service_stored = 0;
    std::uint64_t submit_count = 0;
    std::uint64_t submit_total_us = 0;
    std::uint64_t submit_min_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t submit_max_us = 0;
    std::uint64_t latency_count = 0;
    std::uint64_t latency_total_us = 0;
    std::uint64_t latency_min_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t latency_max_us = 0;
    std::uint32_t publish_hook_calls = 0;
    std::uint32_t last_surface_update_sequence = 0;
    std::uint32_t scene_ready_publish_hook_calls = 0;
    std::uint32_t scene_ready_submit_attempts = 0;
    std::uint32_t scene_ready_successful_submissions = 0;
    std::uint32_t scene_ready_submit_failures = 0;
    std::uint32_t scene_ready_coalesced = 0;
    std::uint32_t scene_ready_dropped = 0;
    std::uint32_t submit_attempts = 0;
    std::uint32_t successful_submissions = 0;
    std::uint32_t submit_failures = 0;
    std::uint32_t acquisitions = 0;
    std::uint32_t consumer_cooperate_calls = 0;
    std::uint32_t transforms_started = 0;
    std::uint32_t transforms_completed = 0;
    std::uint32_t cache_sync_success = 0;
    std::uint32_t cache_sync_failures = 0;
    std::uint32_t releases = 0;
    std::uint32_t native_framebuffer_updates = 0;
    std::uint32_t latency_valid = 0;
    std::uint32_t producer_latency_unavailable = 0;
    std::uint32_t consumer_latency_lookup_missing = 0;
    std::uint32_t correctness_pass = 0;
    std::uint32_t correctness_fail = 0;
    std::uint32_t first_source_crc = 0;
    std::uint32_t final_source_crc = 0;
    std::uint32_t first_native_crc = 0;
    std::uint32_t final_native_crc = 0;
    std::uint32_t isolated_transforms_started = 0;
    std::uint32_t isolated_transforms_completed = 0;
    std::uint32_t isolated_cache_sync_success = 0;
    std::uint32_t isolated_cache_sync_failures = 0;
    std::uint32_t isolated_consumer_cooperate_calls = 0;
    std::uint32_t isolated_correctness_pass = 0;
    std::uint32_t isolated_correctness_fail = 0;
    std::uint32_t isolated_source_crc = 0;
    std::uint32_t isolated_source_crc_after = 0;
    std::uint32_t isolated_first_native_crc = 0;
    std::uint32_t isolated_final_native_crc = 0;
    std::uint32_t isolated_pause_cooperate_calls = 0;
    std::uint32_t isolated_cooperate_calls_at_end = 0;
    std::uint32_t isolated_source_generation = 0;
    std::uint32_t isolated_source_update_sequence = 0;
    std::uint64_t isolated_source_published_sequence = 0;
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE)
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        compute_control_transform_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        compute_control_cache_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        compute_control_service_samples{};
    std::size_t compute_control_transform_stored = 0U;
    std::size_t compute_control_cache_stored = 0U;
    std::size_t compute_control_service_stored = 0U;
    std::uint32_t compute_control_transforms_started = 0U;
    std::uint32_t compute_control_transforms_completed = 0U;
    std::uint32_t compute_control_cache_sync_success = 0U;
    std::uint32_t compute_control_cache_sync_failures = 0U;
    std::uint32_t compute_control_consumer_cooperate_calls = 0U;
    std::uint32_t compute_control_correctness_pass = 0U;
    std::uint32_t compute_control_correctness_fail = 0U;
    std::uint32_t compute_control_source_crc_after = 0U;
    std::uint32_t compute_control_first_native_crc = 0U;
    std::uint32_t compute_control_final_native_crc = 0U;
    bool compute_control_first_native_crc_captured = false;
    bool compute_control_final_native_crc_captured = false;
#endif
    bool first_source_crc_captured = false;
    bool final_source_crc_captured = false;
    bool first_native_crc_captured = false;
    bool final_native_crc_captured = false;
    std::uint32_t source_generation = 0;
    std::uint32_t last_source_update_sequence = 0;
    std::uint32_t scene_ready_surface_update_sequence = 0;
    std::uint32_t final_surface_update_sequence = 0;
    std::uint64_t final_published_sequence = 0;
    std::uint64_t producer_start_us = 0;
    std::uint64_t visible_start_us = 0;
    std::uint64_t visible_elapsed_us = 0;
    bool hook_registered = false;
    bool slots_initialized = false;
    bool immutable_pass = false;
    bool source_generation_initialized = false;
    bool source_sequence_initialized = false;
    bool backlight_enable_failed = false;
    bool backlight_off_failed = false;
    bool visible = false;
    bool timeout_reported = false;
    bool isolated_source_held = false;
    bool isolated_source_crc_captured = false;
    bool isolated_first_native_crc_captured = false;
    bool isolated_final_native_crc_captured = false;
    bool isolated_pause_requested = false;
    bool isolated_pause_acknowledged = false;
    bool isolated_resumed = false;
    np2_presentation_frame_view isolated_source_view{};
    np2_presentation_token isolated_source_token{};
    StaticSemaphore_t isolated_pause_ack_storage{};
    StaticSemaphore_t isolated_pause_resume_storage{};
    SemaphoreHandle_t isolated_pause_ack = nullptr;
    SemaphoreHandle_t isolated_pause_resume = nullptr;
#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
    std::uint8_t *p1_source = nullptr;
    std::uint8_t *p1_destination = nullptr;
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> p1_raw_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> p1_cache_samples{};
    std::array<std::uint32_t, kBenchmarkMeasuredTransforms> p1_guards{};
    std::size_t p1_raw_stored = 0U;
    std::size_t p1_cache_stored = 0U;
    std::uint32_t p1_expected_read_guard = 0U;
    std::uint32_t p1_expected_source_crc = 0U;
    std::uint32_t p1_final_pattern = 0U;
    std::uint32_t p1_validation_crc = 0U;
    std::uint32_t p1_phase_submit_start = 0U;
    std::uint32_t p1_phase_submit_end = 0U;
    std::uint32_t p1_phase_coalesced_start = 0U;
    std::uint32_t p1_phase_coalesced_end = 0U;
    std::uint32_t p1_phase_dropped_start = 0U;
    std::uint32_t p1_phase_dropped_end = 0U;
    std::atomic<std::uint32_t> p1_publish_progress{0U};
    bool p1_producer_ended_during_phase = false;
    bool p1_buffers_initialized = false;
    bool p1_validation_pass = false;
#endif
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)
    std::uint8_t *p8_buffer = nullptr;
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        p8_transform_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        p8_cache_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms>
        p8_service_samples{};
    std::size_t p8_transform_stored = 0U;
    std::size_t p8_cache_stored = 0U;
    std::size_t p8_service_stored = 0U;
    std::uint32_t p8_transforms_started = 0U;
    std::uint32_t p8_transforms_completed = 0U;
    std::uint32_t p8_cache_sync_success = 0U;
    std::uint32_t p8_cache_sync_failures = 0U;
    std::uint32_t p8_consumer_cooperate_calls = 0U;
    std::uint32_t p8_correctness_pass = 0U;
    std::uint32_t p8_correctness_fail = 0U;
    std::uint32_t p8_source_crc_after = 0U;
    std::uint32_t p8_first_native_crc = 0U;
    std::uint32_t p8_final_native_crc = 0U;
    bool p8_first_native_crc_captured = false;
    bool p8_final_native_crc_captured = false;
    std::uint32_t p8_expected_checksum = 0U;
    std::uint64_t p8_control_start_us = 0U;
    std::uint64_t p8_control_end_us = 0U;
    std::uint32_t p8_free_spiram_before = 0U;
    std::uint32_t p8_largest_spiram_before = 0U;
    std::uint32_t p8_free_spiram_after = 0U;
    std::uint32_t p8_largest_spiram_after = 0U;
    bool p8_buffer_initialized = false;
    bool p8_msync_pass = false;
#endif
};

template <std::size_t N>
void benchmark_add_metric(std::uint64_t value, std::array<std::uint64_t, N> &samples,
                          std::size_t &stored, std::uint64_t &count,
                          std::uint64_t &total, std::uint64_t &minimum,
                          std::uint64_t &maximum)
{
    if (stored < N) {
        samples[stored++] = value;
    }
    ++count;
    total += value;
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
}

template <std::size_t N>
std::uint64_t benchmark_percentile(std::array<std::uint64_t, N> &samples,
                                   std::size_t stored, std::size_t numerator)
{
    if (stored == 0U) {
        return 0U;
    }
    std::sort(samples.begin(), samples.begin() + stored);
    const std::size_t rank =
        std::max<std::size_t>(1U, (stored * numerator + 99U) / 100U);
    return samples[rank - 1U];
}

template <std::size_t N>
void benchmark_print_metric(const char *name, std::uint64_t count,
                            std::size_t stored, std::uint64_t total,
                            std::uint64_t minimum, std::uint64_t maximum,
                            std::array<std::uint64_t, N> &samples)
{
    const std::uint64_t average = count == 0U ? 0U : total / count;
    const std::uint64_t p50 = benchmark_percentile(samples, stored, 50U);
    const std::uint64_t p95 = benchmark_percentile(samples, stored, 95U);
    const std::uint64_t p99 = benchmark_percentile(samples, stored, 99U);
    std::printf("P4_NANO_BENCHMARK_TIMING metric=%s count=%" PRIu64
                " stored=%zu min_us=%" PRIu64 " max_us=%" PRIu64
                " average_us=%" PRIu64 " p50_us=%" PRIu64
                " p95_us=%" PRIu64 " p99_us=%" PRIu64
                " p99_quality=%s\n",
                name, count, stored, minimum == std::numeric_limits<std::uint64_t>::max()
                    ? 0U : minimum, maximum, average, p50, p95, p99,
                stored >= 20U ? "usable" : "insufficient");
}

template <std::size_t N>
void benchmark_print_fixed_metric(const char *name,
                                  std::array<std::uint64_t, N> &samples,
                                  std::size_t stored)
{
    std::uint64_t total = 0U;
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0U;
    for (std::size_t index = 0; index < stored; ++index) {
        total += samples[index];
        minimum = std::min(minimum, samples[index]);
        maximum = std::max(maximum, samples[index]);
    }
    benchmark_print_metric(name, stored, stored, total, minimum, maximum,
                           samples);
}

#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
void benchmark_prepare_overlap_analysis(BenchmarkState *state)
{
    if (state == nullptr) {
        return;
    }
    p4_nano_overlap::analyze<kBenchmarkMeasuredTransforms>(
        std::span<const p4_nano_overlap::SubmitInterval>(
            state->overlap_submit_trace.data(),
            state->overlap_submit_trace_stored),
        std::span<const p4_nano_overlap::TransformInterval>(
            state->overlap_transform_trace.data(),
            state->overlap_transform_trace_stored),
        state->overlap_analysis);
    state->overlap_trace_analyzed = true;
    p4_nano_overlap::analyze_pccore<kBenchmarkMeasuredTransforms>(
        std::span<const p4_nano_overlap::PccoreInterval>(
            state->pccore_trace.intervals, state->pccore_trace.stored),
        std::span<const p4_nano_overlap::TransformInterval>(
            state->overlap_transform_trace.data(),
            state->overlap_transform_trace_stored),
        state->producer_result.pccore_exec_count,
        state->pccore_trace.overflow, state->pccore_analysis);
    state->pccore_overlap_analyzed = true;
    p4_nano_overlap::analyze_draw<kBenchmarkMeasuredTransforms>(
        std::span<const p4_nano_overlap::DrawInterval>(
            state->draw_trace.intervals, state->draw_trace.stored),
        std::span<const p4_nano_overlap::TransformInterval>(
            state->overlap_transform_trace.data(),
            state->overlap_transform_trace_stored),
        state->producer_result
            .pccore_profile.phases[NP2_PCCORE_PHASE_DRAW_NESTED]
            .count,
        state->draw_trace.overflow, state->draw_analysis);
    p4_nano_overlap::analyze_hierarchy<kBenchmarkMeasuredTransforms>(
        std::span<const p4_nano_overlap::SubmitInterval>(
            state->overlap_submit_trace.data(),
            state->overlap_submit_trace_stored),
        std::span<const p4_nano_overlap::DrawInterval>(
            state->draw_trace.intervals, state->draw_trace.stored),
        std::span<const p4_nano_overlap::PccoreInterval>(
            state->pccore_trace.intervals, state->pccore_trace.stored),
        state->overlap_analysis, state->draw_analysis, state->pccore_analysis,
        state->hierarchy_analysis);
    state->draw_overlap_analyzed = true;
    const std::uint64_t cpu_exec_count =
        state->producer_result.pccore_profile
            .phases[NP2_PCCORE_PHASE_CPU_EXEC_NESTED]
            .count;
    const std::uint64_t nevent_count =
        state->producer_result.pccore_profile
            .phases[NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED]
            .count;
    p4_nano_overlap::analyze_cpu_nevent<kBenchmarkMeasuredTransforms>(
        std::span<const p4_nano_overlap::CpuNeventInterval>(
            state->cpu_nevent_trace.intervals,
            state->cpu_nevent_trace.stored),
        std::span<const p4_nano_overlap::DrawInterval>(
            state->draw_trace.intervals, state->draw_trace.stored),
        std::span<const p4_nano_overlap::PccoreInterval>(
            state->pccore_trace.intervals, state->pccore_trace.stored),
        std::span<const p4_nano_overlap::TransformInterval>(
            state->overlap_transform_trace.data(),
            state->overlap_transform_trace_stored),
        cpu_exec_count, nevent_count,
        state->cpu_nevent_trace.has_cpu_stored,
        state->cpu_nevent_trace.overflow, state->overlap_analysis,
        state->draw_analysis, state->pccore_analysis,
        state->hierarchy_analysis, state->cpu_nevent_analysis);
    state->cpu_nevent_overlap_analyzed = true;
}

void benchmark_print_pccore_overlap_report(BenchmarkState *state)
{
    if (state == nullptr || !state->pccore_overlap_analyzed) {
        return;
    }
    BenchmarkPccoreAnalysis &analysis = state->pccore_analysis;
    const bool intervals_valid =
        analysis.trace_validation.intervals_valid &&
        analysis.trace_validation.chronological &&
        analysis.trace_validation.call_indices_monotonic;
    const bool trace_valid = analysis.trace_completeness &&
                             !analysis.trace_overflow && intervals_valid &&
                             analysis.trace_validation.intervals_non_overlapping &&
                             analysis.trace_validation.max_concurrent <= 1U;
    std::printf(
        "P4_NANO_PCCORE_TRACE pccore_exec_count=%" PRIu64
        " pccore_trace_intervals=%zu pccore_trace_completeness=%s"
        " pccore_trace_overflow=%s pccore_intervals_valid=%s"
        " pccore_intervals_non_overlapping=%s max_concurrent_pccore_intervals=%zu"
        " validity=%s\n",
        state->producer_result.pccore_exec_count, analysis.pccore_count,
        analysis.trace_completeness ? "PASS" : "FAIL",
        analysis.trace_overflow ? "FAIL" : "PASS",
        intervals_valid ? "PASS" : "FAIL",
        analysis.trace_validation.intervals_non_overlapping ? "PASS" : "FAIL",
        analysis.trace_validation.max_concurrent,
        trace_valid ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_PCCORE_OVERLAP measured_transforms=%zu"
        " transforms_with_pccore_overlap=%zu"
        " transforms_zero_pccore_overlap=%zu overlap_percent=%.3f\n",
        analysis.measured_transform_count,
        analysis.overlapping_transform_count,
        analysis.zero_overlap_transform_count,
        analysis.measured_transform_count == 0U
            ? 0U
            : (100.0 * static_cast<double>(
                   analysis.overlapping_transform_count)) /
                  static_cast<double>(analysis.measured_transform_count));
    benchmark_print_fixed_metric("pccore_overlap_us", analysis.overlap_us,
                                 analysis.overlap_stored);
    benchmark_print_fixed_metric("pccore_overlap_fraction_ppm",
                                 analysis.overlap_fraction_ppm,
                                 analysis.overlap_fraction_stored);
    benchmark_print_fixed_metric("pccore_intersecting_interval_count",
                                 analysis.intersecting_pccore_count,
                                 analysis.intersecting_pccore_count_stored);
    std::printf(
        "P4_NANO_PCCORE_CONDITIONAL zero_overlap_count=%zu"
        " pccore_overlap_count=%zu\n",
        analysis.zero_overlap_transform_count,
        analysis.overlapping_transform_count);
    benchmark_print_fixed_metric("transform_zero_pccore_overlap_us",
                                 analysis.zero_overlap_transform_us,
                                 analysis.zero_overlap_transform_stored);
    benchmark_print_fixed_metric("transform_with_pccore_overlap_us",
                                 analysis.overlapping_transform_us,
                                 analysis.overlapping_transform_stored);
}

void benchmark_print_draw_overlap_report(BenchmarkState *state)
{
    if (state == nullptr || !state->draw_overlap_analyzed) {
        return;
    }
    BenchmarkDrawAnalysis &draw = state->draw_analysis;
    BenchmarkHierarchyAnalysis &hierarchy = state->hierarchy_analysis;
    const bool draw_intervals_valid =
        draw.trace_validation.intervals_valid &&
        draw.trace_validation.chronological &&
        draw.trace_validation.call_indices_monotonic;
    const bool draw_trace_valid = draw.trace_completeness &&
                                  !draw.trace_overflow &&
                                  draw_intervals_valid &&
                                  draw.trace_validation.intervals_non_overlapping &&
                                  draw.trace_validation.max_concurrent <= 1U &&
                                  !state->draw_trace.reentrant;
    std::printf(
        "P4_NANO_DRAW_TRACE draw_nested_count=%" PRIu64
        " draw_trace_intervals=%zu draw_trace_completeness=%s"
        " draw_trace_overflow=%s draw_intervals_valid=%s"
        " draw_intervals_non_overlapping=%s"
        " max_concurrent_draw_intervals=%zu draw_reentrancy_observed=%s"
        " validity=%s\n",
        state->producer_result
            .pccore_profile.phases[NP2_PCCORE_PHASE_DRAW_NESTED]
            .count,
        draw.draw_count, draw.trace_completeness ? "PASS" : "FAIL",
        draw.trace_overflow ? "FAIL" : "PASS",
        draw_intervals_valid ? "PASS" : "FAIL",
        draw.trace_validation.intervals_non_overlapping ? "PASS" : "FAIL",
        draw.trace_validation.max_concurrent,
        state->draw_trace.reentrant ? "FAIL" : "PASS",
        draw_trace_valid ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_DRAW_HIERARCHY draw_without_containing_pccore=%zu"
        " submit_without_containing_draw=%zu submit_subset_draw=%s"
        " draw_subset_pccore=%s hierarchy_validity=%s\n",
        hierarchy.draw_without_containing_pccore_count,
        hierarchy.submit_without_containing_draw_count,
        hierarchy.submit_subset_draw ? "PASS" : "FAIL",
        hierarchy.draw_subset_pccore ? "PASS" : "FAIL",
        hierarchy.validity ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_DRAW_OVERLAP measured_transforms=%zu"
        " transforms_with_draw_overlap=%zu"
        " transforms_zero_draw_overlap=%zu overlap_percent=%.3f\n",
        draw.measured_transform_count, draw.overlapping_transform_count,
        draw.zero_overlap_transform_count,
        draw.measured_transform_count == 0U
            ? 0.0
            : (100.0 * static_cast<double>(draw.overlapping_transform_count)) /
                  static_cast<double>(draw.measured_transform_count));
    benchmark_print_fixed_metric("draw_overlap_us", draw.overlap_us,
                                 draw.overlap_stored);
    benchmark_print_fixed_metric("draw_overlap_fraction_ppm",
                                 draw.overlap_fraction_ppm,
                                 draw.overlap_fraction_stored);
    benchmark_print_fixed_metric("draw_intersecting_interval_count",
                                 draw.intersecting_draw_count,
                                 draw.intersecting_draw_count_stored);
    benchmark_print_fixed_metric("non_submit_draw_overlap_us",
                                 hierarchy.non_submit_draw_overlap_us,
                                 hierarchy.stored);
    benchmark_print_fixed_metric("non_submit_draw_overlap_fraction_ppm",
                                 hierarchy.non_submit_draw_overlap_fraction_ppm,
                                 hierarchy.stored);
    benchmark_print_fixed_metric("non_draw_pccore_overlap_us",
                                 hierarchy.non_draw_pccore_overlap_us,
                                 hierarchy.stored);
    benchmark_print_fixed_metric("outside_pccore_us",
                                 hierarchy.outside_pccore_us,
                                 hierarchy.stored);
    std::printf(
        "P4_NANO_DRAW_CONDITIONAL zero_draw_overlap_count=%zu"
        " draw_overlap_count=%zu\n",
        draw.zero_overlap_transform_count, draw.overlapping_transform_count);
    benchmark_print_fixed_metric("transform_zero_draw_overlap_us",
                                 draw.zero_overlap_transform_us,
                                 draw.zero_overlap_transform_stored);
    benchmark_print_fixed_metric("transform_with_draw_overlap_us",
                                 draw.overlapping_transform_us,
                                 draw.overlapping_transform_stored);
}

void benchmark_print_cpu_nevent_report(BenchmarkState *state)
{
    if (state == nullptr || !state->cpu_nevent_overlap_analyzed) {
        return;
    }
    BenchmarkCpuNeventAnalysis &analysis = state->cpu_nevent_analysis;
    const np2_pccore_profile &profile = state->producer_result.pccore_profile;
    const std::size_t cpu_exec_phase_count = static_cast<std::size_t>(
        profile.phases[NP2_PCCORE_PHASE_CPU_EXEC_NESTED].count);
    const std::size_t nevent_phase_count = static_cast<std::size_t>(
        profile.phases[NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED].count);
    const bool interval_valid =
        analysis.trace_validation.intervals_valid &&
        analysis.trace_validation.chronological &&
        analysis.trace_validation.call_indices_monotonic &&
        analysis.trace_validation.intervals_non_overlapping &&
        analysis.trace_validation.max_concurrent <= 1U;
    std::printf(
        "P4_NANO_CPU_NEVENT_TRACE cpu_exec_phase_count=%zu"
        " nevent_phase_count=%zu pair_trace_intervals=%zu"
        " pair_has_cpu_count=%zu pair_trace_completeness=%s"
        " pair_cpu_completeness=%s cpu_le_nevent=%s overflow=%s"
        " structural_validity=%s"
        " chronology=%s non_overlapping=%s max_concurrent=%zu"
        " pair_without_containing_pccore=%zu"
        " draw_without_containing_nevent=%zu\n",
        cpu_exec_phase_count, nevent_phase_count, analysis.pair_count,
        analysis.cpu_count, analysis.pair_trace_completeness ? "PASS" : "FAIL",
        analysis.pair_cpu_completeness ? "PASS" : "FAIL",
        analysis.cpu_phase_count_order_valid ? "PASS" : "FAIL",
        analysis.trace_overflow ? "FAIL" : "PASS",
        analysis.structural_validity ? "PASS" : "FAIL",
        analysis.trace_validation.chronological ? "PASS" : "FAIL",
        analysis.trace_validation.intervals_non_overlapping ? "PASS" : "FAIL",
        analysis.trace_validation.max_concurrent,
        analysis.pair_without_containing_pccore_count,
        analysis.draw_without_containing_nevent_count);
    std::printf(
        "P4_NANO_CPU_NEVENT_CONTAINMENT pair_subset_pccore=%s"
        " draw_subset_nevent=%s arithmetic_validity=%s"
        " full_hierarchy_validity=%s interval_validity=%s\n",
        analysis.pair_subset_pccore ? "PASS" : "FAIL",
        analysis.draw_subset_nevent ? "PASS" : "FAIL",
        analysis.arithmetic_validity ? "PASS" : "FAIL",
        analysis.full_hierarchy_validity ? "PASS" : "FAIL",
        interval_valid ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_CPU_EXEC_OVERLAP measured_transforms=%zu"
        " transforms_with_cpu_exec_overlap=%zu"
        " transforms_zero_cpu_exec_overlap=%zu overlap_percent=%.3f\n",
        analysis.measured_transform_count,
        analysis.cpu_overlap_transform_count,
        analysis.cpu_zero_overlap_transform_count,
        analysis.measured_transform_count == 0U
            ? 0.0
            : 100.0 * static_cast<double>(analysis.cpu_overlap_transform_count) /
                  static_cast<double>(analysis.measured_transform_count));
    benchmark_print_fixed_metric("cpu_exec_overlap_us",
                                 analysis.cpu_overlap_us,
                                 analysis.cpu_overlap_stored);
    benchmark_print_fixed_metric("cpu_exec_overlap_fraction_ppm",
                                 analysis.cpu_overlap_fraction_ppm,
                                 analysis.cpu_overlap_fraction_stored);
    benchmark_print_fixed_metric("cpu_exec_intersecting_interval_count",
                                 analysis.cpu_intersecting_interval_count,
                                 analysis.cpu_intersecting_interval_count_stored);
    std::printf(
        "P4_NANO_NEVENT_OVERLAP measured_transforms=%zu"
        " transforms_with_nevent_overlap=%zu"
        " transforms_zero_nevent_overlap=%zu overlap_percent=%.3f\n",
        analysis.measured_transform_count,
        analysis.nevent_overlap_transform_count,
        analysis.nevent_zero_overlap_transform_count,
        analysis.measured_transform_count == 0U
            ? 0.0
            : 100.0 * static_cast<double>(analysis.nevent_overlap_transform_count) /
                  static_cast<double>(analysis.measured_transform_count));
    benchmark_print_fixed_metric("nevent_overlap_us",
                                 analysis.nevent_overlap_us,
                                 analysis.nevent_overlap_stored);
    benchmark_print_fixed_metric("nevent_overlap_fraction_ppm",
                                 analysis.nevent_overlap_fraction_ppm,
                                 analysis.nevent_overlap_fraction_stored);
    benchmark_print_fixed_metric("nevent_intersecting_interval_count",
                                 analysis.nevent_intersecting_interval_count,
                                 analysis.nevent_intersecting_interval_count_stored);
    benchmark_print_fixed_metric("non_draw_nevent_overlap_us",
                                 analysis.non_draw_nevent_overlap_us,
                                 analysis.non_draw_nevent_overlap_stored);
    benchmark_print_fixed_metric("non_draw_nevent_overlap_fraction_ppm",
                                 analysis.non_draw_nevent_overlap_fraction_ppm,
                                 analysis.non_draw_nevent_overlap_fraction_stored);
    benchmark_print_fixed_metric("other_pccore_overlap_us",
                                 analysis.other_pccore_overlap_us,
                                 analysis.other_pccore_overlap_stored);
}

void benchmark_print_overlap_report(
    BenchmarkState *state, std::uint32_t successful_submissions_window,
    bool submit_trace_is_complete)
{
    if (state == nullptr || !state->overlap_trace_analyzed) {
        return;
    }
    BenchmarkOverlapAnalysis &analysis = state->overlap_analysis;
    const bool sequence_match_pass =
        analysis.unmatched_acquired_count == 0U &&
        analysis.sequence_metadata_mismatch_count == 0U &&
        analysis.submit_source_sequences_monotonic &&
        analysis.submit_published_sequences_monotonic &&
        analysis.transform_source_sequences_monotonic &&
        analysis.transform_published_sequences_monotonic;
    std::printf(
        "P4_NANO_OVERLAP_TRACE submit_intervals=%zu transform_intervals=%zu "
        "measured_transforms=%zu successful_submissions_window=%" PRIu32
        " submit_trace_completeness=%s trace_overflow=%s "
        "sequence_matching=%s unmatched_submits=%zu "
        "unmatched_acquired=%zu metadata_mismatch=%zu "
        "max_concurrent_submit_intervals=%zu "
        "max_concurrent_expected=1 submit_intervals_non_overlapping=%s\n",
        analysis.submit_count, analysis.transform_count,
        analysis.measured_transform_count,
        successful_submissions_window,
        submit_trace_is_complete ? "PASS" : "FAIL",
        (state->overlap_submit_trace_overflow ||
         state->overlap_transform_trace_overflow)
            ? "FAIL"
            : "PASS",
        sequence_match_pass ? "PASS" : "FAIL",
        analysis.unmatched_submit_count, analysis.unmatched_acquired_count,
        analysis.sequence_metadata_mismatch_count,
        analysis.max_concurrent_submit_count,
        analysis.submit_intervals_non_overlapping ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_OVERLAP_SUMMARY measured=%zu with_overlap=%zu "
        "zero_overlap=%zu one_submit_overlap=%zu "
        "multiple_submit_overlap=%zu overlap_percent=%.3f\n",
        analysis.measured_transform_count,
        analysis.overlapping_transform_count,
        analysis.zero_overlap_transform_count,
        analysis.single_submit_overlap_transform_count,
        analysis.multiple_submit_overlap_transform_count,
        analysis.measured_transform_count == 0U
            ? 0.0
            : static_cast<double>(analysis.overlapping_transform_count) *
                  100.0 / static_cast<double>(analysis.measured_transform_count));
    benchmark_print_fixed_metric("submit_overlap_us", analysis.overlap_us,
                                 analysis.overlap_stored);
    benchmark_print_fixed_metric("submit_overlap_fraction_ppm",
                                 analysis.overlap_fraction_ppm,
                                 analysis.overlap_fraction_stored);
    benchmark_print_fixed_metric("intersecting_submit_count",
                                 analysis.intersecting_submit_count,
                                 analysis.intersecting_submit_count_stored);
    benchmark_print_fixed_metric("transform_zero_overlap_us",
                                 analysis.zero_overlap_transform_us,
                                 analysis.zero_overlap_transform_stored);
    benchmark_print_fixed_metric("transform_with_overlap_us",
                                 analysis.overlapping_transform_us,
                                 analysis.overlapping_transform_stored);
}
#endif

void benchmark_print_vsync_stats(
    const p4_nano_display::DisplaySession &display)
{
    p4_nano_display::VsyncStatsSnapshot stats{};
    p4_nano_display::display_session_snapshot_vsync(&display, &stats);
    const std::uint64_t average_us =
        stats.period_count == 0U
            ? 0U
            : stats.period_total_us / stats.period_count;
    const std::uint64_t refresh_millihz =
        average_us == 0U ? 0U : 1'000'000'000ULL / average_us;
    const bool count_consistent =
        stats.callback_count > 0U &&
        stats.period_count == stats.callback_count - 1U;
    std::printf(
        "P4_NANO_VSYNC callback_registered=%u callback_count=%" PRIu32
        " period_count=%" PRIu32 " period_total_us=%" PRIu64
        " period_avg_us=%" PRIu64 " period_min_us=%" PRIu32
        " period_max_us=%" PRIu32 " jitter_minmax_us=%" PRIu32
        " refresh_millihz=%" PRIu64 " count_consistency=%s\n",
        stats.callback_registered ? 1U : 0U, stats.callback_count,
        stats.period_count, stats.period_total_us, average_us,
        stats.period_min_us, stats.period_max_us,
        stats.period_max_us >= stats.period_min_us
            ? stats.period_max_us - stats.period_min_us
            : 0U,
        refresh_millihz, count_consistent ? "PASS" : "FAIL");
}

#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
bool benchmark_vsync_valid(
    const p4_nano_display::DisplaySession &display)
{
    p4_nano_display::VsyncStatsSnapshot stats{};
    p4_nano_display::display_session_snapshot_vsync(&display, &stats);
    return stats.callback_registered && stats.callback_count > 0U &&
           stats.period_count == stats.callback_count - 1U;
}
#endif

bool benchmark_validate_frame(const np2_presentation_frame_view &view)
{
    return view.ptr != nullptr && esp_ptr_external_ram(view.ptr) &&
           view.width == np2video_golden_width &&
           view.height == np2video_golden_height &&
           view.bpp == np2video_golden_bpp &&
           view.pixel_format == NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE &&
           view.pitch == np2video_golden_pitch &&
           view.width == p4_nano_live_display::kSourceWidth &&
           view.height == p4_nano_live_display::kSourceHeight &&
           view.pitch == p4_nano_live_display::kSourceWidth *
                             sizeof(std::uint16_t);
}

void benchmark_publish_hook(const SCRNMNG_PUBLISH_VIEW *view, void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state == nullptr || view == nullptr) {
        return;
    }
    ++state->publish_hook_calls;
    state->last_surface_update_sequence = view->surface_update_sequence;
    ++state->submit_attempts;
    const bool measurement_active =
        state->scene_ready.load(std::memory_order_acquire);
    const np2_presentation_source_view source{
        .ptr = view->ptr,
        .width = static_cast<std::uint32_t>(view->width),
        .height = static_cast<std::uint32_t>(view->height),
        .pitch = view->pitch,
        .bpp = view->bpp,
        .pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE,
        .source_generation = view->surface_generation,
        .source_update_sequence = view->surface_update_sequence,
    };
    const std::uint64_t expected_sequence = state->publisher.published_sequence + 1U;
    const std::uint64_t submit_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (measurement_active) {
        /* Publish the timestamp before submit exposes PENDING to the
         * consumer.  A failed submit leaves a provisional entry that a
         * retry of the same expected sequence may safely replace. */
        BenchmarkTimestamp &entry =
            state->timestamp_ring[expected_sequence % kBenchmarkTimestampRingSize];
        entry.submit_start_us.store(submit_start, std::memory_order_relaxed);
        entry.sequence.store(expected_sequence, std::memory_order_release);
    }
    const np2_presentation_status status = np2_presentation_submit(
        &state->publisher, &source);
    const std::uint64_t submit_end =
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (measurement_active) {
        benchmark_add_metric(submit_end - submit_start, state->submit_samples,
                             state->submit_stored, state->submit_count,
                             state->submit_total_us, state->submit_min_us,
                             state->submit_max_us);
    }
    if (status == NP2_PRESENTATION_OK) {
        ++state->successful_submissions;
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
        if (measurement_active) {
            const p4_nano_overlap::SubmitInterval interval{
                .start_us = submit_start,
                .end_us = submit_end,
                .source_update_sequence = view->surface_update_sequence,
                .published_sequence = state->publisher.published_sequence,
            };
            (void)p4_nano_overlap::append_bounded(
                state->overlap_submit_trace,
                state->overlap_submit_trace_stored, interval,
                state->overlap_submit_trace_overflow);
        }
#endif
#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
        state->p1_publish_progress.fetch_add(1U, std::memory_order_relaxed);
#endif
        const std::uint64_t published_sequence = state->publisher.published_sequence;
        if (measurement_active && published_sequence == expected_sequence &&
            published_sequence != 0U) {
            /* The entry was published before submit, so there is no
             * producer/consumer publication window to lose here. */
        } else if (measurement_active) {
            ++state->producer_latency_unavailable;
        }
    } else if (status != NP2_PRESENTATION_DROPPED) {
        ++state->submit_failures;
        state->publish_failed.store(true, std::memory_order_release);
    }
}

void benchmark_scene_ready(std::uint32_t generation,
                           std::uint32_t update_sequence, void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state == nullptr) {
        return;
    }
    state->scene_generation.store(generation, std::memory_order_relaxed);
    state->scene_update_sequence.store(update_sequence,
                                       std::memory_order_relaxed);
    state->scene_ready_publish_hook_calls = state->publish_hook_calls;
    state->scene_ready_submit_attempts = state->submit_attempts;
    state->scene_ready_successful_submissions =
        state->successful_submissions;
    state->scene_ready_submit_failures = state->submit_failures;
    state->scene_ready_coalesced =
        np2_presentation_coalesced_count(&state->publisher);
    state->scene_ready_dropped =
        np2_presentation_dropped_count(&state->publisher);
    state->scene_ready_surface_update_sequence = update_sequence;
    /* The runner invokes this callback on the producer task before its
     * continuous scene-window loop.  Capture all deltas first, then publish
     * scene_ready; the submit hook therefore cannot observe the active bit
     * until the matching baselines are complete. */
    /* Start the VSYNC statistics at the same scene boundary used by the
     * existing LIVE/isolated counters.  This keeps panel initialization and
     * prelude traffic out of the baseline window without adding a new timing
     * path for the frame pipeline. */
    p4_nano_display::display_session_reset_vsync(&state->display);
    state->scene_ready.store(true, std::memory_order_release);
}

bool benchmark_runner_output(void *, const char *data, std::size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }
    const bool complete = std::fwrite(data, 1, length, stdout) == length;
    std::fflush(stdout);
    return complete;
}

bool benchmark_runner_ready(void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state == nullptr || !state->slots_initialized) {
        return false;
    }
    state->producer_core.store(xPortGetCoreID(), std::memory_order_relaxed);
    state->producer_priority.store(
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)),
        std::memory_order_relaxed);
    scrnmng_set_publish_hook(benchmark_publish_hook, state);
    state->hook_registered = true;
    std::printf("P4_NANO_BENCHMARK_PUBLISHER_READY slots=%zu slot_bytes=%zu\n",
                kSlotCount, kSlotBytes);
    return true;
}

void benchmark_runner_stopping(void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state != nullptr && state->hook_registered) {
        scrnmng_set_publish_hook(nullptr, nullptr);
        state->hook_registered = false;
    }
}

void benchmark_runner_complete(const np2video_runner_result *result, void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state != nullptr && result != nullptr) {
        state->producer_result = *result;
        state->producer_done.store(true, std::memory_order_release);
    }
}

bool benchmark_enable_backlight(BenchmarkState *state);
void benchmark_hold_visible(BenchmarkState *state);
void benchmark_release(BenchmarkState *state, np2_presentation_token *token);

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
void benchmark_producer_cooperate(std::uint32_t cooperate_calls,
                                  void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state != nullptr) {
        state->producer_cooperate_calls.store(cooperate_calls,
                                              std::memory_order_release);
    }
}

bool benchmark_pause_at_cooperate(std::uint32_t cooperate_calls, void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    if (state == nullptr ||
        !state->producer_pause_requested.load(std::memory_order_acquire)) {
        return true;
    }
    state->isolated_pause_cooperate_calls = cooperate_calls;
    state->producer_pause_acknowledged.store(true, std::memory_order_release);
    if (xSemaphoreGive(state->isolated_pause_ack) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(state->isolated_pause_resume, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    state->producer_pause_acknowledged.store(false, std::memory_order_release);
    return true;
}

bool benchmark_hold_isolated_source(BenchmarkState *state)
{
    if (state == nullptr) {
        return false;
    }
    for (;;) {
        if (state->producer_done.load(std::memory_order_acquire)) {
            return false;
        }
        if (!state->scene_ready.load(std::memory_order_acquire)) {
            vTaskDelay(kConsumerPollDelayTicks);
            continue;
        }
        np2_presentation_frame_view view{};
        np2_presentation_token token{};
        const np2_presentation_status acquire = np2_presentation_acquire(
            &state->publisher, &view, &token);
        if (acquire == NP2_PRESENTATION_NO_FRAME) {
            vTaskDelay(kConsumerPollDelayTicks);
            continue;
        }
        if (acquire != NP2_PRESENTATION_OK || !benchmark_validate_frame(view) ||
            view.source_generation !=
                state->scene_generation.load(std::memory_order_relaxed) ||
            view.source_update_sequence <=
                state->scene_update_sequence.load(std::memory_order_relaxed)) {
            if (acquire == NP2_PRESENTATION_OK) {
                benchmark_release(state, &token);
            }
            return false;
        }
        ++state->acquisitions;
        state->isolated_source_view = view;
        state->isolated_source_token = token;
        state->isolated_source_held = true;
        state->isolated_source_generation = view.source_generation;
        state->isolated_source_update_sequence = view.source_update_sequence;
        state->isolated_source_published_sequence = view.published_sequence;
        state->isolated_source_crc = p4_nano_display::crc32(
            view.ptr, np2video_golden_visible_bytes);
        state->isolated_source_crc_captured = true;
        return true;
    }
}

bool benchmark_request_isolated_pause(BenchmarkState *state)
{
    if (state == nullptr || state->isolated_pause_ack == nullptr ||
        state->isolated_pause_resume == nullptr) {
        return false;
    }
    state->isolated_pause_requested = true;
    state->producer_pause_requested.store(true, std::memory_order_release);
    std::printf("PRODUCER_PAUSE_REQUESTED\n");
    if (xSemaphoreTake(state->isolated_pause_ack,
                       pdMS_TO_TICKS(5000)) != pdTRUE ||
        !state->producer_pause_acknowledged.load(std::memory_order_acquire)) {
        return false;
    }
    state->isolated_pause_acknowledged = true;
    std::printf("PRODUCER_PAUSE_ACK cooperate_calls=%" PRIu32 "\n",
                state->isolated_pause_cooperate_calls);
    return true;
}

bool benchmark_run_isolated_samples(BenchmarkState *state,
                                     bool compute_control = false,
                                     bool psram_read_control = false)
{
    if (state == nullptr || !state->isolated_source_held ||
        !state->isolated_source_crc_captured) {
        return false;
    }
    const auto source = std::span<const std::uint16_t>(
        reinterpret_cast<const std::uint16_t *>(state->isolated_source_view.ptr),
        p4_nano_display::kTransformSourcePixelCount);
    const auto destination = std::span<std::uint16_t>(
        state->display.framebuffer,
        p4_nano_display::kTransformDestinationPixelCount);
    auto *transform_samples = &state->isolated_transform_samples;
    auto *cache_samples = &state->isolated_cache_samples;
    auto *service_samples = &state->isolated_service_samples;
    std::size_t *transform_stored = &state->isolated_transform_stored;
    std::size_t *cache_stored = &state->isolated_cache_stored;
    std::size_t *service_stored = &state->isolated_service_stored;
    std::uint32_t *transforms_started = &state->isolated_transforms_started;
    std::uint32_t *transforms_completed = &state->isolated_transforms_completed;
    std::uint32_t *cache_sync_success = &state->isolated_cache_sync_success;
    std::uint32_t *cache_sync_failures = &state->isolated_cache_sync_failures;
    std::uint32_t *consumer_cooperate_calls =
        &state->isolated_consumer_cooperate_calls;
    std::uint32_t *correctness_pass = &state->isolated_correctness_pass;
    std::uint32_t *correctness_fail = &state->isolated_correctness_fail;
    std::uint32_t *source_crc_after = &state->isolated_source_crc_after;
    std::uint32_t *first_native_crc = &state->isolated_first_native_crc;
    std::uint32_t *final_native_crc = &state->isolated_final_native_crc;
    bool *first_native_crc_captured =
        &state->isolated_first_native_crc_captured;
    bool *final_native_crc_captured =
        &state->isolated_final_native_crc_captured;
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE)
    if (compute_control) {
        transform_samples = &state->compute_control_transform_samples;
        cache_samples = &state->compute_control_cache_samples;
        service_samples = &state->compute_control_service_samples;
        transform_stored = &state->compute_control_transform_stored;
        cache_stored = &state->compute_control_cache_stored;
        service_stored = &state->compute_control_service_stored;
        transforms_started = &state->compute_control_transforms_started;
        transforms_completed = &state->compute_control_transforms_completed;
        cache_sync_success = &state->compute_control_cache_sync_success;
        cache_sync_failures = &state->compute_control_cache_sync_failures;
        consumer_cooperate_calls =
            &state->compute_control_consumer_cooperate_calls;
        correctness_pass = &state->compute_control_correctness_pass;
        correctness_fail = &state->compute_control_correctness_fail;
        source_crc_after = &state->compute_control_source_crc_after;
        first_native_crc = &state->compute_control_first_native_crc;
        final_native_crc = &state->compute_control_final_native_crc;
        first_native_crc_captured =
            &state->compute_control_first_native_crc_captured;
        final_native_crc_captured =
            &state->compute_control_final_native_crc_captured;
    }
#endif
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)
    if (psram_read_control) {
        transform_samples = &state->p8_transform_samples;
        cache_samples = &state->p8_cache_samples;
        service_samples = &state->p8_service_samples;
        transform_stored = &state->p8_transform_stored;
        cache_stored = &state->p8_cache_stored;
        service_stored = &state->p8_service_stored;
        transforms_started = &state->p8_transforms_started;
        transforms_completed = &state->p8_transforms_completed;
        cache_sync_success = &state->p8_cache_sync_success;
        cache_sync_failures = &state->p8_cache_sync_failures;
        consumer_cooperate_calls = &state->p8_consumer_cooperate_calls;
        correctness_pass = &state->p8_correctness_pass;
        correctness_fail = &state->p8_correctness_fail;
        source_crc_after = &state->p8_source_crc_after;
        first_native_crc = &state->p8_first_native_crc;
        final_native_crc = &state->p8_final_native_crc;
        first_native_crc_captured = &state->p8_first_native_crc_captured;
        final_native_crc_captured = &state->p8_final_native_crc_captured;
    }
#else
    (void)psram_read_control;
#endif
    std::printf("%s\n", compute_control ?
                "COMPUTE_CONTROL_B_MEASUREMENT_BEGIN" :
                (psram_read_control ?
                     "PSRAM_READ_CONTROL_MEASUREMENT_BEGIN" :
                     "ISOLATED_MEASUREMENT_BEGIN"));
    for (std::uint32_t transform_index = 0U;
         transform_index < kBenchmarkTotalTransforms; ++transform_index) {
        const bool correctness_sample = transform_index == 0U ||
                                        transform_index + 1U ==
                                            kBenchmarkTotalTransforms;
        const std::uint64_t service_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        ++*transforms_started;
        const std::uint64_t transform_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const bool transformed = p4_nano_display::transform_to_native(
            source, destination,
            p4_nano_display::QuarterTurn::CounterClockwise);
        const std::uint64_t transform_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - transform_start;
        if (!transformed) {
            return false;
        }
        const std::uint64_t cache_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const esp_err_t sync_result =
            p4_nano_display::display_session_sync_framebuffer(&state->display);
        const std::uint64_t cache_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - cache_start;
        if (sync_result != ESP_OK) {
            ++*cache_sync_failures;
            return false;
        }
        ++*cache_sync_success;
        ++state->native_framebuffer_updates;
        if (correctness_sample) {
            const std::uint32_t source_crc = p4_nano_display::crc32(
                state->isolated_source_view.ptr,
                np2video_golden_visible_bytes);
            const std::uint32_t native_crc = p4_nano_display::crc32(
                reinterpret_cast<const std::uint8_t *>(
                    state->display.framebuffer),
                p4_nano_display::kNativeFramebufferBytes);
            if (source_crc == state->isolated_source_crc) {
                ++*correctness_pass;
            } else {
                ++*correctness_fail;
            }
            if (!compute_control && !psram_read_control) {
                state->isolated_source_crc_after = source_crc;
            } else {
                *source_crc_after = source_crc;
            }
            if (transform_index == 0U) {
                *first_native_crc = native_crc;
                *first_native_crc_captured = true;
            } else {
                *final_native_crc = native_crc;
                *final_native_crc_captured = true;
            }
        }
        if (transform_index == 0U && !benchmark_enable_backlight(state)) {
            return false;
        }
        const std::uint64_t service_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - service_start;
        if (benchmark_is_measured_sample(transform_index)) {
            const std::size_t measured_index =
                transform_index - kBenchmarkWarmupTransforms;
            (*transform_samples)[measured_index] = transform_us;
            (*cache_samples)[measured_index] = cache_us;
            (*service_samples)[measured_index] = service_us;
            *transform_stored = measured_index + 1U;
            *cache_stored = measured_index + 1U;
            *service_stored = measured_index + 1U;
        }
        ++*transforms_completed;
        ++*consumer_cooperate_calls;
        /* Keep the real one-tick CPU0 cooperation outside every measured
         * transform/cache interval, including warm-up and final validation. */
        np2_host_taskmng_cooperate();
    }
    if (!compute_control && !psram_read_control) {
        state->isolated_cooperate_calls_at_end =
            state->producer_cooperate_calls.load(std::memory_order_acquire);
    }
    const std::uint32_t producer_cooperate_calls =
        state->producer_cooperate_calls.load(std::memory_order_acquire);
    std::printf("%s producer_cooperate_calls=%" PRIu32
                " consumer_cooperate_calls=%" PRIu32 "\n",
                compute_control ? "COMPUTE_CONTROL_B_MEASUREMENT_END" :
                                   (psram_read_control ?
                                        "PSRAM_READ_CONTROL_MEASUREMENT_END" :
                                        "ISOLATED_MEASUREMENT_END"),
                producer_cooperate_calls,
                *consumer_cooperate_calls);
    return true;
}

esp_err_t run_isolated_benchmark_after_start(BenchmarkState *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool failed = false;
    if (!benchmark_hold_isolated_source(state) ||
        !benchmark_request_isolated_pause(state)) {
        failed = true;
    }
    if (!failed && !benchmark_run_isolated_samples(state)) {
        failed = true;
    }
    if (state->isolated_transforms_completed != kBenchmarkTotalTransforms) {
        failed = true;
    }
    /* Always take the normal producer stop path, including an early
     * acquisition/pause/transform failure; otherwise a failed isolated run
     * could leave CPU1 free-running until its outer slice limit. */
    state->stop_requested.store(true, std::memory_order_release);
    const bool source_immutable =
        state->isolated_source_crc_captured &&
        state->isolated_source_crc == state->isolated_source_crc_after;
    const bool native_stable =
        state->isolated_first_native_crc_captured &&
        state->isolated_final_native_crc_captured &&
        state->isolated_first_native_crc == state->isolated_final_native_crc;
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->isolated_cooperate_calls_at_end &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    if (state->publish_failed.load(std::memory_order_acquire) ||
        !source_immutable || !native_stable || !pause_stable ||
        state->isolated_correctness_fail != 0U ||
        state->isolated_transform_stored != kBenchmarkMeasuredTransforms ||
        state->isolated_cache_stored != kBenchmarkMeasuredTransforms ||
        state->isolated_service_stored != kBenchmarkMeasuredTransforms ||
        state->isolated_cache_sync_failures != 0U) {
        failed = true;
    }
    benchmark_print_fixed_metric("isolated_transform_only_us",
                                 state->isolated_transform_samples,
                                 state->isolated_transform_stored);
    benchmark_print_fixed_metric("isolated_cache_sync_us",
                                 state->isolated_cache_samples,
                                 state->isolated_cache_stored);
    benchmark_print_fixed_metric("isolated_consumer_service_us",
                                 state->isolated_service_samples,
                                 state->isolated_service_stored);
    std::printf("ISOLATED_TRANSFORM_CORRECTNESS source_crc_before=0x%08" PRIx32
                " source_crc_after=0x%08" PRIx32
                " first_native_crc=0x%08" PRIx32
                " final_native_crc=0x%08" PRIx32 " result=%s\n",
                state->isolated_source_crc, state->isolated_source_crc_after,
                state->isolated_first_native_crc,
                state->isolated_final_native_crc,
                (!failed && source_immutable && native_stable) ? "PASS" : "FAIL");
    std::printf("ISOLATED_TRANSFORM_SAMPLE_COUNTS transform=%zu cache_sync=%zu "
                "consumer_service=%zu warmup=%u measured=%u final_validation=%u\n",
                state->isolated_transform_stored,
                state->isolated_cache_stored,
                state->isolated_service_stored,
                static_cast<unsigned>(kBenchmarkWarmupTransforms),
                static_cast<unsigned>(kBenchmarkMeasuredTransforms),
                static_cast<unsigned>(kBenchmarkFinalValidationTransforms));
    benchmark_print_vsync_stats(state->display);

    /* Resume only after final CRC/counter capture and the isolated summary
     * have completed.  The held presentation lease is released after the
     * producer has left the pause protocol, then the normal visible-hold and
     * cleanup lifecycle is used. */
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    const esp_err_t backlight_off_result =
        p4_nano_board::display_backlight_set(0U);
    state->backlight_off_failed = backlight_off_result != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions || !scheduling_contract) {
        failed = true;
    }
    std::printf("P4_NANO_TRANSFORM_ISOLATED_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}

#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)
esp_err_t run_ppa_rotation_benchmark_after_start(BenchmarkState *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool failed = false;
    esp_err_t ppa_result = ESP_FAIL;
    if (!benchmark_hold_isolated_source(state) ||
        !benchmark_request_isolated_pause(state)) {
        failed = true;
    }
    if (!failed) {
        const p4_nano_ppa_rotation::Input input{
            .source = state->isolated_source_view.ptr,
            .source_bytes = np2video_golden_visible_bytes,
            .source_width = static_cast<std::uint32_t>(
                state->isolated_source_view.width),
            .source_height = static_cast<std::uint32_t>(
                state->isolated_source_view.height),
            .source_pitch_bytes = state->isolated_source_view.pitch,
            .source_bpp = state->isolated_source_view.bpp,
            .presentation_slot0 = state->slots[0].ptr,
            .presentation_slot1 = state->slots[1].ptr,
            .presentation_slot_bytes = kSlotBytes,
            .native_framebuffer = state->display.framebuffer,
            .native_framebuffer_bytes =
                p4_nano_display::kNativeFramebufferBytes,
        };
        ppa_result = p4_nano_ppa_rotation::run(input);
        if (ppa_result != ESP_OK) {
            failed = true;
        }
    }

    /* Stop the producer before releasing the pause.  PPA does not write the
     * native framebuffer, so no transform/scanout ownership handoff is added
     * here. */
    state->stop_requested.store(true, std::memory_order_release);
    state->isolated_cooperate_calls_at_end =
        state->producer_cooperate_calls.load(std::memory_order_acquire);
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->isolated_cooperate_calls_at_end &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    state->backlight_off_failed =
        p4_nano_board::display_backlight_set(0U) != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    const bool vsync_valid = benchmark_vsync_valid(state->display);
    if (ppa_result != ESP_OK || state->publish_failed.load(
            std::memory_order_acquire) || !pause_stable ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions ||
        state->producer_result.status != ESP_OK || !scheduling_contract ||
        !vsync_valid) {
        failed = true;
    }
    benchmark_print_vsync_stats(state->display);
    std::printf("P4_NANO_PPA_ROTATION_VSYNC_VALID=%s\n",
                vsync_valid ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_ROTATION_LIFECYCLE_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#endif

#if defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE)
esp_err_t run_ppa_internal_tile_benchmark_after_start(BenchmarkState *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool failed = false;
    esp_err_t tile_result = ESP_FAIL;
    if (!benchmark_hold_isolated_source(state) ||
        !benchmark_request_isolated_pause(state)) {
        failed = true;
    }
    if (!failed) {
        const p4_nano_ppa_internal_tile::Input input{
            .source = state->isolated_source_view.ptr,
            .source_bytes = np2video_golden_visible_bytes,
            .source_width = static_cast<std::uint32_t>(
                state->isolated_source_view.width),
            .source_height = static_cast<std::uint32_t>(
                state->isolated_source_view.height),
            .source_pitch_bytes = state->isolated_source_view.pitch,
            .source_bpp = state->isolated_source_view.bpp,
            .presentation_slot0 = state->slots[0].ptr,
            .presentation_slot1 = state->slots[1].ptr,
            .presentation_slot_bytes = kSlotBytes,
            .native_framebuffer = state->display.framebuffer,
            .native_framebuffer_bytes =
                p4_nano_display::kNativeFramebufferBytes,
        };
        tile_result = p4_nano_ppa_internal_tile::run(input);
        failed = tile_result != ESP_OK;
    }

    state->stop_requested.store(true, std::memory_order_release);
    state->isolated_cooperate_calls_at_end =
        state->producer_cooperate_calls.load(std::memory_order_acquire);
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->isolated_cooperate_calls_at_end &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    state->backlight_off_failed =
        p4_nano_board::display_backlight_set(0U) != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    const bool vsync_valid = benchmark_vsync_valid(state->display);
    if (tile_result != ESP_OK || state->publish_failed.load(
            std::memory_order_acquire) || !pause_stable ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions ||
        state->producer_result.status != ESP_OK || !scheduling_contract ||
        !vsync_valid) {
        failed = true;
    }
    benchmark_print_vsync_stats(state->display);
    std::printf("P4_NANO_PPA_INTERNAL_TILE_VSYNC_VALID=%s\n",
                vsync_valid ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_INTERNAL_TILE_LIFECYCLE_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#endif

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE)
esp_err_t run_compute_control_benchmark_after_start(BenchmarkState *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool failed = false;
    bool control_started = false;
    std::printf("P4_NANO_COMPUTE_CONTROL_SEQUENCE=A_then_B\n");
    if (!benchmark_hold_isolated_source(state) ||
        !benchmark_request_isolated_pause(state)) {
        failed = true;
    }
    if (!failed && !benchmark_run_isolated_samples(state, false)) {
        failed = true;
    }
    if (!failed &&
        (state->isolated_transforms_completed != kBenchmarkTotalTransforms ||
         state->isolated_transform_stored != kBenchmarkMeasuredTransforms)) {
        failed = true;
    }
    if (!failed && !p4_nano_compute_control::start_and_calibrate()) {
        failed = true;
    }
    const auto &calibration = p4_nano_compute_control::calibration();
    const bool layout_validity =
        p4_nano_compute_control::stack_internal() &&
        p4_nano_compute_control::tcb_internal() &&
        p4_nano_compute_control::state_internal() &&
        esp_ptr_in_iram(reinterpret_cast<const void *>(
            &p4_nano_compute_control::run_chunk)) &&
        esp_ptr_executable(reinterpret_cast<const void *>(
            &p4_nano_compute_control::run_chunk));
    std::printf("P4_NANO_COMPUTE_CONTROL_CONFIG task_core=1 task_priority=%u "
                "stack_depth=%u stack_internal=%u tcb_internal=%u "
                "state_internal=%u hot_loop_iram=%u hot_loop_executable=%u "
                "layout_validity=%s "
                "stack_bytes=%" PRIu32 " tcb_bytes=%" PRIu32
                " state_bytes=%" PRIu32 " static_bytes=%" PRIu32
                " calibration_iterations=%" PRIu32
                " calibration_elapsed_us=%" PRIu64
                " chunk_iterations=%" PRIu32
                " target_interval_us=%" PRIu32
                " relief_ticks=1\n",
                static_cast<unsigned>(tskIDLE_PRIORITY + 3U),
                static_cast<unsigned>(p4_nano_compute_control::kTaskStackWords),
                p4_nano_compute_control::stack_internal() ? 1U : 0U,
                p4_nano_compute_control::tcb_internal() ? 1U : 0U,
                p4_nano_compute_control::state_internal() ? 1U : 0U,
                esp_ptr_in_iram(reinterpret_cast<const void *>(
                    &p4_nano_compute_control::run_chunk)) ? 1U : 0U,
                esp_ptr_executable(reinterpret_cast<const void *>(
                    &p4_nano_compute_control::run_chunk)) ? 1U : 0U,
                layout_validity ? "PASS" : "FAIL",
                p4_nano_compute_control::stack_bytes(),
                p4_nano_compute_control::tcb_bytes(),
                p4_nano_compute_control::state_bytes(),
                p4_nano_compute_control::static_bytes(),
                calibration.calibration_iterations,
                calibration.calibration_elapsed_us,
                calibration.chunk_iterations, calibration.target_interval_us);
    bool control_ready = !failed;
    if (!failed && !layout_validity) {
        failed = true;
        if (!p4_nano_compute_control::stop()) {
            failed = true;
        }
        control_ready = false;
    }
    if (!failed && !p4_nano_compute_control::begin()) {
        failed = true;
        (void)p4_nano_compute_control::stop();
        control_ready = false;
    } else if (!failed) {
        control_started = true;
        if (!benchmark_run_isolated_samples(state, true)) {
            failed = true;
        }
    }
    if ((control_started || control_ready) && !p4_nano_compute_control::stop()) {
        failed = true;
    }
    const auto &health = p4_nano_compute_control::health();
    const std::uint32_t source_crc_after =
        state->isolated_source_held && state->isolated_source_view.ptr != nullptr
            ? p4_nano_display::crc32(state->isolated_source_view.ptr,
                                      np2video_golden_visible_bytes)
            : 0U;
    state->compute_control_source_crc_after = source_crc_after;
    const bool source_immutable = state->isolated_source_crc_captured &&
                                  source_crc_after == state->isolated_source_crc;
    const bool a_native_stable =
        state->isolated_first_native_crc_captured &&
        state->isolated_final_native_crc_captured &&
        state->isolated_first_native_crc == state->isolated_final_native_crc;
    const bool b_native_stable =
        state->compute_control_first_native_crc_captured &&
        state->compute_control_final_native_crc_captured &&
        state->compute_control_first_native_crc ==
            state->compute_control_final_native_crc;
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->isolated_cooperate_calls_at_end &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    if (state->publish_failed.load(std::memory_order_acquire) ||
        !source_immutable || !a_native_stable || !b_native_stable ||
        !pause_stable || state->isolated_correctness_fail != 0U ||
        state->compute_control_correctness_fail != 0U ||
        state->compute_control_transforms_completed != kBenchmarkTotalTransforms ||
        state->compute_control_transform_stored != kBenchmarkMeasuredTransforms ||
        state->compute_control_cache_stored != kBenchmarkMeasuredTransforms ||
        state->compute_control_service_stored != kBenchmarkMeasuredTransforms ||
        state->isolated_cache_sync_failures != 0U ||
        state->compute_control_cache_sync_failures != 0U ||
        !layout_validity ||
        !health.ready || !health.clean_stop || health.chunks == 0U ||
        health.iterations == 0U || health.relief_count == 0U ||
        health.checksum == 0U) {
        failed = true;
    }
    benchmark_print_fixed_metric("compute_control_A_transform_only_us",
                                 state->isolated_transform_samples,
                                 state->isolated_transform_stored);
    benchmark_print_fixed_metric("compute_control_A_cache_sync_us",
                                 state->isolated_cache_samples,
                                 state->isolated_cache_stored);
    benchmark_print_fixed_metric("compute_control_A_consumer_service_us",
                                 state->isolated_service_samples,
                                 state->isolated_service_stored);
    benchmark_print_fixed_metric("compute_control_B_transform_only_us",
                                 state->compute_control_transform_samples,
                                 state->compute_control_transform_stored);
    benchmark_print_fixed_metric("compute_control_B_cache_sync_us",
                                 state->compute_control_cache_samples,
                                 state->compute_control_cache_stored);
    benchmark_print_fixed_metric("compute_control_B_consumer_service_us",
                                 state->compute_control_service_samples,
                                 state->compute_control_service_stored);
    std::printf("P4_NANO_COMPUTE_CONTROL_HEALTH chunks=%" PRIu64
                " iterations=%" PRIu64 " relief_count=%" PRIu32
                " checksum=0x%08" PRIx32 " stack_high_water_words=%" PRIu32
                " ready=%u clean_stop=%u\n",
                health.chunks, health.iterations, health.relief_count,
                health.checksum, health.stack_high_water_words,
                health.ready ? 1U : 0U, health.clean_stop ? 1U : 0U);
    std::printf("P4_NANO_COMPUTE_CONTROL_CORRECTNESS A_source_crc=0x%08" PRIx32
                " B_source_crc=0x%08" PRIx32
                " A_native_crc=0x%08" PRIx32
                " B_native_crc=0x%08" PRIx32 " result=%s\n",
                state->isolated_source_crc, source_crc_after,
                state->isolated_final_native_crc,
                state->compute_control_final_native_crc,
                (!failed && source_immutable && a_native_stable &&
                 b_native_stable) ? "PASS" : "FAIL");
    std::printf("P4_NANO_COMPUTE_CONTROL_SAMPLE_COUNTS A_transform=%zu "
                "A_cache_sync=%zu A_service=%zu B_transform=%zu "
                "B_cache_sync=%zu B_service=%zu warmup=%u measured=%u "
                "final_validation=%u\n",
                state->isolated_transform_stored,
                state->isolated_cache_stored, state->isolated_service_stored,
                state->compute_control_transform_stored,
                state->compute_control_cache_stored,
                state->compute_control_service_stored,
                static_cast<unsigned>(kBenchmarkWarmupTransforms),
                static_cast<unsigned>(kBenchmarkMeasuredTransforms),
                static_cast<unsigned>(kBenchmarkFinalValidationTransforms));
    benchmark_print_vsync_stats(state->display);

    state->stop_requested.store(true, std::memory_order_release);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    const esp_err_t backlight_off_result =
        p4_nano_board::display_backlight_set(0U);
    state->backlight_off_failed = backlight_off_result != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions || !scheduling_contract) {
        failed = true;
    }
    std::printf("P4_NANO_COMPUTE_CONTROL_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#endif
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)
void benchmark_p8_free_buffer(BenchmarkState *state);
bool benchmark_p8_prepare_buffer(BenchmarkState *state);

esp_err_t run_psram_read_control_benchmark_after_start(BenchmarkState *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bool failed = false;
    bool control_started = false;
    bool control_ready = false;
    std::printf("P4_NANO_PSRAM_READ_CONTROL_SEQUENCE=A_then_B\n");
    if (!benchmark_hold_isolated_source(state) ||
        !benchmark_request_isolated_pause(state)) {
        failed = true;
    }
    if (!failed && !benchmark_p8_prepare_buffer(state)) {
        failed = true;
    }
    if (!failed && !benchmark_run_isolated_samples(state, false, false)) {
        failed = true;
    }
    if (!failed &&
        (state->isolated_transforms_completed != kBenchmarkTotalTransforms ||
         state->isolated_transform_stored != kBenchmarkMeasuredTransforms ||
         state->isolated_cache_stored != kBenchmarkMeasuredTransforms ||
         state->isolated_service_stored != kBenchmarkMeasuredTransforms)) {
        failed = true;
    }

    if (!failed && !p4_nano_psram_read_control::start_and_calibrate(
                       state->p8_buffer, state->p8_expected_checksum)) {
        failed = true;
    }
    const auto &calibration = p4_nano_psram_read_control::calibration();
    const bool layout_validity =
        state->p8_buffer != nullptr &&
        esp_ptr_external_ram(state->p8_buffer) &&
        (reinterpret_cast<std::uintptr_t>(state->p8_buffer) %
             p4_nano_psram_read_control::kAlignmentBytes) == 0U &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->isolated_source_view.ptr,
                        np2video_golden_visible_bytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->slots[0].ptr, kSlotBytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->slots[1].ptr, kSlotBytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->display.framebuffer,
                        p4_nano_display::kNativeFramebufferBytes) &&
        p4_nano_psram_read_control::stack_internal() &&
        p4_nano_psram_read_control::tcb_internal() &&
        p4_nano_psram_read_control::state_internal() &&
        esp_ptr_in_iram(reinterpret_cast<const void *>(
            &p4_nano_psram_read_control::read_sweep)) &&
        esp_ptr_executable(reinterpret_cast<const void *>(
            &p4_nano_psram_read_control::read_sweep));
    std::printf(
        "P4_NANO_PSRAM_READ_CONTROL_CONFIG task_core=1 task_priority=%u "
        "stack_depth=%u stack_internal=%u tcb_internal=%u state_internal=%u "
        "kernel_iram=%u kernel_executable=%u layout_validity=%s "
        "stack_bytes=%" PRIu32 " tcb_bytes=%" PRIu32
        " state_bytes=%" PRIu32 " static_bytes=%" PRIu32
        " calibration_sweeps=%" PRIu32
        " calibration_elapsed_us=%" PRIu64
        " sweeps_per_relief=%" PRIu32 " target_interval_us=%" PRIu32
        " relief_ticks=1\n",
        static_cast<unsigned>(tskIDLE_PRIORITY + 3U),
        static_cast<unsigned>(p4_nano_psram_read_control::kTaskStackWords),
        p4_nano_psram_read_control::stack_internal() ? 1U : 0U,
        p4_nano_psram_read_control::tcb_internal() ? 1U : 0U,
        p4_nano_psram_read_control::state_internal() ? 1U : 0U,
        esp_ptr_in_iram(reinterpret_cast<const void *>(
            &p4_nano_psram_read_control::read_sweep)) ? 1U : 0U,
        esp_ptr_executable(reinterpret_cast<const void *>(
            &p4_nano_psram_read_control::read_sweep)) ? 1U : 0U,
        layout_validity ? "PASS" : "FAIL",
        p4_nano_psram_read_control::stack_bytes(),
        p4_nano_psram_read_control::tcb_bytes(),
        p4_nano_psram_read_control::state_bytes(),
        p4_nano_psram_read_control::static_bytes(),
        calibration.calibration_sweeps, calibration.calibration_elapsed_us,
        calibration.sweeps_per_relief, calibration.target_interval_us);
    control_ready = !failed;
    if (!failed && !layout_validity) {
        failed = true;
        if (!p4_nano_psram_read_control::stop()) {
            failed = true;
        }
        control_ready = false;
    }
    if (!failed && !p4_nano_psram_read_control::begin()) {
        failed = true;
        (void)p4_nano_psram_read_control::stop();
        control_ready = false;
    } else if (!failed) {
        control_started = true;
        state->p8_control_start_us = static_cast<std::uint64_t>(
            esp_timer_get_time());
        if (!benchmark_run_isolated_samples(state, false, true)) {
            failed = true;
        }
        state->p8_control_end_us = static_cast<std::uint64_t>(
            esp_timer_get_time());
    }
    if ((control_started || control_ready) &&
        !p4_nano_psram_read_control::stop()) {
        failed = true;
    }

    const auto &health = p4_nano_psram_read_control::health();
    const std::uint32_t source_crc_after =
        state->isolated_source_held && state->isolated_source_view.ptr != nullptr
            ? p4_nano_display::crc32(state->isolated_source_view.ptr,
                                      np2video_golden_visible_bytes)
            : 0U;
    state->p8_source_crc_after = source_crc_after;
    const bool source_immutable = state->isolated_source_crc_captured &&
                                  source_crc_after == state->isolated_source_crc;
    const bool a_native_stable =
        state->isolated_first_native_crc_captured &&
        state->isolated_final_native_crc_captured &&
        state->isolated_first_native_crc == state->isolated_final_native_crc;
    const bool b_native_stable =
        state->p8_first_native_crc_captured &&
        state->p8_final_native_crc_captured &&
        state->p8_first_native_crc == state->p8_final_native_crc;
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->isolated_cooperate_calls_at_end &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    const bool control_wall_valid = state->p8_control_end_us >
                                    state->p8_control_start_us;
    const bool reader_active_wall_valid =
        health.active_start_us != 0U &&
        health.active_end_us > health.active_start_us;
    const std::uint64_t reader_active_wall_us = reader_active_wall_valid
                                                    ? health.active_end_us -
                                                          health.active_start_us
                                                    : 0U;
    // This is requested 4-MiB sweep payload over the CPU1 reader lifetime,
    // not a hardware bus-bandwidth counter.
    double payload_mib_s = 0.0;
    const bool payload_rate_valid =
        p4_nano_psram_read_control::payload_mib_per_second(
            health.total_bytes, reader_active_wall_us, &payload_mib_s);
    if (state->publish_failed.load(std::memory_order_acquire) ||
        !state->p8_buffer_initialized || !state->p8_msync_pass ||
        !source_immutable || !a_native_stable || !b_native_stable ||
        !pause_stable || state->isolated_correctness_fail != 0U ||
        state->p8_correctness_fail != 0U ||
        state->p8_transforms_completed != kBenchmarkTotalTransforms ||
        state->p8_transform_stored != kBenchmarkMeasuredTransforms ||
        state->p8_cache_stored != kBenchmarkMeasuredTransforms ||
        state->p8_service_stored != kBenchmarkMeasuredTransforms ||
        state->isolated_cache_sync_failures != 0U ||
        state->p8_cache_sync_failures != 0U || !layout_validity ||
        !health.ready || !health.clean_stop || !health.checksum_valid ||
        health.sweeps == 0U || health.total_bytes == 0U ||
        health.relief_count == 0U ||
        health.last_sweep_checksum != state->p8_expected_checksum ||
        !control_wall_valid || !reader_active_wall_valid ||
        !payload_rate_valid) {
        failed = true;
    }
    benchmark_print_fixed_metric("psram_read_control_A_transform_only_us",
                                 state->isolated_transform_samples,
                                 state->isolated_transform_stored);
    benchmark_print_fixed_metric("psram_read_control_A_cache_sync_us",
                                 state->isolated_cache_samples,
                                 state->isolated_cache_stored);
    benchmark_print_fixed_metric("psram_read_control_A_consumer_service_us",
                                 state->isolated_service_samples,
                                 state->isolated_service_stored);
    benchmark_print_fixed_metric("psram_read_control_B_transform_only_us",
                                 state->p8_transform_samples,
                                 state->p8_transform_stored);
    benchmark_print_fixed_metric("psram_read_control_B_cache_sync_us",
                                 state->p8_cache_samples, state->p8_cache_stored);
    benchmark_print_fixed_metric("psram_read_control_B_consumer_service_us",
                                 state->p8_service_samples,
                                 state->p8_service_stored);
    const std::uint64_t control_wall_us = control_wall_valid
                                              ? state->p8_control_end_us -
                                                    state->p8_control_start_us
                                              : 0U;
    std::printf(
        "P4_NANO_PSRAM_READ_CONTROL_HEALTH sweeps=%" PRIu64
        " bytes_per_sweep=%zu total_bytes=%" PRIu64
        " relief_count=%" PRIu32 " last_sweep_checksum=0x%08" PRIx32
        " expected_checksum=0x%08" PRIx32
        " checksum_validity=%u stack_high_water_words=%" PRIu32
        " ready=%u clean_stop=%u B_phase_wall_us=%" PRIu64
        " reader_active_wall_us=%" PRIu64
        " approximate_payload_mib_s=%.3f\n",
        health.sweeps, p4_nano_psram_read_control::kBufferBytes,
        health.total_bytes, health.relief_count, health.last_sweep_checksum,
        state->p8_expected_checksum, health.checksum_valid ? 1U : 0U,
        health.stack_high_water_words, health.ready ? 1U : 0U,
        health.clean_stop ? 1U : 0U, control_wall_us, reader_active_wall_us,
        payload_mib_s);
    std::printf(
        "P4_NANO_PSRAM_READ_CONTROL_CORRECTNESS A_source_crc=0x%08" PRIx32
        " B_source_crc=0x%08" PRIx32 " A_native_crc=0x%08" PRIx32
        " B_native_crc=0x%08" PRIx32 " result=%s\n",
        state->isolated_source_crc, source_crc_after,
        state->isolated_final_native_crc, state->p8_final_native_crc,
        (!failed && source_immutable && a_native_stable && b_native_stable)
            ? "PASS"
            : "FAIL");
    std::printf(
        "P4_NANO_PSRAM_READ_CONTROL_SAMPLE_COUNTS A_transform=%zu "
        "A_cache_sync=%zu A_service=%zu B_transform=%zu B_cache_sync=%zu "
        "B_service=%zu warmup=%u measured=%u final_validation=%u\n",
        state->isolated_transform_stored, state->isolated_cache_stored,
        state->isolated_service_stored, state->p8_transform_stored,
        state->p8_cache_stored, state->p8_service_stored,
        static_cast<unsigned>(kBenchmarkWarmupTransforms),
        static_cast<unsigned>(kBenchmarkMeasuredTransforms),
        static_cast<unsigned>(kBenchmarkFinalValidationTransforms));
    benchmark_print_vsync_stats(state->display);

    state->stop_requested.store(true, std::memory_order_release);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    state->backlight_off_failed =
        p4_nano_board::display_backlight_set(0U) != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions || !scheduling_contract) {
        failed = true;
    }
    std::printf("P4_NANO_PSRAM_READ_CONTROL_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    benchmark_p8_free_buffer(state);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#endif
#endif

bool benchmark_stop_requested(void *context)
{
    auto *state = static_cast<BenchmarkState *>(context);
    return state != nullptr && state->stop_requested.load(std::memory_order_acquire);
}

void benchmark_release(BenchmarkState *state, np2_presentation_token *token)
{
    if (state == nullptr || token == nullptr || token->lease == 0U) {
        return;
    }
    if (np2_presentation_release(&state->publisher, token) == NP2_PRESENTATION_OK) {
        ++state->releases;
    } else {
        state->publish_failed.store(true, std::memory_order_release);
    }
}

bool benchmark_enable_backlight(BenchmarkState *state)
{
    if (state == nullptr || state->visible) {
        return true;
    }
    if (p4_nano_board::display_backlight_set(
            p4_nano_board::kBacklightConservative) != ESP_OK) {
        state->backlight_enable_failed = true;
        return false;
    }
    state->visible = true;
    state->visible_start_us =
        static_cast<std::uint64_t>(esp_timer_get_time());
    return true;
}

int benchmark_consume_one(BenchmarkState *state)
{
    np2_presentation_frame_view view{};
    np2_presentation_token token{};
    const np2_presentation_status acquire = np2_presentation_acquire(
        &state->publisher, &view, &token);
    if (acquire == NP2_PRESENTATION_NO_FRAME) {
        return 0;
    }
    if (acquire != NP2_PRESENTATION_OK) {
        state->publish_failed.store(true, std::memory_order_release);
        return -1;
    }
    const std::uint64_t service_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    if (!state->scene_ready.load(std::memory_order_acquire)) {
        benchmark_release(state, &token);
        return 0;
    }
    const std::uint32_t scene_generation =
        state->scene_generation.load(std::memory_order_relaxed);
    const std::uint32_t scene_update_sequence =
        state->scene_update_sequence.load(std::memory_order_relaxed);
    if (view.source_generation != scene_generation) {
        benchmark_release(state, &token);
        return -1;
    }
    if (view.source_update_sequence <= scene_update_sequence) {
        benchmark_release(state, &token);
        return 0;
    }
    ++state->acquisitions;
    if (state->acquisitions == 1U) {
        std::printf("P4_NANO_BENCHMARK_FIRST_ACQUIRE=1\n");
    }
    const std::uint64_t acquired_at = service_start;
    const std::uint64_t sequence = view.published_sequence;
    BenchmarkTimestamp &entry =
        state->timestamp_ring[sequence % kBenchmarkTimestampRingSize];
    const std::uint64_t tag_before = entry.sequence.load(std::memory_order_acquire);
    const std::uint64_t submit_start =
        entry.submit_start_us.load(std::memory_order_relaxed);
    const std::uint64_t tag_after = entry.sequence.load(std::memory_order_acquire);
    if (tag_before == sequence && tag_after == sequence && submit_start <= acquired_at) {
        ++state->latency_valid;
        benchmark_add_metric(acquired_at - submit_start, state->latency_samples,
                             state->latency_stored, state->latency_count,
                             state->latency_total_us, state->latency_min_us,
                             state->latency_max_us);
    } else {
        ++state->consumer_latency_lookup_missing;
    }
    if (!benchmark_validate_frame(view)) {
        benchmark_release(state, &token);
        return -1;
    }
    if (!state->source_generation_initialized) {
        state->source_generation = view.source_generation;
        state->source_generation_initialized = true;
    } else if (view.source_generation != state->source_generation) {
        benchmark_release(state, &token);
        return -1;
    }
    if (state->source_sequence_initialized &&
        view.source_update_sequence <= state->last_source_update_sequence) {
        benchmark_release(state, &token);
        return -1;
    }
    state->last_source_update_sequence = view.source_update_sequence;
    state->source_sequence_initialized = true;
    if (state->transforms_completed >= kBenchmarkTotalTransforms) {
        benchmark_release(state, &token);
        return 1;
    }

    const std::uint32_t transform_index = state->transforms_completed;
    const bool correctness_sample = transform_index == 0U ||
                                    transform_index + 1U == kBenchmarkTotalTransforms;
    std::uint32_t source_crc_before = 0U;
    if (correctness_sample) {
        source_crc_before = p4_nano_display::crc32(
            view.ptr, np2video_golden_visible_bytes);
    }
    ++state->transforms_started;
    const auto source = std::span<const std::uint16_t>(
        reinterpret_cast<const std::uint16_t *>(view.ptr),
        p4_nano_display::kTransformSourcePixelCount);
    const auto destination = std::span<std::uint16_t>(
        state->display.framebuffer,
        p4_nano_display::kTransformDestinationPixelCount);
    const std::uint64_t transform_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const bool transformed = p4_nano_display::transform_to_native(
        source, destination, p4_nano_display::QuarterTurn::CounterClockwise);
    const std::uint64_t transform_end =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint64_t transform_us = transform_end - transform_start;
    if (!transformed) {
        benchmark_release(state, &token);
        return -1;
    }
    if (transform_index == 0U) {
        std::printf("P4_NANO_BENCHMARK_FIRST_TRANSFORM_COMPLETE=1\n");
    }
    if (correctness_sample) {
        const std::uint32_t source_crc_after = p4_nano_display::crc32(
            view.ptr, np2video_golden_visible_bytes);
        if (source_crc_before == source_crc_after) {
            ++state->correctness_pass;
        } else {
            ++state->correctness_fail;
        }
        state->immutable_pass = state->correctness_fail == 0U;
        if (transform_index == 0U) {
            state->first_source_crc = source_crc_before;
            state->first_source_crc_captured = true;
        } else {
            state->final_source_crc = source_crc_before;
            state->final_source_crc_captured = true;
        }
    }
    const std::uint64_t cache_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const esp_err_t sync_result =
        p4_nano_display::display_session_sync_framebuffer(&state->display);
    const std::uint64_t cache_sync_end =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint64_t cache_us = cache_sync_end - cache_start;
    if (sync_result != ESP_OK) {
        ++state->cache_sync_failures;
        benchmark_release(state, &token);
        return -1;
    }
    ++state->cache_sync_success;
    ++state->native_framebuffer_updates;
    if (transform_index == 0U) {
        std::printf("P4_NANO_BENCHMARK_FIRST_CACHE_SYNC_COMPLETE=1\n");
    }
    if (correctness_sample) {
        const std::uint32_t native_crc = p4_nano_display::crc32(
            reinterpret_cast<const std::uint8_t *>(state->display.framebuffer),
            p4_nano_display::kNativeFramebufferBytes);
        if (transform_index == 0U) {
            state->first_native_crc = native_crc;
            state->first_native_crc_captured = true;
        } else {
            state->final_native_crc = native_crc;
            state->final_native_crc_captured = true;
        }
    }
    if (transform_index == 0U && !benchmark_enable_backlight(state)) {
        benchmark_release(state, &token);
        return -1;
    }
    if (transform_index == 0U && state->visible) {
        std::printf("P4_NANO_BENCHMARK_FIRST_VISIBLE=1\n");
    }
    benchmark_release(state, &token);
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
    const p4_nano_overlap::TransformInterval transform_interval{
        .acquire_us = acquired_at,
        .transform_start_us = transform_start,
        .transform_end_us = transform_end,
        .cache_sync_end_us = cache_sync_end,
        .release_us = 0U,
        .source_update_sequence = view.source_update_sequence,
        .published_sequence = view.published_sequence,
        .transform_index = transform_index,
        .measured = benchmark_is_measured_sample(transform_index),
    };
    (void)p4_nano_overlap::append_bounded(
        state->overlap_transform_trace,
        state->overlap_transform_trace_stored, transform_interval,
        state->overlap_transform_trace_overflow);
#endif
    const std::uint64_t service_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - service_start;
    if (benchmark_is_measured_sample(transform_index)) {
        /* Only indices 8..135 may write the exactly 128-entry performance
         * arrays.  The final validation transform (index 136) is excluded. */
        const std::size_t measured_index =
            transform_index - kBenchmarkWarmupTransforms;
        state->transform_samples[measured_index] = transform_us;
        state->cache_samples[measured_index] = cache_us;
        state->service_samples[measured_index] = service_us;
        state->transform_stored = measured_index + 1U;
        state->cache_stored = measured_index + 1U;
        state->service_stored = measured_index + 1U;
    }
    ++state->transforms_completed;
    state->final_published_sequence = view.published_sequence;
    state->final_surface_update_sequence = view.source_update_sequence;
    if (state->transforms_completed == kBenchmarkTotalTransforms) {
        state->stop_requested.store(true, std::memory_order_release);
    }
    /* Keep scheduler cooperation outside all isolated wall-clock intervals,
     * sample storage, release semantics, and presentation backpressure. */
    ++state->consumer_cooperate_calls;
    np2_host_taskmng_cooperate();
    return 1;
}

void benchmark_hold_visible(BenchmarkState *state)
{
    if (state == nullptr || !state->visible) {
        return;
    }
    for (;;) {
        const std::uint64_t now =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const std::uint64_t elapsed = now - state->visible_start_us;
        if (elapsed >= kVisibleHoldUs) {
            state->visible_elapsed_us = elapsed;
            return;
        }
        const std::uint64_t remaining_us = kVisibleHoldUs - elapsed;
        std::uint64_t delay_ms = (remaining_us + 999U) / 1000U;
        if (delay_ms == 0U) {
            delay_ms = 1U;
        }
        vTaskDelay(pdMS_TO_TICKS(static_cast<std::uint32_t>(delay_ms)));
    }
}

std::uint32_t benchmark_counter_delta(std::uint32_t final_value,
                                       std::uint32_t baseline)
{
    return final_value >= baseline ? final_value - baseline : 0U;
}

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)

static_assert(p4_nano_psram_read_control::kExpectedSweepChecksum != 0U);

void benchmark_p8_free_buffer(BenchmarkState *state)
{
    if (state == nullptr) {
        return;
    }
    heap_caps_free(state->p8_buffer);
    state->p8_buffer = nullptr;
    state->p8_buffer_initialized = false;
    state->p8_msync_pass = false;
}

bool benchmark_p8_prepare_buffer(BenchmarkState *state)
{
    if (state == nullptr || !state->isolated_source_held ||
        !state->isolated_source_crc_captured) {
        return false;
    }
    state->p8_free_spiram_before = static_cast<std::uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    state->p8_largest_spiram_before = static_cast<std::uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    state->p8_buffer = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        p4_nano_psram_read_control::kAlignmentBytes,
        p4_nano_psram_read_control::kBufferBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state->p8_buffer == nullptr) {
        std::printf("P4_NANO_PSRAM_READ_CONTROL_RESULT=FAIL reason=buffer_alloc "
                    "free_spiram_before=%" PRIu32
                    " largest_spiram_before=%" PRIu32 "\n",
                    state->p8_free_spiram_before,
                    state->p8_largest_spiram_before);
        return false;
    }
    const bool aligned =
        reinterpret_cast<std::uintptr_t>(state->p8_buffer) %
            p4_nano_psram_read_control::kAlignmentBytes == 0U;
    const bool external = esp_ptr_external_ram(state->p8_buffer);
    const bool disjoint =
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->isolated_source_view.ptr,
                        np2video_golden_visible_bytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->slots[0].ptr, kSlotBytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->slots[1].ptr, kSlotBytes) &&
        !ranges_overlap(state->p8_buffer,
                        p4_nano_psram_read_control::kBufferBytes,
                        state->display.framebuffer,
                        p4_nano_display::kNativeFramebufferBytes);
    if (!aligned || !external || !disjoint) {
        std::printf("P4_NANO_PSRAM_READ_CONTROL_RESULT=FAIL reason=buffer_layout "
                    "external=%u aligned=%u disjoint=%u bytes=%zu\n",
                    external ? 1U : 0U, aligned ? 1U : 0U,
                    disjoint ? 1U : 0U,
                    p4_nano_psram_read_control::kBufferBytes);
        benchmark_p8_free_buffer(state);
        return false;
    }

    auto *words = reinterpret_cast<std::uint32_t *>(state->p8_buffer);
    std::uint32_t lanes[8] = {
        p4_nano_psram_read_control::initial_lane(0U),
        p4_nano_psram_read_control::initial_lane(1U),
        p4_nano_psram_read_control::initial_lane(2U),
        p4_nano_psram_read_control::initial_lane(3U),
        p4_nano_psram_read_control::initial_lane(4U),
        p4_nano_psram_read_control::initial_lane(5U),
        p4_nano_psram_read_control::initial_lane(6U),
        p4_nano_psram_read_control::initial_lane(7U),
    };
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(
                     p4_nano_psram_read_control::kWordsPerSweep);
         index += 8U) {
        for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
            const std::uint32_t word_index = index + lane;
            const std::uint32_t value =
                p4_nano_psram_read_control::pattern_word(word_index);
            words[word_index] = value;
            lanes[lane] = p4_nano_psram_read_control::fold_lane(
                lanes[lane], value, word_index);
        }
    }
    state->p8_expected_checksum =
        lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3] ^ lanes[4] ^ lanes[5] ^
        lanes[6] ^ lanes[7];
    if (state->p8_expected_checksum !=
        p4_nano_psram_read_control::kExpectedSweepChecksum) {
        benchmark_p8_free_buffer(state);
        return false;
    }
    state->p8_free_spiram_after = static_cast<std::uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    state->p8_largest_spiram_after = static_cast<std::uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    const esp_err_t sync_result = esp_cache_msync(
        state->p8_buffer, p4_nano_psram_read_control::kBufferBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    state->p8_msync_pass = sync_result == ESP_OK;
    state->p8_buffer_initialized = state->p8_msync_pass;
    std::printf("P4_NANO_PSRAM_READ_CONTROL_CONFIG buffer_bytes=%zu "
                "alignment=%zu external=%u aligned=%u disjoint=%u "
                "free_spiram_before=%" PRIu32
                " largest_spiram_before=%" PRIu32
                " free_spiram_after=%" PRIu32
                " largest_spiram_after=%" PRIu32
                " expected_sweep_checksum=0x%08" PRIx32
                " msync_result=%s msync_flags=C2M|DATA|INVALIDATE\n",
                p4_nano_psram_read_control::kBufferBytes,
                p4_nano_psram_read_control::kAlignmentBytes,
                external ? 1U : 0U, aligned ? 1U : 0U, disjoint ? 1U : 0U,
                state->p8_free_spiram_before, state->p8_largest_spiram_before,
                state->p8_free_spiram_after, state->p8_largest_spiram_after,
                state->p8_expected_checksum, esp_err_to_name(sync_result));
    if (!state->p8_msync_pass) {
        benchmark_p8_free_buffer(state);
        return false;
    }
    return true;
}

#endif

#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)

constexpr std::size_t kP1BufferBytes = 4U * 1024U * 1024U;
constexpr std::size_t kP1AlignmentBytes = 64U;
constexpr int kP1CacheSyncFlags = ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                   ESP_CACHE_MSYNC_FLAG_UNALIGNED;

#if defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_READ)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::Read;
#elif defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_WRITE16)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::Write16;
#elif defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_WRITE32)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::Write32;
#elif defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_MEMCPY)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::Memcpy;
#elif defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_ROW_COPY)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::RowMemcpy;
#elif defined(P4_NANO_PSRAM_BANDWIDTH_OPERATION_PROXY)
constexpr auto kP1Operation = p4_nano_psram_bandwidth::Operation::Proxy;
#else
#error "P1 PSRAM bandwidth operation is not selected"
#endif

constexpr std::uint32_t p1_pattern(std::uint32_t index)
{
    return 0x10203040U ^ (index * 0x9e3779b9U);
}

void benchmark_p1_free_buffers(BenchmarkState *state)
{
    if (state == nullptr) {
        return;
    }
    heap_caps_free(state->p1_source);
    heap_caps_free(state->p1_destination);
    state->p1_source = nullptr;
    state->p1_destination = nullptr;
    state->p1_buffers_initialized = false;
}

bool benchmark_p1_prepare_buffers(BenchmarkState *state)
{
    if (state == nullptr) {
        return false;
    }
    state->p1_source = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kP1AlignmentBytes, kP1BufferBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    state->p1_destination = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kP1AlignmentBytes, kP1BufferBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state->p1_source == nullptr || state->p1_destination == nullptr) {
        std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=buffer_alloc "
                    "source=%p destination=%p free_spiram=%" PRIu32
                    " largest_spiram=%" PRIu32 "\n",
                    static_cast<void *>(state->p1_source),
                    static_cast<void *>(state->p1_destination),
                    static_cast<std::uint32_t>(heap_caps_get_free_size(
                        MALLOC_CAP_SPIRAM)),
                    static_cast<std::uint32_t>(heap_caps_get_largest_free_block(
                        MALLOC_CAP_SPIRAM)));
        benchmark_p1_free_buffers(state);
        return false;
    }
    const bool layout_valid = esp_ptr_external_ram(state->p1_source) &&
                               esp_ptr_external_ram(state->p1_destination) &&
                               (reinterpret_cast<std::uintptr_t>(
                                    state->p1_source) % kP1AlignmentBytes) == 0U &&
                               (reinterpret_cast<std::uintptr_t>(
                                    state->p1_destination) % kP1AlignmentBytes) == 0U &&
                               !ranges_overlap(state->p1_source, kP1BufferBytes,
                                               state->p1_destination,
                                               kP1BufferBytes);
    if (!layout_valid) {
        std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=buffer_layout "
                    "source_external=%d destination_external=%d "
                    "source_mod64=%" PRIuPTR " destination_mod64=%" PRIuPTR
                    "\n",
                    esp_ptr_external_ram(state->p1_source) ? 1 : 0,
                    esp_ptr_external_ram(state->p1_destination) ? 1 : 0,
                    reinterpret_cast<std::uintptr_t>(state->p1_source) %
                        kP1AlignmentBytes,
                    reinterpret_cast<std::uintptr_t>(state->p1_destination) %
                        kP1AlignmentBytes);
        benchmark_p1_free_buffers(state);
        return false;
    }
    for (std::size_t index = 0; index < kP1BufferBytes; ++index) {
        state->p1_source[index] = static_cast<std::uint8_t>(
            (index * 37U + 11U) & 0xffU);
    }
    std::memset(state->p1_destination, 0, kP1BufferBytes);
    state->p1_expected_source_crc = p4_nano_display::crc32(
        state->p1_source, kP1BufferBytes);
    if (kP1Operation == p4_nano_psram_bandwidth::Operation::Read) {
        state->p1_expected_read_guard =
            p4_nano_psram_bandwidth::run_kernel(
                kP1Operation, state->p1_source, state->p1_destination, 0U);
    }
    state->p1_buffers_initialized = true;
    std::printf("P4_NANO_PSRAM_BANDWIDTH_BUFFERS result=PASS source=%p "
                "destination=%p bytes_each=%zu alignment=%zu external=1 "
                "disjoint=1\n",
                static_cast<void *>(state->p1_source),
                static_cast<void *>(state->p1_destination), kP1BufferBytes,
                kP1AlignmentBytes);
    report_memory("after_psram_bandwidth_buffers");
    return true;
}

bool benchmark_p1_validate(BenchmarkState *state)
{
    if (state == nullptr || !state->p1_buffers_initialized) {
        return false;
    }
    bool valid = true;
    switch (kP1Operation) {
    case p4_nano_psram_bandwidth::Operation::Read:
        for (std::size_t index = 0; index < state->p1_raw_stored; ++index) {
            valid = valid &&
                    state->p1_guards[index] == state->p1_expected_read_guard;
        }
        state->p1_validation_crc = state->p1_expected_source_crc;
        break;
    case p4_nano_psram_bandwidth::Operation::Write16: {
        const auto *output = reinterpret_cast<const std::uint16_t *>(
            state->p1_destination);
        const std::uint32_t pattern = p1_pattern(kBenchmarkTotalTransforms - 1U);
        for (std::size_t index = 0; index < kP1BufferBytes / sizeof(std::uint16_t);
             ++index) {
            if (output[index] != static_cast<std::uint16_t>(
                                    pattern ^ static_cast<std::uint32_t>(index))) {
                valid = false;
                break;
            }
        }
        state->p1_validation_crc = p4_nano_display::crc32(
            state->p1_destination, kP1BufferBytes);
        break;
    }
    case p4_nano_psram_bandwidth::Operation::Write32: {
        const auto *output = reinterpret_cast<const std::uint32_t *>(
            state->p1_destination);
        const std::uint32_t pattern = p1_pattern(kBenchmarkTotalTransforms - 1U);
        for (std::size_t index = 0; index < kP1BufferBytes / sizeof(std::uint32_t);
             ++index) {
            const std::uint32_t pixel =
                pattern ^ static_cast<std::uint32_t>(index);
            if (output[index] != (pixel | (pixel << 16U))) {
                valid = false;
                break;
            }
        }
        state->p1_validation_crc = p4_nano_display::crc32(
            state->p1_destination, kP1BufferBytes);
        break;
    }
    case p4_nano_psram_bandwidth::Operation::Memcpy:
        valid = std::memcmp(state->p1_source, state->p1_destination,
                            kP1BufferBytes) == 0;
        state->p1_validation_crc = p4_nano_display::crc32(
            state->p1_destination, kP1BufferBytes);
        break;
    case p4_nano_psram_bandwidth::Operation::RowMemcpy:
        valid = std::memcmp(state->p1_source, state->p1_destination,
                            512'000U) == 0;
        state->p1_validation_crc = p4_nano_display::crc32(
            state->p1_destination, 512'000U);
        break;
    case p4_nano_psram_bandwidth::Operation::Proxy: {
        const auto *source = reinterpret_cast<const std::uint16_t *>(
            state->p1_source);
        const auto *destination = reinterpret_cast<const std::uint16_t *>(
            state->p1_destination);
        constexpr std::size_t source_pixels = 512'000U / sizeof(std::uint16_t);
        for (std::size_t index = 0; index < source_pixels; ++index) {
            const std::uint16_t pixel = source[index];
            const std::size_t output_index = index * 4U;
            if (destination[output_index + 0U] != pixel ||
                destination[output_index + 1U] != pixel ||
                destination[output_index + 2U] != pixel ||
                destination[output_index + 3U] != pixel) {
                valid = false;
                break;
            }
        }
        state->p1_validation_crc = p4_nano_display::crc32(
            state->p1_destination, 2'048'000U);
        break;
    }
    }
    state->p1_validation_pass = valid;
    return valid;
}

void benchmark_p1_print_metric(const char *metric,
                               std::array<std::uint64_t,
                                          kBenchmarkMeasuredTransforms> &samples,
                               std::size_t stored, std::size_t read_bytes,
                               std::size_t write_bytes)
{
    if (stored == 0U) {
        std::printf("P4_NANO_PSRAM_TIMING metric=%s count=0\n", metric);
        return;
    }
    std::uint64_t total = 0U;
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0U;
    for (std::size_t index = 0; index < stored; ++index) {
        total += samples[index];
        minimum = std::min(minimum, samples[index]);
        maximum = std::max(maximum, samples[index]);
    }
    const std::uint64_t average = total / stored;
    const std::uint64_t p50 = benchmark_percentile(samples, stored, 50U);
    const std::uint64_t p95 = benchmark_percentile(samples, stored, 95U);
    const std::uint64_t p99 = benchmark_percentile(samples, stored, 99U);
    const std::size_t payload = read_bytes + write_bytes;
    const double mib_per_second =
        average == 0U
            ? 0.0
            : static_cast<double>(payload) * 1'000'000.0 /
                  static_cast<double>(average) / 1'048'576.0;
    std::printf("P4_NANO_PSRAM_TIMING metric=%s count=%zu stored=%zu "
                "read_bytes=%zu write_bytes=%zu min_us=%" PRIu64
                " max_us=%" PRIu64 " average_us=%" PRIu64
                " p50_us=%" PRIu64 " p95_us=%" PRIu64
                " p99_us=%" PRIu64 " payload_mib_s=%.3f\n",
                metric, stored, stored, read_bytes, write_bytes, minimum,
                maximum, average, p50, p95, p99, mib_per_second);
}

bool benchmark_p1_run_samples(BenchmarkState *state)
{
    if (state == nullptr || !state->p1_buffers_initialized ||
        !state->scene_ready.load(std::memory_order_acquire)) {
        return false;
    }
    state->p1_phase_submit_start = state->p1_publish_progress.load(
        std::memory_order_acquire);
    state->p1_phase_coalesced_start =
        np2_presentation_coalesced_count(&state->publisher);
    state->p1_phase_dropped_start =
        np2_presentation_dropped_count(&state->publisher);
    bool valid = true;
    for (std::uint32_t index = 0U; index < kBenchmarkTotalTransforms;
         ++index) {
        if (state->producer_done.load(std::memory_order_acquire)) {
            state->p1_producer_ended_during_phase = true;
            valid = false;
            break;
        }
        const std::uint32_t pattern = p1_pattern(index);
        const std::uint64_t raw_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const std::uint32_t guard = p4_nano_psram_bandwidth::run_kernel(
            kP1Operation, state->p1_source, state->p1_destination, pattern);
        const std::uint64_t raw_us = static_cast<std::uint64_t>(
            esp_timer_get_time()) - raw_start;
        std::uint64_t cache_us = 0U;
        if (p4_nano_psram_bandwidth::write_bytes(kP1Operation) != 0U) {
            const std::uint64_t cache_start =
                static_cast<std::uint64_t>(esp_timer_get_time());
            const esp_err_t sync_result = esp_cache_msync(
                state->p1_destination,
                p4_nano_psram_bandwidth::write_bytes(kP1Operation),
                kP1CacheSyncFlags);
            cache_us = static_cast<std::uint64_t>(esp_timer_get_time()) -
                       cache_start;
            if (sync_result != ESP_OK) {
                valid = false;
                break;
            }
        }
        if (benchmark_is_measured_sample(index)) {
            const std::size_t measured_index =
                index - kBenchmarkWarmupTransforms;
            state->p1_raw_samples[measured_index] = raw_us;
            state->p1_guards[measured_index] = guard;
            state->p1_raw_stored = measured_index + 1U;
            if (p4_nano_psram_bandwidth::write_bytes(kP1Operation) != 0U) {
                state->p1_cache_samples[measured_index] = cache_us;
                state->p1_cache_stored = measured_index + 1U;
            }
        }
        np2_host_taskmng_cooperate();
    }
    if (valid && state->p1_raw_stored == kBenchmarkMeasuredTransforms) {
        const std::uint32_t final_pattern =
            p1_pattern(kBenchmarkTotalTransforms - 1U);
        (void)p4_nano_psram_bandwidth::run_kernel(
            kP1Operation, state->p1_source, state->p1_destination,
            final_pattern);
        state->p1_final_pattern = final_pattern;
        valid = benchmark_p1_validate(state);
    }
    state->p1_phase_submit_end = state->p1_publish_progress.load(
        std::memory_order_acquire);
    state->p1_phase_coalesced_end =
        np2_presentation_coalesced_count(&state->publisher);
    state->p1_phase_dropped_end =
        np2_presentation_dropped_count(&state->publisher);
    return valid;
}

void benchmark_p1_print_report(BenchmarkState *state, const char *condition,
                               bool valid)
{
    const std::size_t read_bytes =
        p4_nano_psram_bandwidth::read_bytes(kP1Operation);
    const std::size_t write_bytes =
        p4_nano_psram_bandwidth::write_bytes(kP1Operation);
    const std::uint32_t submit_delta = benchmark_counter_delta(
        state->p1_phase_submit_end, state->p1_phase_submit_start);
    const std::uint32_t coalesced_delta = benchmark_counter_delta(
        state->p1_phase_coalesced_end, state->p1_phase_coalesced_start);
    const std::uint32_t dropped_delta = benchmark_counter_delta(
        state->p1_phase_dropped_end, state->p1_phase_dropped_start);
    benchmark_p1_print_metric("raw", state->p1_raw_samples,
                              state->p1_raw_stored, read_bytes, write_bytes);
    if (state->p1_cache_stored != 0U) {
        benchmark_p1_print_metric("cache_sync", state->p1_cache_samples,
                                  state->p1_cache_stored, 0U, write_bytes);
    }
    std::printf("P4_NANO_PSRAM_BANDWIDTH operation=%s condition=%s "
                "read_bytes=%zu write_bytes=%zu result=%s "
                "validation=%s validation_crc=0x%08" PRIx32
                " producer_done_at_end=%d producer_ended_during_phase=%d"
                " publish_delta=%" PRIu32
                " coalesced_delta=%" PRIu32 " dropped_delta=%" PRIu32
                " pccore_exec_count=%" PRIu64 "\n",
                p4_nano_psram_bandwidth::operation_name(kP1Operation),
                condition, read_bytes, write_bytes, valid ? "PASS" : "FAIL",
                state->p1_validation_pass ? "PASS" : "FAIL",
                state->p1_validation_crc, state->producer_done.load(
                    std::memory_order_acquire) ? 1 : 0,
                state->p1_producer_ended_during_phase ? 1 : 0, submit_delta,
                coalesced_delta, dropped_delta,
                state->producer_result.pccore_exec_count);
    benchmark_print_vsync_stats(state->display);
}

#endif

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
 #if defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE)
struct Exact2xStats final {
    std::array<std::uint64_t, p4_nano_display::kExact2xMeasuredSamples>
        kernel{};
    std::array<std::uint64_t, p4_nano_display::kExact2xMeasuredSamples>
        cache{};
    std::array<std::uint64_t, p4_nano_display::kExact2xMeasuredSamples>
        service{};
    std::size_t stored = 0U;
    std::uint32_t source_crc_before = 0U;
    std::uint32_t source_crc_after = 0U;
    std::uint32_t output_crc = 0U;
    bool source_immutable = false;
    bool byte_exact = false;
    bool final_validation_ok = false;
    bool cache_ok = true;
};

static Exact2xStats exact2x_scalar_stats;
static Exact2xStats exact2x_pie_stats;

constexpr std::size_t kExact2xCooperateInterval = 64U;
constexpr TickType_t kExact2xCooperateDelayTicks = 1;
static_assert(kExact2xCooperateDelayTicks == 1,
              "exact2x cooperation must block for exactly one tick");

using Exact2xKernel = bool (*)(const std::uint16_t *, std::uint16_t *) noexcept;

void exact2x_reset_stats(Exact2xStats *stats) noexcept
{
    if (stats == nullptr) {
        return;
    }
    // Keep the bounded report arrays in static storage. In particular, avoid
    // materializing an aggregate temporary on this task's constrained stack.
    stats->kernel.fill(0U);
    stats->cache.fill(0U);
    stats->service.fill(0U);
    stats->stored = 0U;
    stats->source_crc_before = 0U;
    stats->source_crc_after = 0U;
    stats->output_crc = 0U;
    stats->source_immutable = false;
    stats->byte_exact = false;
    stats->final_validation_ok = false;
    stats->cache_ok = true;
}

void exact2x_add_sample(Exact2xStats *stats, std::uint64_t kernel,
                        std::uint64_t cache, std::uint64_t service,
                        std::size_t index) noexcept
{
    if (stats == nullptr || index >= stats->kernel.size()) {
        return;
    }
    stats->kernel[index] = kernel;
    stats->cache[index] = cache;
    stats->service[index] = service;
    stats->stored = index + 1U;
}

template <std::size_t N>
void exact2x_print_metric(const char *phase, const char *metric,
                          std::array<std::uint64_t, N> &samples,
                          std::size_t count)
{
    // Statistics are emitted only after sampling; chronological order is no
    // longer needed, so sort the static sample storage in place.  Keeping the
    // 128-sample arrays out of this task's automatic frame avoids a stack
    // copy of roughly 1 KiB before printf/vfprintf runs.
    std::sort(samples.begin(), samples.begin() + count);
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        total += samples[index];
    }
    const auto percentile = [&samples, count](std::size_t rank) {
        if (count == 0U) {
            return std::uint64_t{0U};
        }
        const std::size_t index =
            std::min(count - 1U, (count * rank + 99U) / 100U - 1U);
        return samples[index];
    };
    std::printf("P4_NANO_EXACT2X_%s_%s count=%zu min_us=%" PRIu64
                " average_us=%" PRIu64 " p50_us=%" PRIu64
                " p95_us=%" PRIu64 " p99_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                phase, metric, count, count == 0U ? 0U : samples[0],
                count == 0U ? 0U : total / count, percentile(50U),
                percentile(95U), percentile(99U),
                count == 0U ? 0U : samples[count - 1U]);
}

bool exact2x_full_match(const std::uint16_t *source,
                        const std::uint16_t *destination) noexcept
{
    if (source == nullptr || destination == nullptr) {
        return false;
    }
    for (std::size_t y = 0U; y < p4_nano_display::kExact2xSourceHeight; ++y) {
        for (std::size_t x = 0U; x < p4_nano_display::kExact2xSourceWidth; ++x) {
            const std::uint16_t pixel = source[y * p4_nano_display::kExact2xSourceWidth + x];
            for (std::size_t oy = 0U; oy < 2U; ++oy) {
                for (std::size_t ox = 0U; ox < 2U; ++ox) {
                    if (destination[(2U * y + oy) * p4_nano_display::kExact2xDestinationWidth +
                                    2U * x + ox] != pixel) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool exact2x_normalize(const std::uint16_t *source, std::uint16_t *destination) noexcept
{
    const esp_err_t source_result = esp_cache_msync(
        const_cast<std::uint16_t *>(source), p4_nano_display::kExact2xSourceBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const esp_err_t destination_result = esp_cache_msync(
        destination, p4_nano_display::kExact2xDestinationBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return source_result == ESP_OK && destination_result == ESP_OK;
}

bool exact2x_scalar_kernel(const std::uint16_t *source,
                           std::uint16_t *destination) noexcept
{
    return p4_nano_display::exact2x_scalar(
        source, p4_nano_display::kExact2xSourceBytes, destination,
        p4_nano_display::kExact2xDestinationBytes);
}

bool exact2x_pie_kernel(const std::uint16_t *source,
                        std::uint16_t *destination) noexcept
{
    p4_nano_display::exact2x_pie_aligned(source, destination);
    return true;
}

bool exact2x_phase_result(const Exact2xStats &stats) noexcept
{
    return stats.cache_ok && stats.source_immutable && stats.byte_exact &&
           stats.final_validation_ok &&
           stats.stored == p4_nano_display::kExact2xMeasuredSamples;
}

void exact2x_report_invalid_phase(Exact2xStats *stats, const char *phase)
{
    if (stats == nullptr || phase == nullptr) {
        return;
    }
    exact2x_reset_stats(stats);
    stats->cache_ok = false;
    exact2x_print_metric(phase, "KERNEL", stats->kernel, stats->stored);
    exact2x_print_metric(phase, "CACHE_SYNC", stats->cache, stats->stored);
    exact2x_print_metric(phase, "SERVICE", stats->service, stats->stored);
    std::printf("P4_NANO_EXACT2X_%s_CORRECTNESS source_crc_before=0x00000000"
                " source_crc_after=0x00000000 output_crc=0x00000000"
                " source_immutable=0 byte_exact=0 final_validation=0"
                " samples=0 result=FAIL\n",
                phase);
    std::printf("P4_NANO_EXACT2X_%s_RESULT=FAIL\n", phase);
}

bool exact2x_run_phase(BenchmarkState *state, Exact2xStats *stats,
                       Exact2xKernel kernel, const char *phase,
                       const std::uint16_t *source, std::uint16_t *destination,
                       bool kernel_layout_ok)
{
    if (state == nullptr || stats == nullptr || kernel == nullptr || phase == nullptr ||
        source == nullptr || destination == nullptr) {
        return false;
    }
    exact2x_reset_stats(stats);
    stats->source_crc_before = p4_nano_display::crc32(
        reinterpret_cast<const std::uint8_t *>(source),
        p4_nano_display::kExact2xSourceBytes);
    if (!kernel_layout_ok ||
        stats->source_crc_before != p4_nano_display::kExact2xExpectedSourceCrc ||
        esp_cache_msync(const_cast<std::uint16_t *>(source),
                        p4_nano_display::kExact2xSourceBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
        stats->cache_ok = false;
    }
    for (std::size_t index = 0U;
         stats->cache_ok &&
         index < p4_nano_display::kExact2xWarmupSamples +
                     p4_nano_display::kExact2xMeasuredSamples;
         ++index) {
        if (!exact2x_normalize(source, destination)) {
            stats->cache_ok = false;
            break;
        }
        const std::uint64_t kernel_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const bool transformed = kernel(source, destination);
        const std::uint64_t kernel_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - kernel_start;
        const std::uint64_t cache_start =
            static_cast<std::uint64_t>(esp_timer_get_time());
        const esp_err_t sync_result =
            p4_nano_display::display_session_sync_framebuffer(&state->display);
        const std::uint64_t cache_us =
            static_cast<std::uint64_t>(esp_timer_get_time()) - cache_start;
        if (!transformed || sync_result != ESP_OK) {
            stats->cache_ok = false;
            break;
        }
        if (index >= p4_nano_display::kExact2xWarmupSamples) {
            exact2x_add_sample(stats, kernel_us, cache_us, kernel_us + cache_us,
                               index - p4_nano_display::kExact2xWarmupSamples);
        }
        const std::size_t completed_iterations = index + 1U;
        if (completed_iterations % kExact2xCooperateInterval == 0U) {
            vTaskDelay(kExact2xCooperateDelayTicks);
        }
    }
    // Keep this required one-call validation outside the measured window.
    if (stats->cache_ok && exact2x_normalize(source, destination)) {
        const bool transformed = kernel(source, destination);
        const esp_err_t sync_result =
            p4_nano_display::display_session_sync_framebuffer(&state->display);
        stats->final_validation_ok = transformed && sync_result == ESP_OK;
    }
    if (stats->cache_ok) {
        stats->source_crc_after = p4_nano_display::crc32(
            reinterpret_cast<const std::uint8_t *>(source),
            p4_nano_display::kExact2xSourceBytes);
        stats->output_crc = p4_nano_display::crc32(
            reinterpret_cast<const std::uint8_t *>(destination),
            p4_nano_display::kExact2xDestinationBytes);
        stats->source_immutable =
            stats->source_crc_before == stats->source_crc_after;
        stats->byte_exact =
            stats->output_crc == p4_nano_display::kExact2xExpectedDestinationCrc &&
            exact2x_full_match(source, destination);
    }
    exact2x_print_metric(phase, "KERNEL", stats->kernel, stats->stored);
    exact2x_print_metric(phase, "CACHE_SYNC", stats->cache, stats->stored);
    exact2x_print_metric(phase, "SERVICE", stats->service, stats->stored);
    const bool result = exact2x_phase_result(*stats);
    std::printf("P4_NANO_EXACT2X_%s_CORRECTNESS source_crc_before=0x%08" PRIx32
                " source_crc_after=0x%08" PRIx32 " output_crc=0x%08" PRIx32
                " source_immutable=%d byte_exact=%d final_validation=%d"
                " samples=%zu result=%s\n",
                phase, stats->source_crc_before, stats->source_crc_after,
                stats->output_crc, stats->source_immutable ? 1 : 0,
                stats->byte_exact ? 1 : 0, stats->final_validation_ok ? 1 : 0,
                stats->stored, result ? "PASS" : "FAIL");
    std::printf("P4_NANO_EXACT2X_%s_RESULT=%s\n", phase,
                result ? "PASS" : "FAIL");
    return result;
}

bool exact2x_run_samples(BenchmarkState *state)
{
    if (state == nullptr || !state->isolated_source_held ||
        state->isolated_source_view.ptr == nullptr) {
        return false;
    }
    auto *source = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(
        64U, p4_nano_display::kExact2xSourceBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *destination = state->display.framebuffer;
    const auto *original = reinterpret_cast<const std::uint16_t *>(
        state->isolated_source_view.ptr);
    const bool source_m2c_alignment_ok = source != nullptr &&
        reinterpret_cast<std::uintptr_t>(source) %
                p4_nano_display::kExact2xM2CAlignmentBytes == 0U &&
        p4_nano_display::kExact2xSourceBytes %
                p4_nano_display::kExact2xM2CAlignmentBytes == 0U;
    const bool destination_m2c_alignment_ok = destination != nullptr &&
        reinterpret_cast<std::uintptr_t>(destination) %
                p4_nano_display::kExact2xM2CAlignmentBytes == 0U &&
        p4_nano_display::kExact2xDestinationBytes %
                p4_nano_display::kExact2xM2CAlignmentBytes == 0U;
    const bool layout_ok = source != nullptr && destination != nullptr &&
        esp_ptr_external_ram(source) && esp_ptr_external_ram(destination) &&
        reinterpret_cast<std::uintptr_t>(source) % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(destination) % 16U == 0U &&
        source_m2c_alignment_ok && destination_m2c_alignment_ok;
    std::printf("P4_NANO_EXACT2X_SOURCE ptr=%p external=%d mod64=%zu bytes=%zu "
                "m2c_alignment=%s geometry=400x640 stride=800 immutable=1\n",
                static_cast<void *>(source), source != nullptr && esp_ptr_external_ram(source) ? 1 : 0,
                source == nullptr ? 0U : reinterpret_cast<std::uintptr_t>(source) % 64U,
                p4_nano_display::kExact2xSourceBytes,
                source_m2c_alignment_ok ? "PASS" : "FAIL");
    std::printf("P4_NANO_EXACT2X_FRAMEBUFFER ptr=%p external=%d mod16=%zu mod64=%zu "
                "bytes=%zu m2c_alignment=%s geometry=800x1280 stride=1600 num_fbs=1\n",
                static_cast<void *>(destination), destination != nullptr && esp_ptr_external_ram(destination) ? 1 : 0,
                destination == nullptr ? 0U : reinterpret_cast<std::uintptr_t>(destination) % 16U,
                destination == nullptr ? 0U : reinterpret_cast<std::uintptr_t>(destination) % 64U,
                p4_nano_display::kExact2xDestinationBytes,
                destination_m2c_alignment_ok ? "PASS" : "FAIL");
    const bool pie_layout_ok = source != nullptr && destination != nullptr &&
        esp_ptr_external_ram(source) && esp_ptr_external_ram(destination) &&
        reinterpret_cast<std::uintptr_t>(source) %
                p4_nano_display::kExact2xRequiredAlignmentBytes == 0U &&
        reinterpret_cast<std::uintptr_t>(destination) %
                p4_nano_display::kExact2xRequiredAlignmentBytes == 0U;
    std::printf("P4_NANO_EXACT2X_PIE_LAYOUT source_mod16=%zu destination_mod16=%zu "
                "aligned=%s\n",
                source == nullptr ? 0U :
                    reinterpret_cast<std::uintptr_t>(source) %
                        p4_nano_display::kExact2xRequiredAlignmentBytes,
                destination == nullptr ? 0U :
                    reinterpret_cast<std::uintptr_t>(destination) %
                        p4_nano_display::kExact2xRequiredAlignmentBytes,
                pie_layout_ok ? "PASS" : "FAIL");
    if (!layout_ok) {
        exact2x_report_invalid_phase(&exact2x_pie_stats, "PIE");
        heap_caps_free(source);
        return false;
    }
    for (std::size_t y = 0U; y < 400U; ++y) {
        for (std::size_t x = 0U; x < 640U; ++x) {
            source[(640U - 1U - x) *
                       p4_nano_display::kExact2xSourceWidth + y] =
                original[y * 640U + x];
        }
    }
    std::printf("P4_NANO_EXACT2X_COOPERATE interval=%zu delay_ticks=%u\n",
                kExact2xCooperateInterval,
                static_cast<unsigned>(kExact2xCooperateDelayTicks));
    const bool scalar_result = exact2x_run_phase(
        state, &exact2x_scalar_stats, exact2x_scalar_kernel, "SCALAR", source,
        destination, true);
    const bool pie_result = exact2x_run_phase(
        state, &exact2x_pie_stats, exact2x_pie_kernel, "PIE", source,
        destination, pie_layout_ok);
    std::printf("P4_NANO_EXACT2X_SAMPLE_COUNTS warmup=%zu measured=%zu final_validation=%zu "
                "scalar_final_validation_ok=%d pie_final_validation_ok=%d "
                "scalar_stored=%zu pie_stored=%zu cache_normalization=outside_kernel\n",
                p4_nano_display::kExact2xWarmupSamples,
                p4_nano_display::kExact2xMeasuredSamples,
                p4_nano_display::kExact2xFinalValidationSamples,
                exact2x_scalar_stats.final_validation_ok ? 1 : 0,
                exact2x_pie_stats.final_validation_ok ? 1 : 0,
                exact2x_scalar_stats.stored, exact2x_pie_stats.stored);
    p4_nano_display::VsyncStatsSnapshot vsync{};
    p4_nano_display::display_session_snapshot_vsync(&state->display, &vsync);
    std::printf("P4_NANO_EXACT2X_VSYNC callback_registered=%d callback_count=%" PRIu32
                " period_count=%" PRIu32 " period_avg_us=%" PRIu64
                " period_min_us=%" PRIu32 " period_max_us=%" PRIu32
                " count_consistency=%s\n", vsync.callback_registered ? 1 : 0,
                vsync.callback_count, vsync.period_count,
                vsync.period_count == 0U ? 0U : vsync.period_total_us / vsync.period_count,
                vsync.period_min_us, vsync.period_max_us,
                vsync.callback_count > 0U && vsync.period_count == vsync.callback_count - 1U
                    ? "PASS" : "FAIL");
    const bool result = scalar_result && pie_result;
    std::printf("P4_NANO_EXACT2X_RESULT=%s\n", result ? "PASS" : "FAIL");
    heap_caps_free(source);
    return result;
}

esp_err_t run_exact2x_benchmark_after_start(BenchmarkState *state)
{
    bool failed = !benchmark_hold_isolated_source(state) ||
                  !benchmark_request_isolated_pause(state);
    if (!failed) {
        failed = !exact2x_run_samples(state);
    }
    state->stop_requested.store(true, std::memory_order_release);
    const bool pause_stable = state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->producer_cooperate_calls.load(std::memory_order_acquire) &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    if (!pause_stable) {
        failed = true;
    }
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        state->releases != state->acquisitions || !state->isolated_resumed) {
        failed = true;
    }
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#elif defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE)
esp_err_t run_exact2x_internal_source_benchmark_after_start(BenchmarkState *state)
{
    bool failed = !benchmark_hold_isolated_source(state) ||
                  !benchmark_request_isolated_pause(state);
    esp_err_t internal_source_result = ESP_FAIL;
    if (!failed) {
        const p4_nano_exact2x_internal_source::Input input{
            .original_source = state->isolated_source_view.ptr,
            .original_source_bytes = np2video_golden_visible_bytes,
            .presentation_slot0 = state->slots[0].ptr,
            .presentation_slot1 = state->slots[1].ptr,
            .presentation_slot_bytes = kSlotBytes,
            .active_framebuffer = state->display.framebuffer,
            .active_framebuffer_bytes =
                p4_nano_display::kNativeFramebufferBytes,
        };
        internal_source_result = p4_nano_exact2x_internal_source::run(input);
        failed = internal_source_result != ESP_OK;
    }
    state->stop_requested.store(true, std::memory_order_release);
    const bool pause_stable = state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->producer_cooperate_calls.load(std::memory_order_acquire) &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    benchmark_hold_visible(state);
    state->backlight_off_failed =
        p4_nano_board::display_backlight_set(0U) != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    const bool vsync_valid = benchmark_vsync_valid(state->display);
    if (internal_source_result != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        !pause_stable ||
        state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions ||
        state->producer_result.status != ESP_OK || !scheduling_contract ||
        !vsync_valid) {
        failed = true;
    }
    benchmark_print_vsync_stats(state->display);
    std::printf("P4_NANO_EXACT2X_INTERNAL_SOURCE_VSYNC_VALID=%s\n",
                vsync_valid ? "PASS" : "FAIL");
    std::printf("P4_NANO_EXACT2X_INTERNAL_SOURCE_LIFECYCLE_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#elif defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
esp_err_t run_ppa_pie_overlap_benchmark_after_start(BenchmarkState *state)
{
    bool failed = !benchmark_hold_isolated_source(state) ||
                  !benchmark_request_isolated_pause(state);
    esp_err_t overlap_result = ESP_FAIL;
    if (!failed) {
        const p4_nano_ppa_pie_overlap::Input input{
            .original_source = state->isolated_source_view.ptr,
            .original_source_bytes = np2video_golden_visible_bytes,
            .presentation_slot0 = state->slots[0].ptr,
            .presentation_slot1 = state->slots[1].ptr,
            .presentation_slot_bytes = kSlotBytes,
            .active_framebuffer = state->display.framebuffer,
            .active_framebuffer_bytes = p4_nano_display::kNativeFramebufferBytes,
        };
        overlap_result = p4_nano_ppa_pie_overlap::run(input);
        failed = overlap_result != ESP_OK;
    }
    state->stop_requested.store(true, std::memory_order_release);
    const bool pause_stable = state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->producer_cooperate_calls.load(std::memory_order_acquire) &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    const bool retain_source_lifetime =
        p4_nano_ppa_pie_overlap::transaction_lifetime_must_be_retained();
    if (state->isolated_source_held && !retain_source_lifetime) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    } else if (retain_source_lifetime) {
        std::printf("P4_NANO_PPA_PIE_OVERLAP_SOURCE_LIFETIME=RETAINED\n");
    }
    benchmark_hold_visible(state);
    state->backlight_off_failed =
        p4_nano_board::display_backlight_set(0U) != ESP_OK;
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) == kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    const bool vsync_valid = benchmark_vsync_valid(state->display);
    if (overlap_result != ESP_OK || state->publish_failed.load(std::memory_order_acquire) ||
        !pause_stable || state->producer_pause_acknowledged.load(std::memory_order_acquire) ||
        !state->isolated_resumed || state->backlight_off_failed ||
        state->releases != state->acquisitions || state->producer_result.status != ESP_OK ||
        !scheduling_contract || !vsync_valid) {
        failed = true;
    }
    benchmark_print_vsync_stats(state->display);
    std::printf("P4_NANO_PPA_PIE_OVERLAP_VSYNC_VALID=%s\n",
                vsync_valid ? "PASS" : "FAIL");
    std::printf("P4_NANO_PPA_PIE_OVERLAP_LIFECYCLE_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#elif defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
esp_err_t benchmark_p1_run_isolated_after_start(BenchmarkState *state)
{
    bool failed = !benchmark_hold_isolated_source(state) ||
                  !benchmark_request_isolated_pause(state);
    if (!failed) {
        failed = !benchmark_p1_run_samples(state);
    }
    state->stop_requested.store(true, std::memory_order_release);
    const bool pause_stable =
        state->isolated_pause_acknowledged &&
        state->isolated_pause_cooperate_calls ==
            state->producer_cooperate_calls.load(std::memory_order_acquire) &&
        state->producer_pause_acknowledged.load(std::memory_order_acquire);
    if (!pause_stable) {
        failed = true;
    }
    state->producer_pause_requested.store(false, std::memory_order_release);
    if (state->isolated_pause_requested && state->isolated_pause_resume != nullptr) {
        (void)xSemaphoreGive(state->isolated_pause_resume);
        state->isolated_resumed = true;
        std::printf("PRODUCER_RESUMED\n");
    }
    while (!state->producer_done.load(std::memory_order_acquire)) {
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (state->isolated_source_held) {
        benchmark_release(state, &state->isolated_source_token);
        state->isolated_source_held = false;
    }
    const bool scheduling_contract =
        state->producer_core.load(std::memory_order_relaxed) ==
            kBenchmarkProducerCore &&
        state->producer_priority.load(std::memory_order_relaxed) ==
            kBenchmarkProducerPriority && xPortGetCoreID() == 0 &&
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) == 1U;
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        !state->isolated_resumed || !scheduling_contract ||
        state->releases != state->acquisitions) {
        failed = true;
    }
    benchmark_p1_print_report(state, "isolated", !failed);
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    benchmark_p1_free_buffers(state);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    return failed ? ESP_FAIL : ESP_OK;
}
 #endif
#endif

#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
esp_err_t benchmark_p1_run_live_after_start(BenchmarkState *state)
{
    bool failed = false;
    const std::uint64_t wait_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    while (!state->scene_ready.load(std::memory_order_acquire) &&
           !state->producer_done.load(std::memory_order_acquire)) {
        if (static_cast<std::uint64_t>(esp_timer_get_time()) - wait_start >
            kBenchmarkWatchdogUs) {
            failed = true;
            break;
        }
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (!failed) {
        failed = !benchmark_p1_run_samples(state);
    }
    if (state->p1_phase_submit_end <= state->p1_phase_submit_start) {
        failed = true;
    }
    state->stop_requested.store(true, std::memory_order_release);
    const std::uint64_t stop_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    while (!state->producer_done.load(std::memory_order_acquire)) {
        if (static_cast<std::uint64_t>(esp_timer_get_time()) - stop_start >
            kBenchmarkWatchdogUs) {
            failed = true;
            break;
        }
        vTaskDelay(kConsumerPollDelayTicks);
    }
    if (!state->producer_done.load(std::memory_order_acquire)) {
        failed = true;
    }
    if (state->producer_result.status != ESP_OK ||
        state->publish_failed.load(std::memory_order_acquire) ||
        state->producer_core.load(std::memory_order_relaxed) !=
            kBenchmarkProducerCore ||
        state->producer_priority.load(std::memory_order_relaxed) !=
            kBenchmarkProducerPriority || xPortGetCoreID() != 0 ||
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)) != 1U) {
        failed = true;
    }
    benchmark_p1_print_report(state, "live", !failed);
    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state->display);
    benchmark_p1_free_buffers(state);
    heap_caps_free(state->slots[0].ptr);
    heap_caps_free(state->slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=%s\n",
                failed ? "FAIL" : "PASS");
    return failed ? ESP_FAIL : ESP_OK;
}
#endif

#endif

} // namespace

namespace p4_nano_live_display {

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
esp_err_t run_benchmark();
#endif

esp_err_t run()
{
#if defined(P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE)
    return run_motion_validation();
#elif defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE) || \
    defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
    return run_benchmark();
#else
    LiveState state;
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    std::printf("P4_NANO_LIVE_DISPLAY rotation=CCW chip_revision=%d\n",
                chip_info.revision);
    std::printf("P4_NANO_LIVE_SCENE fixture_id=%s scene_id=%u "
                "source_geometry=%ux%u source_crc32=0x%08" PRIx32 "\n",
                np2video_golden_fixture_id,
                static_cast<unsigned>(np2video_golden_scene_id),
                static_cast<unsigned>(np2video_golden_width),
                static_cast<unsigned>(np2video_golden_height),
                np2video_golden_crc32);
    report_memory("before_presentation_slots");
    std::printf("P4_NANO_LIVE_DISPLAY_INIT backlight=OFF num_fbs=1\n");

    esp_err_t result = state.session.initialize();
    if (result != ESP_OK) {
        std::printf("P4_NANO_LIVE_SESSION result=FAIL initialize=%s\n",
                    esp_err_to_name(result));
        return result;
    }
    std::printf("P4_NANO_LIVE_SLOTS result=PASS count=%zu bytes_each=%zu "
                "bytes_total=%zu external=1 disjoint=1\n",
                kSlotCount, kSlotBytes, kSlotCount * kSlotBytes);
    report_memory("after_presentation_slots");
    std::printf("P4_NANO_LIVE_FRAMEBUFFER result=PASS native=800x1280 "
                "bytes=%zu num_fbs=1 external=%d\n",
                p4_nano_display::kNativeFramebufferBytes,
                state.session.native_framebuffer_external() ? 1 : 0);
    report_memory("after_native_framebuffer");

    const np2video_runner_config runner_config{
        .output = runner_output,
        .output_context = nullptr,
        .ready = runner_ready,
        .scene_ready = nullptr,
        .stopping = runner_stopping,
        .complete = runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
        .stop_requested = nullptr,
        .cooperate = nullptr,
        .pause_at_cooperate = nullptr,
        .task_scheduling_override = false,
        .task_core_id = 0,
        .task_priority = 0,
    };
    result = np2video_runner_start_ex(&runner_config);
    if (result != ESP_OK) {
        std::printf("P4_NANO_LIVE_PRODUCER result=FAIL start=%s\n",
                    esp_err_to_name(result));
        (void)state.session.shutdown();
        return result;
    }
    std::printf("P4_NANO_LIVE_PRODUCER result=STARTED\n");

    const std::uint64_t producer_start_us =
        static_cast<std::uint64_t>(esp_timer_get_time());
    std::uint64_t visible_start_us = 0U;
    bool failed = false;
    bool final_drain = false;

    auto consume_one = [&]() -> int {
        const p4_nano_live_display_session::ConsumeResult consumed =
            state.session.consume_one(normal_frame_observer, &state);
        if (consumed == p4_nano_live_display_session::ConsumeResult::NoFrame) {
            return 0;
        }
        if (consumed == p4_nano_live_display_session::ConsumeResult::Failed) {
            return -1;
        }
        if (visible_start_us == 0U && state.session.visible()) {
            visible_start_us = static_cast<std::uint64_t>(esp_timer_get_time());
            std::printf("P4_NANO_LIVE_VISIBLE rotation=CCW backlight=0x40 "
                        "hold_seconds=30\n");
        }
        return 1;
    };

    while (!state.producer_done.load(std::memory_order_acquire)) {
        const int consumed = consume_one();
        if (consumed < 0) {
            failed = true;
        } else if (consumed == 0) {
            vTaskDelay(kConsumerPollDelayTicks);
        }
        if (static_cast<std::uint64_t>(esp_timer_get_time()) -
                producer_start_us > kProducerWatchdogUs &&
            state.timeout_reported == 0U) {
            std::printf("P4_NANO_LIVE_PRODUCER timeout=1\n");
            state.timeout_reported = 1U;
            failed = true;
        }
    }

    while (!final_drain) {
        const int consumed = consume_one();
        if (consumed < 0) {
            failed = true;
        } else if (consumed == 0) {
            final_drain = true;
        }
    }

    const auto &counters = state.session.counters();
    if (state.producer_result.status != ESP_OK ||
        state.session.failed() || counters.transformed == 0U ||
        !state.immutable_pass ||
        state.final_source_crc != np2video_golden_crc32 ||
        state.final_source_crc != state.producer_result.source_crc32) {
        failed = true;
    }

    if (!failed && state.session.visible()) {
        const std::uint64_t elapsed =
            static_cast<std::uint64_t>(esp_timer_get_time()) - visible_start_us;
        if (elapsed < kVisibleHoldUs) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<std::uint32_t>(
                (kVisibleHoldUs - elapsed + 999U) / 1000U)));
        }
    }

    std::printf("P4_NANO_LIVE_COUNTERS submitted=%" PRIu32
                " acquired=%" PRIu32 " transformed=%" PRIu32
                " released=%" PRIu32 " coalesced=%" PRIu32
                " dropped=%" PRIu32 "\n",
                counters.submitted, counters.acquired, counters.transformed,
                counters.released, counters.coalesced, counters.dropped);
    const std::uint64_t average = counters.transformed == 0U ? 0U :
        state.total_transform_us / counters.transformed;
    std::printf("P4_NANO_LIVE_TRANSFORM_TIMING count=%" PRIu32
                " min_us=%" PRIu64 " max_us=%" PRIu64
                " average_us=%" PRIu64 " total_us=%" PRIu64 "\n",
                counters.transformed,
                state.min_transform_us == std::numeric_limits<std::uint64_t>::max()
                    ? 0U : state.min_transform_us,
                state.max_transform_us, average, state.total_transform_us);
    std::printf("P4_NANO_LIVE_FINAL source_generation=%" PRIu32
                " source_update_sequence=%" PRIu32
                " published_sequence=%" PRIu64
                " source_crc32=0x%08" PRIx32
                " native_crc32=0x%08" PRIx32 " source_golden=%s\n",
                state.final_source_generation, state.final_source_update_sequence,
                state.final_published_sequence, state.final_source_crc,
                state.final_native_crc,
                state.final_source_crc == np2video_golden_crc32 ? "PASS" : "FAIL");
    std::printf("P4_NANO_LIVE_RESULT=%s visible_hold_seconds=30\n",
                failed ? "FAIL" : "PASS");

    const esp_err_t cleanup_result = state.session.shutdown();
    std::printf("P4_NANO_LIVE_CLEANUP result=%s backlight=OFF\n",
                cleanup_result == ESP_OK ? "PASS" : "FAIL");
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
#endif
}

#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
esp_err_t run_psram_bandwidth_benchmark()
{
    static BenchmarkState state;
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    const char *condition =
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
        "isolated";
#else
        "live";
#endif
    std::printf("P4_NANO_PSRAM_BANDWIDTH_START operation=%s condition=%s "
                "chip_revision=%d fixture_id=%s scene_id=%u warmup=%u "
                "measured=%u final_validation=%u scanout=active num_fbs=1\n",
                p4_nano_psram_bandwidth::operation_name(kP1Operation),
                condition, chip_info.revision, np2video_golden_fixture_id,
                static_cast<unsigned>(np2video_golden_scene_id),
                static_cast<unsigned>(kBenchmarkWarmupTransforms),
                static_cast<unsigned>(kBenchmarkMeasuredTransforms),
                static_cast<unsigned>(kBenchmarkFinalValidationTransforms));
    report_memory("before_psram_bandwidth_slots");
    for (std::size_t index = 0; index < kSlotCount; ++index) {
        state.slots[index].ptr = static_cast<std::uint8_t *>(heap_caps_calloc(
            1, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        state.slots[index].capacity = kSlotBytes;
        if (state.slots[index].ptr == nullptr) {
            for (std::size_t release = 0; release < index; ++release) {
                heap_caps_free(state.slots[release].ptr);
            }
            std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=slot_alloc\n");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!esp_ptr_external_ram(state.slots[0].ptr) ||
        !esp_ptr_external_ram(state.slots[1].ptr) ||
        ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.slots[1].ptr, kSlotBytes)) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=slot_layout\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (np2_presentation_init(&state.publisher, state.slots) !=
        NP2_PRESENTATION_OK) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=publisher_init\n");
        return ESP_ERR_INVALID_STATE;
    }
    state.slots_initialized = true;
    report_memory("after_psram_bandwidth_slots");

    esp_err_t result =
        p4_nano_display::display_session_initialize(&state.display);
    if (result != ESP_OK) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    if (ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.display.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes) ||
        ranges_overlap(state.slots[1].ptr, kSlotBytes,
                       state.display.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes)) {
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_PSRAM_BANDWIDTH_RESULT=FAIL reason=dsi_alias\n");
        return ESP_ERR_INVALID_STATE;
    }
    report_memory("after_psram_bandwidth_framebuffer");
    result = p4_nano_board::display_backlight_set(0U);
    if (result != ESP_OK || !benchmark_p1_prepare_buffers(&state)) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        benchmark_p1_free_buffers(&state);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result == ESP_OK ? ESP_ERR_NO_MEM : result;
    }

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    state.isolated_pause_ack =
        xSemaphoreCreateBinaryStatic(&state.isolated_pause_ack_storage);
    state.isolated_pause_resume =
        xSemaphoreCreateBinaryStatic(&state.isolated_pause_resume_storage);
    if (state.isolated_pause_ack == nullptr ||
        state.isolated_pause_resume == nullptr) {
        benchmark_p1_free_buffers(&state);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_NO_MEM;
    }
#endif

    const np2video_runner_config runner_config{
        .output = benchmark_runner_output,
        .output_context = nullptr,
        .ready = benchmark_runner_ready,
        .scene_ready = benchmark_scene_ready,
        .stopping = benchmark_runner_stopping,
        .complete = benchmark_runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
        .stop_requested = benchmark_stop_requested,
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
        .cooperate = benchmark_producer_cooperate,
        .pause_at_cooperate = benchmark_pause_at_cooperate,
#else
        .cooperate = nullptr,
        .pause_at_cooperate = nullptr,
#endif
        .task_scheduling_override = true,
        .task_core_id = kBenchmarkProducerCore,
        .task_priority = kBenchmarkProducerPriority,
    };
    result = np2video_runner_start_ex(&runner_config);
    if (result != ESP_OK) {
        benchmark_p1_free_buffers(&state);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    state.producer_start_us = static_cast<std::uint64_t>(esp_timer_get_time());
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    return benchmark_p1_run_isolated_after_start(&state);
#else
    return benchmark_p1_run_live_after_start(&state);
#endif
}
#endif

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)

esp_err_t run_benchmark()
{
#if defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
    return run_psram_bandwidth_benchmark();
#else
    /* The fixed statistics arrays are deliberately static: keeping them out
     * of app_main's task stack avoids turning bounded instrumentation storage
     * into a stack-overflow risk. */
    static BenchmarkState state;
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    std::printf("P4_NANO_LIVE_DISPLAY_BENCHMARK rotation=CCW chip_revision=%d\n",
                chip_info.revision);
    std::printf("P4_NANO_BENCHMARK_SCENE fixture_id=%s scene_id=%u "
                "source_geometry=%ux%u warmup=%u measured=%u total=%u\n",
                np2video_golden_fixture_id,
                static_cast<unsigned>(np2video_golden_scene_id),
                static_cast<unsigned>(np2video_golden_width),
                static_cast<unsigned>(np2video_golden_height),
                static_cast<unsigned>(kBenchmarkWarmupTransforms),
                static_cast<unsigned>(kBenchmarkMeasuredTransforms),
                static_cast<unsigned>(kBenchmarkTotalTransforms));
#if defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE)
    p4_nano_display::print_benchmark_display_config();
#endif
#if defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
    p4_nano_display::print_benchmark_display_config();
#endif
#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)
    std::printf("P4_NANO_PPA_ROTATION_MODE rotation=CCW90 scale=1.0,1.0 "
                "input=640x400 output=400x640 rgb565=1 scanout=active "
                "native_framebuffer_write=0 blocking=1\n");
#endif
#if defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE)
    std::printf("P4_NANO_PPA_INTERNAL_TILE_MODE rotation=CCW90 scale=1.0,1.0 "
                "input=640x400 output=400x640 rgb565=1 scanout=active "
                "native_framebuffer_write=0 blocking=1 tile_widths=32,64,128\n");
#endif
#if defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE)
    std::printf("P4_NANO_EXACT2X_MODE source=400x640 destination=800x1280 "
                "format=RGB565 mapping=nearest_neighbor_2x scanout=active "
                "native_framebuffer=1 pie_available=1\n");
#endif
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    std::printf("P4_NANO_TRANSFORM_ISOLATED_CONFIG producer_core=%d "
                "producer_priority=%" PRIu32 " consumer_core=%d "
                "consumer_priority=%" PRIu32 " num_fbs=1 fastpath=0 "
                "source_memory=external destination_memory=external "
                "dsi_scanout=active producer_pause_policy=post_pccore_block\n",
                kBenchmarkProducerCore, kBenchmarkProducerPriority,
                xPortGetCoreID(),
                static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)));
#endif
    report_memory("before_benchmark_slots");

    for (std::size_t index = 0; index < kSlotCount; ++index) {
        state.slots[index].ptr = static_cast<std::uint8_t *>(heap_caps_calloc(
            1, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        state.slots[index].capacity = kSlotBytes;
        if (state.slots[index].ptr == nullptr) {
            for (std::size_t release = 0; release < index; ++release) {
                heap_caps_free(state.slots[release].ptr);
            }
            std::printf("P4_NANO_BENCHMARK_RESULT=FAIL reason=slot_alloc\n");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!esp_ptr_external_ram(state.slots[0].ptr) ||
        !esp_ptr_external_ram(state.slots[1].ptr) ||
        ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.slots[1].ptr, kSlotBytes)) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_BENCHMARK_RESULT=FAIL reason=slot_layout\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (np2_presentation_init(&state.publisher, state.slots) !=
        NP2_PRESENTATION_OK) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_BENCHMARK_RESULT=FAIL reason=publisher_init\n");
        return ESP_ERR_INVALID_STATE;
    }
    state.slots_initialized = true;
    std::printf("P4_NANO_BENCHMARK_SLOTS result=PASS count=%zu bytes_each=%zu "
                "bytes_total=%zu external=1 disjoint=1\n",
                kSlotCount, kSlotBytes, kSlotCount * kSlotBytes);
    report_memory("after_benchmark_slots");

    esp_err_t result = p4_nano_display::display_session_initialize(&state.display);
    if (result != ESP_OK) {
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    if (ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.display.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes) ||
        ranges_overlap(state.slots[1].ptr, kSlotBytes,
                       state.display.framebuffer,
                       p4_nano_display::kNativeFramebufferBytes)) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_BENCHMARK_RESULT=FAIL reason=dsi_alias\n");
        return ESP_ERR_INVALID_STATE;
    }
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    if (!esp_ptr_external_ram(state.display.framebuffer)) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        std::printf("P4_NANO_TRANSFORM_ISOLATED_RESULT=FAIL "
                    "reason=destination_not_external\n");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    std::printf("P4_NANO_BENCHMARK_FRAMEBUFFER result=PASS native=800x1280 "
                "bytes=%zu num_fbs=1 external=%d\n",
                p4_nano_display::kNativeFramebufferBytes,
                esp_ptr_external_ram(state.display.framebuffer) ? 1 : 0);
    report_memory("after_benchmark_framebuffer");

    /* Keep the panel dark until the first complete transformed frame has
     * reached the native framebuffer and its cache has been synchronized. */
    result = p4_nano_board::display_backlight_set(0U);
    if (result != ESP_OK) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }

#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    state.isolated_pause_ack =
        xSemaphoreCreateBinaryStatic(&state.isolated_pause_ack_storage);
    state.isolated_pause_resume =
        xSemaphoreCreateBinaryStatic(&state.isolated_pause_resume_storage);
    if (state.isolated_pause_ack == nullptr ||
        state.isolated_pause_resume == nullptr) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_NO_MEM;
    }
#endif

    /* Pinned default-priority scheduler A/B candidate.  The explicit
     * post-pccore one-tick cooperation remains mandatory; this is not a final
     * emulator-performance policy until physically validated. */
    const np2video_runner_config runner_config{
        .output = benchmark_runner_output,
        .output_context = nullptr,
        .ready = benchmark_runner_ready,
        .scene_ready = benchmark_scene_ready,
        .stopping = benchmark_runner_stopping,
        .complete = benchmark_runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
        .stop_requested = benchmark_stop_requested,
#if defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
        .cooperate = benchmark_producer_cooperate,
        .pause_at_cooperate = benchmark_pause_at_cooperate,
#else
        .cooperate = nullptr,
        .pause_at_cooperate = nullptr,
#endif
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
        .pccore_trace = &state.pccore_trace,
        .draw_trace = &state.draw_trace,
        .cpu_nevent_trace = &state.cpu_nevent_trace,
#else
        .pccore_trace = nullptr,
        .draw_trace = nullptr,
        .cpu_nevent_trace = nullptr,
#endif
        .task_scheduling_override = true,
        .task_core_id = kBenchmarkProducerCore,
        .task_priority = kBenchmarkProducerPriority,
    };
    result = np2video_runner_start_ex(&runner_config);
    if (result != ESP_OK) {
        (void)p4_nano_board::display_backlight_set(0U);
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    state.producer_start_us = static_cast<std::uint64_t>(esp_timer_get_time());

#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)
    return run_ppa_rotation_benchmark_after_start(&state);
#elif defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE)
    return run_ppa_internal_tile_benchmark_after_start(&state);
#elif defined(P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE)
    return run_exact2x_internal_source_benchmark_after_start(&state);
#elif defined(P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE)
    return run_ppa_pie_overlap_benchmark_after_start(&state);
#elif defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE)
    return run_exact2x_benchmark_after_start(&state);
#elif defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE)
    return run_psram_read_control_benchmark_after_start(&state);
#elif defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE)
    return run_compute_control_benchmark_after_start(&state);
#elif defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE)
    return run_isolated_benchmark_after_start(&state);
#else
    bool failed = false;
    while (!state.producer_done.load(std::memory_order_acquire)) {
        const int consumed = benchmark_consume_one(&state);
        if (consumed < 0) {
            failed = true;
            state.stop_requested.store(true, std::memory_order_release);
        } else if (consumed == 0) {
            vTaskDelay(kConsumerPollDelayTicks);
        }
        const std::uint64_t elapsed =
            static_cast<std::uint64_t>(esp_timer_get_time()) - state.producer_start_us;
        if (elapsed > kBenchmarkWatchdogUs && !state.timeout_reported) {
            state.timeout_reported = true;
            state.stop_requested.store(true, std::memory_order_release);
            failed = true;
        }
    }
    while (benchmark_consume_one(&state) > 0) {
        /* Drain pending latest-frame-wins state; frames after the target are
         * released without another transform. */
    }

    benchmark_hold_visible(&state);
    const esp_err_t backlight_off_result =
        p4_nano_board::display_backlight_set(0U);
    state.backlight_off_failed = backlight_off_result != ESP_OK;

    const std::uint32_t coalesced =
        np2_presentation_coalesced_count(&state.publisher);
    const std::uint32_t dropped =
        np2_presentation_dropped_count(&state.publisher);
    const std::uint32_t publish_hook_callbacks = benchmark_counter_delta(
        state.publish_hook_calls, state.scene_ready_publish_hook_calls);
    const std::uint32_t submit_attempts = benchmark_counter_delta(
        state.submit_attempts, state.scene_ready_submit_attempts);
    const std::uint32_t successful_submissions = benchmark_counter_delta(
        state.successful_submissions, state.scene_ready_successful_submissions);
    const std::uint32_t submit_failures = benchmark_counter_delta(
        state.submit_failures, state.scene_ready_submit_failures);
    const std::uint32_t coalesced_window = benchmark_counter_delta(
        coalesced, state.scene_ready_coalesced);
    const std::uint32_t dropped_window = benchmark_counter_delta(
        dropped, state.scene_ready_dropped);
    const int consumer_core = xPortGetCoreID();
    const std::uint32_t consumer_priority =
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr));
    const np2_pccore_profile &validated_pccore_profile =
        state.producer_result.pccore_profile;
    const std::uint64_t validated_cpu_exec_total =
        validated_pccore_profile.counters[NP2_PCCORE_COUNTER_CPU_EXEC_I286] +
        validated_pccore_profile.counters[NP2_PCCORE_COUNTER_CPU_EXEC_V30];
    const bool pccore_phase_counts_match =
        validated_pccore_profile
                .phases[NP2_PCCORE_PHASE_CPU_EXEC_NESTED]
                .count == validated_cpu_exec_total &&
        validated_pccore_profile
                .phases[NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED]
                .count ==
            validated_pccore_profile
                .counters[NP2_PCCORE_COUNTER_NEVENT_PROGRESS];
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
    benchmark_prepare_overlap_analysis(&state);
    const bool submit_trace_is_complete =
        p4_nano_overlap::submit_trace_complete(
            state.overlap_submit_trace_stored,
            successful_submissions);
    const bool overlap_trace_valid =
        !state.overlap_submit_trace_overflow &&
        !state.overlap_transform_trace_overflow &&
        submit_trace_is_complete &&
        state.overlap_analysis.transform_count == state.transforms_completed &&
        state.overlap_analysis.measured_transform_count ==
            kBenchmarkMeasuredTransforms &&
        state.overlap_analysis.unmatched_acquired_count == 0U &&
        state.overlap_analysis.sequence_metadata_mismatch_count == 0U &&
        state.overlap_analysis.submit_intervals_valid &&
        state.overlap_analysis.submit_intervals_chronological &&
        state.overlap_analysis.submit_intervals_non_overlapping &&
        state.overlap_analysis.submit_source_sequences_monotonic &&
        state.overlap_analysis.submit_published_sequences_monotonic &&
        state.overlap_analysis.transform_source_sequences_monotonic &&
        state.overlap_analysis.transform_published_sequences_monotonic;
    const bool pccore_trace_valid =
        state.pccore_overlap_analyzed &&
        state.pccore_analysis.trace_completeness &&
        !state.pccore_analysis.trace_overflow &&
        state.pccore_analysis.trace_validation.intervals_valid &&
        state.pccore_analysis.trace_validation.chronological &&
        state.pccore_analysis.trace_validation.call_indices_monotonic &&
        state.pccore_analysis.trace_validation.intervals_non_overlapping &&
        state.pccore_analysis.trace_validation.max_concurrent <= 1U;
    const bool draw_trace_valid =
        state.draw_overlap_analyzed &&
        state.draw_analysis.trace_completeness &&
        !state.draw_analysis.trace_overflow &&
        state.draw_analysis.trace_validation.intervals_valid &&
        state.draw_analysis.trace_validation.chronological &&
        state.draw_analysis.trace_validation.call_indices_monotonic &&
        state.draw_analysis.trace_validation.intervals_non_overlapping &&
        state.draw_analysis.trace_validation.max_concurrent <= 1U &&
        !state.draw_trace.reentrant &&
        state.draw_analysis.transform_count == state.transforms_completed &&
        state.draw_analysis.measured_transform_count ==
            kBenchmarkMeasuredTransforms;
    const bool hierarchy_valid = draw_trace_valid &&
                                 state.hierarchy_analysis.submit_subset_draw &&
                                 state.hierarchy_analysis.draw_subset_pccore &&
                                 state.hierarchy_analysis.validity &&
                                 state.hierarchy_analysis.stored ==
                                     kBenchmarkMeasuredTransforms &&
                                 state.cpu_nevent_overlap_analyzed &&
                                 state.cpu_nevent_analysis.full_hierarchy_validity;
#else
    const bool overlap_trace_valid = true;
    const bool pccore_trace_valid = true;
    const bool hierarchy_valid = true;
#endif
    if (state.producer_result.status != ESP_OK ||
        state.publish_failed.load(std::memory_order_acquire) ||
        state.transforms_completed != kBenchmarkTotalTransforms ||
        state.correctness_fail != 0U || state.correctness_pass < 2U ||
        !state.immutable_pass ||
        state.cache_sync_failures != 0U || state.releases != state.acquisitions ||
        state.source_generation != state.producer_result.source_generation ||
        state.final_surface_update_sequence <=
            state.scene_ready_surface_update_sequence ||
        state.transform_stored != kBenchmarkMeasuredTransforms ||
        state.cache_stored != kBenchmarkMeasuredTransforms ||
        state.service_stored != kBenchmarkMeasuredTransforms ||
        !state.first_source_crc_captured || !state.final_source_crc_captured ||
        !state.first_native_crc_captured || !state.final_native_crc_captured ||
        !state.visible || state.visible_elapsed_us < kVisibleHoldUs ||
        state.backlight_enable_failed || state.backlight_off_failed ||
        state.timeout_reported ||
        state.producer_core.load(std::memory_order_relaxed) !=
            kBenchmarkProducerCore ||
        state.producer_priority.load(std::memory_order_relaxed) !=
            kBenchmarkProducerPriority || consumer_core != 0 ||
        consumer_priority != 1U || !pccore_phase_counts_match ||
        !overlap_trace_valid || !pccore_trace_valid || !hierarchy_valid) {
        failed = true;
    }

    benchmark_print_metric("submit_us", state.submit_count,
                           state.submit_stored, state.submit_total_us,
                           state.submit_min_us, state.submit_max_us,
                           state.submit_samples);
    benchmark_print_fixed_metric("transform_only_us", state.transform_samples,
                                 state.transform_stored);
    benchmark_print_fixed_metric("cache_sync_us", state.cache_samples,
                                 state.cache_stored);
    benchmark_print_fixed_metric("consumer_service_us", state.service_samples,
                                 state.service_stored);
    benchmark_print_metric("submit_start_to_acquire_us", state.latency_count,
                           state.latency_stored, state.latency_total_us,
                           state.latency_min_us, state.latency_max_us,
                           state.latency_samples);
#if defined(P4_NANO_OVERLAP_TRACE_ACTIVE)
    benchmark_print_overlap_report(&state, successful_submissions,
                                   submit_trace_is_complete);
    benchmark_print_pccore_overlap_report(&state);
    benchmark_print_draw_overlap_report(&state);
    benchmark_print_cpu_nevent_report(&state);
#endif
    std::printf("P4_NANO_BENCHMARK_WARMUP transforms=%u completed=%u\n",
                static_cast<unsigned>(kBenchmarkWarmupTransforms),
                static_cast<unsigned>(std::min(
                    state.transforms_completed, kBenchmarkWarmupTransforms)));
    std::printf("P4_NANO_BENCHMARK_COUNTERS publish_hook_callbacks=%" PRIu32
                " guest_update_callbacks=%" PRIu32
                " submit_attempts=%" PRIu32 " successful_submissions=%" PRIu32
                " submit_failures=%" PRIu32 " coalesced=%" PRIu32
                " dropped=%" PRIu32 " acquisitions=%" PRIu32
                " transforms_started=%" PRIu32 " transforms_completed=%" PRIu32
                " cache_sync_success=%" PRIu32 " cache_sync_failures=%" PRIu32
                " releases=%" PRIu32 " native_framebuffer_updates=%" PRIu32
                " latency_valid=%" PRIu32
                " latency_producer_unavailable=%" PRIu32
                " latency_consumer_lookup_missing=%" PRIu32
                " correctness_pass=%" PRIu32 " correctness_fail=%" PRIu32 "\n",
                publish_hook_callbacks, publish_hook_callbacks, submit_attempts,
                successful_submissions,
                submit_failures, coalesced_window, dropped_window,
                state.acquisitions, state.transforms_started,
                state.transforms_completed, state.cache_sync_success,
                state.cache_sync_failures, state.releases,
                state.native_framebuffer_updates, state.latency_valid,
                state.producer_latency_unavailable,
                state.consumer_latency_lookup_missing,
                state.correctness_pass, state.correctness_fail);
    std::printf("P4_NANO_BENCHMARK_PRELUDE publish_hook_callbacks=%" PRIu32
                " submit_attempts=%" PRIu32 " successful_submissions=%" PRIu32
                " submit_failures=%" PRIu32 " coalesced=%" PRIu32
                " dropped=%" PRIu32 "\n",
                state.scene_ready_publish_hook_calls,
                state.scene_ready_submit_attempts,
                state.scene_ready_successful_submissions,
                state.scene_ready_submit_failures, state.scene_ready_coalesced,
                state.scene_ready_dropped);
    std::printf("P4_NANO_BENCHMARK_SCENE_READY generation=%" PRIu32
                " surface_update_sequence=%" PRIu32 "\n",
                state.scene_generation.load(std::memory_order_relaxed),
                state.scene_update_sequence.load(std::memory_order_relaxed));
    std::printf("P4_NANO_BENCHMARK_GUEST publish_hook_calls=%" PRIu32
                " last_surface_update_sequence=%" PRIu32
                " scene_ready_surface_update_sequence=%" PRIu32
                " final_surface_update_sequence=%" PRIu32
                " delta_surface_update_sequence=%" PRIu32 "\n",
                state.publish_hook_calls,
                state.last_surface_update_sequence,
                state.scene_ready_surface_update_sequence,
                state.final_surface_update_sequence,
                state.final_surface_update_sequence >=
                        state.scene_ready_surface_update_sequence
                    ? state.final_surface_update_sequence -
                          state.scene_ready_surface_update_sequence
                    : 0U);
    std::printf("P4_NANO_BENCHMARK_CORRECTNESS first_source_crc=0x%08" PRIx32
                " final_source_crc=0x%08" PRIx32
                " first_native_crc=0x%08" PRIx32
                " final_native_crc=0x%08" PRIx32
                " immutable=%s source_generation=%" PRIu32
                " final_published_sequence=%" PRIu64 "\n",
                state.first_source_crc, state.final_source_crc,
                state.first_native_crc, state.final_native_crc,
                state.immutable_pass ? "PASS" : "FAIL", state.source_generation,
                state.final_published_sequence);
    std::printf("P4_NANO_BENCHMARK_VISIBLE visible_start_us=%" PRIu64
                " visible_elapsed_us=%" PRIu64 " backlight=OFF"
                " backlight_off=%s\n",
                state.visible_start_us, state.visible_elapsed_us,
                state.backlight_off_failed ? "FAIL" : "PASS");
    std::printf("P4_NANO_BENCHMARK_SAMPLE_COUNTS transform=%zu cache_sync=%zu "
                "consumer_service=%zu\n",
                state.transform_stored, state.cache_stored,
                state.service_stored);
    benchmark_print_vsync_stats(state.display);
    std::printf("P4_NANO_BENCHMARK_COOPERATION producer_cooperate_calls=%" PRIu32
                " consumer_cooperate_calls=%" PRIu32 "\n",
                state.producer_result.cooperate_calls,
                state.consumer_cooperate_calls);
    const std::uint64_t pccore_exec_average_us =
        state.producer_result.pccore_exec_count == 0U
            ? 0U
            : state.producer_result.pccore_exec_total_us /
                  state.producer_result.pccore_exec_count;
    std::printf("P4_NANO_BENCHMARK_EXEC_SLICE metric=pccore_exec_wall_us "
                "count=%" PRIu64
                " first_us=%" PRIu64 " min_us=%" PRIu64
                " max_us=%" PRIu64 " average_us=%" PRIu64 "\n",
                state.producer_result.pccore_exec_count,
                state.producer_result.pccore_exec_first_us,
                state.producer_result.pccore_exec_min_us,
                state.producer_result.pccore_exec_max_us,
                pccore_exec_average_us);
    const auto print_pccore_phase = [](const char *name,
                                       const np2_pccore_phase_stats &stats) {
        const std::uint64_t average_us =
            stats.count == 0U ? 0U : stats.total_us / stats.count;
        std::printf("P4_NANO_PCCORE_PHASE phase=%s count=%" PRIu64
                    " total_us=%" PRIu64 " min_single_us=%" PRIu64
                    " max_single_us=%" PRIu64 " average_us=%" PRIu64
                    "\n",
                    name, stats.count, stats.total_us,
                    stats.count == 0U ? 0U : stats.min_single_us,
                    stats.max_single_us, average_us);
    };
    const np2_pccore_profile &pccore_profile =
        state.producer_result.pccore_profile;
    print_pccore_phase(
        "loop_inclusive",
        pccore_profile.phases[NP2_PCCORE_PHASE_LOOP_INCLUSIVE]);
    print_pccore_phase("callbacks",
                       pccore_profile.phases[NP2_PCCORE_PHASE_CALLBACKS]);
    print_pccore_phase("sound",
                       pccore_profile.phases[NP2_PCCORE_PHASE_SOUND]);
    print_pccore_phase("draw_nested",
                       pccore_profile.phases[NP2_PCCORE_PHASE_DRAW_NESTED]);
    print_pccore_phase(
        "cpu_exec_nested",
        pccore_profile.phases[NP2_PCCORE_PHASE_CPU_EXEC_NESTED]);
    print_pccore_phase(
        "nevent_progress_nested",
        pccore_profile.phases[NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED]);
    const std::uint64_t loop_iterations =
        pccore_profile.counters[NP2_PCCORE_COUNTER_LOOP_ITERATION];
    const std::uint64_t cpu_exec_i286 =
        pccore_profile.counters[NP2_PCCORE_COUNTER_CPU_EXEC_I286];
    const std::uint64_t cpu_exec_v30 =
        pccore_profile.counters[NP2_PCCORE_COUNTER_CPU_EXEC_V30];
    const std::uint64_t cpu_skipped_remclock =
        pccore_profile.counters[NP2_PCCORE_COUNTER_CPU_SKIPPED_REMCLOCK];
    const std::uint64_t nevent_progress =
        pccore_profile.counters[NP2_PCCORE_COUNTER_NEVENT_PROGRESS];
    const std::uint64_t cpu_exec_total = cpu_exec_i286 + cpu_exec_v30;
    const std::uint64_t iterations_per_pccore =
        state.producer_result.pccore_exec_count == 0U
            ? 0U
            : loop_iterations / state.producer_result.pccore_exec_count;
    const double cpu_exec_fraction =
        loop_iterations == 0U
            ? 0.0
            : static_cast<double>(cpu_exec_total) /
                  static_cast<double>(loop_iterations);
    const double cpu_skip_fraction =
        loop_iterations == 0U
            ? 0.0
            : static_cast<double>(cpu_skipped_remclock) /
                  static_cast<double>(loop_iterations);
    const std::uint64_t naive_v2_extra_timer_reads = loop_iterations * 4U;
    const std::uint64_t transition_v2_extra_timer_reads =
        cpu_exec_total * 3U + cpu_skipped_remclock * 2U;
    std::printf("P4_NANO_PCCORE_COUNTERS loop_iterations=%" PRIu64
                " cpu_exec_i286=%" PRIu64 " cpu_exec_v30=%" PRIu64
                " cpu_exec_total=%" PRIu64
                " cpu_skipped_remclock=%" PRIu64
                " nevent_progress=%" PRIu64 "\n",
                loop_iterations, cpu_exec_i286, cpu_exec_v30, cpu_exec_total,
                cpu_skipped_remclock, nevent_progress);
    std::printf("P4_NANO_PCCORE_COUNTER_DERIVED iterations_per_pccore=%" PRIu64
                " cpu_exec_fraction=%.6f cpu_skip_fraction=%.6f"
                " naive_v2_extra_timer_reads=%" PRIu64
                " transition_v2_extra_timer_reads=%" PRIu64 "\n",
                iterations_per_pccore, cpu_exec_fraction, cpu_skip_fraction,
                naive_v2_extra_timer_reads,
                transition_v2_extra_timer_reads);
    const np2_pccore_phase_stats &cpu_exec_stats =
        pccore_profile.phases[NP2_PCCORE_PHASE_CPU_EXEC_NESTED];
    const np2_pccore_phase_stats &nevent_stats =
        pccore_profile.phases[NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED];
    const np2_pccore_phase_stats &loop_stats =
        pccore_profile.phases[NP2_PCCORE_PHASE_LOOP_INCLUSIVE];
    const np2_pccore_phase_stats &draw_stats =
        pccore_profile.phases[NP2_PCCORE_PHASE_DRAW_NESTED];
    const std::uint64_t loop_accounted_us =
        cpu_exec_stats.total_us + nevent_stats.total_us;
    const bool loop_reconciliation_valid =
        loop_accounted_us <= loop_stats.total_us;
    const std::uint64_t loop_other_us =
        loop_reconciliation_valid
            ? loop_stats.total_us - loop_accounted_us
            : 0U;
    const auto fraction_percent = [](std::uint64_t numerator,
                                     std::uint64_t denominator) {
        return denominator == 0U
                   ? 0.0
                   : static_cast<double>(numerator) * 100.0 /
                         static_cast<double>(denominator);
    };
    std::printf(
        "P4_NANO_PCCORE_LOOP_BREAKDOWN loop_total_us=%" PRIu64
        " cpu_exec_us=%" PRIu64 " nevent_us=%" PRIu64
        " loop_other_us=%" PRIu64 " cpu_fraction_percent=%.6f"
        " nevent_fraction_percent=%.6f loop_other_fraction_percent=%.6f"
        " draw_fraction_of_nevent_percent=%.6f"
        " draw_fraction_of_loop_percent=%.6f reconciliation=%s\n",
        loop_stats.total_us, cpu_exec_stats.total_us, nevent_stats.total_us,
        loop_other_us, fraction_percent(cpu_exec_stats.total_us,
                                        loop_stats.total_us),
        fraction_percent(nevent_stats.total_us, loop_stats.total_us),
        fraction_percent(loop_other_us, loop_stats.total_us),
        fraction_percent(draw_stats.total_us, nevent_stats.total_us),
        fraction_percent(draw_stats.total_us, loop_stats.total_us),
        loop_reconciliation_valid ? "PASS" : "FAIL");
    const std::uint64_t top_level_profiled_us =
        pccore_profile.phases[NP2_PCCORE_PHASE_LOOP_INCLUSIVE].total_us +
        pccore_profile.phases[NP2_PCCORE_PHASE_CALLBACKS].total_us +
        pccore_profile.phases[NP2_PCCORE_PHASE_SOUND].total_us;
    const std::uint64_t outer_total_us =
        state.producer_result.pccore_exec_total_us;
    const std::uint64_t unattributed_us =
        outer_total_us >= top_level_profiled_us
            ? outer_total_us - top_level_profiled_us
            : 0U;
    const std::uint64_t coverage_percent =
        outer_total_us == 0U ? 0U :
        (top_level_profiled_us * 100U) / outer_total_us;
    std::printf("P4_NANO_PCCORE_PROFILE outer_total_us=%" PRIu64
                " top_level_profiled_us=%" PRIu64
                " unattributed_us=%" PRIu64
                " coverage_percent=%" PRIu64
                " nested_draw_excluded_from_top_level=1\n",
                outer_total_us, top_level_profiled_us, unattributed_us,
                coverage_percent);
    std::printf("P4_NANO_BENCHMARK_TIMING timing_clock=esp_timer_wall_elapsed "
                "preemption_may_be_included=1 "
                "cooperation_delay_outside_isolated_metrics=1\n");
#ifdef CONFIG_ESP_MAIN_TASK_AFFINITY
    std::printf("P4_NANO_BENCHMARK_TASK main_task_affinity=%d freertos_cores=%d "
                "producer_creation=xTaskCreatePinnedToCore "
                "producer_core_policy=cpu1 producer_core=%d "
                "producer_priority=%" PRIu32
                " producer_priority_policy=provisional_pinned_default_priority "
                "consumer_core=%d consumer_priority=%" PRIu32 "\n",
                CONFIG_ESP_MAIN_TASK_AFFINITY, CONFIG_FREERTOS_NUMBER_OF_CORES,
                state.producer_core.load(std::memory_order_relaxed),
                state.producer_priority.load(std::memory_order_relaxed),
                consumer_core, consumer_priority);
#else
    std::printf("P4_NANO_BENCHMARK_TASK main_task_affinity=unknown "
                "freertos_cores=%d producer_creation=xTaskCreatePinnedToCore "
                "producer_core_policy=cpu1 producer_core=%d "
                "producer_priority=%" PRIu32
                " producer_priority_policy=provisional_pinned_default_priority "
                "consumer_core=%d consumer_priority=%" PRIu32 "\n",
                CONFIG_FREERTOS_NUMBER_OF_CORES,
                state.producer_core.load(std::memory_order_relaxed),
                state.producer_priority.load(std::memory_order_relaxed),
                consumer_core, consumer_priority);
#endif
    std::printf("P4_NANO_BENCHMARK_PROFILE num_fbs=1 ppa=0 dma2d=0 simd=0 "
                "second_framebuffer=0 uart_hot_path=0\n");
    std::printf("P4_NANO_BENCHMARK_RESULT=%s\n", failed ? "FAIL" : "PASS");

    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state.display);
    heap_caps_free(state.slots[0].ptr);
    heap_caps_free(state.slots[1].ptr);
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
#endif
#endif
}

#endif

} // namespace p4_nano_live_display
