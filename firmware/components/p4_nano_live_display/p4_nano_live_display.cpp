/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_live_display.hpp"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <compiler.h>
#include "np2_presentation.h"
#include "np2video_runner/np2video_runner.h"
#include "p4_nano_board/p4_nano_board.hpp"
#include "p4_nano_display/p4_nano_display.hpp"
#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_display/p4_nano_display_transform.hpp"
#include "p4_nano_live_display/p4_nano_live_display_contract.hpp"
#include "scrnmng.h"

#include "np2video_golden.h"

namespace {

constexpr std::size_t kSlotBytes =
    p4_nano_live_display::kPresentationSlotBytes;
constexpr std::size_t kSlotCount =
    p4_nano_live_display::kPresentationSlotCount;
constexpr TickType_t kConsumerPollDelay = pdMS_TO_TICKS(1);
constexpr std::uint64_t kVisibleHoldUs = 30ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kProducerWatchdogUs = 120ULL * 1000ULL * 1000ULL;

struct LiveState {
    np2_presentation_publisher publisher{};
    np2_presentation_slot_storage slots[kSlotCount]{};
    p4_nano_display::DisplaySession display{};
    np2video_runner_result producer_result{};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> publish_failed{false};
    std::uint32_t submitted = 0;
    std::uint32_t acquired = 0;
    std::uint32_t transformed = 0;
    std::uint32_t released = 0;
    bool hook_registered = false;
    bool slots_initialized = false;
    bool immutable_checked = false;
    bool immutable_pass = false;
    bool visible = false;
    std::uint32_t final_source_generation = 0;
    std::uint32_t final_source_update_sequence = 0;
    std::uint64_t final_published_sequence = 0;
    std::uint32_t final_source_crc = 0;
    std::uint32_t final_native_crc = 0;
    std::uint64_t min_transform_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_transform_us = 0;
    std::uint64_t total_transform_us = 0;
    std::uint32_t timeout_reported = 0;
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

void publish_hook(const SCRNMNG_PUBLISH_VIEW *view, void *context)
{
    auto *state = static_cast<LiveState *>(context);
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
        std::printf("P4_NANO_LIVE_PUBLISH result=FAIL status=%d\n",
                    static_cast<int>(status));
        state->publish_failed.store(true, std::memory_order_release);
    }
}

bool runner_ready(void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state == nullptr || !state->slots_initialized) {
        return false;
    }
    scrnmng_set_publish_hook(publish_hook, state);
    state->hook_registered = true;
    std::printf("P4_NANO_LIVE_PUBLISHER_READY slots=%zu slot_bytes=%zu\n",
                kSlotCount, kSlotBytes);
    return true;
}

void runner_stopping(void *context)
{
    auto *state = static_cast<LiveState *>(context);
    if (state != nullptr && state->hook_registered) {
        scrnmng_set_publish_hook(nullptr, nullptr);
        state->hook_registered = false;
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

void release_if_held(LiveState *state, np2_presentation_token *token)
{
    if (state == nullptr || token == nullptr || token->lease == 0U) {
        return;
    }
    if (np2_presentation_release(&state->publisher, token) ==
        NP2_PRESENTATION_OK) {
        ++state->released;
    } else {
        state->publish_failed.store(true, std::memory_order_release);
    }
}

} // namespace

namespace p4_nano_live_display {

esp_err_t run()
{
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

    for (std::size_t index = 0; index < kSlotCount; ++index) {
        state.slots[index].ptr = static_cast<std::uint8_t *>(heap_caps_calloc(
            1, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        state.slots[index].capacity = kSlotBytes;
        if (state.slots[index].ptr == nullptr) {
            std::printf("P4_NANO_LIVE_SLOTS result=FAIL index=%zu\n", index);
            for (std::size_t release = 0; release < index; ++release) {
                heap_caps_free(state.slots[release].ptr);
            }
            return ESP_ERR_NO_MEM;
        }
    }
    if (!esp_ptr_external_ram(state.slots[0].ptr) ||
        !esp_ptr_external_ram(state.slots[1].ptr) ||
        ranges_overlap(state.slots[0].ptr, kSlotBytes,
                       state.slots[1].ptr, kSlotBytes)) {
        std::printf("P4_NANO_LIVE_SLOTS result=FAIL external_or_disjoint\n");
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_INVALID_STATE;
    }
    if (np2_presentation_init(&state.publisher, state.slots) !=
        NP2_PRESENTATION_OK) {
        std::printf("P4_NANO_LIVE_SLOTS result=FAIL publisher_init\n");
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_INVALID_STATE;
    }
    state.slots_initialized = true;
    std::printf("P4_NANO_LIVE_SLOTS result=PASS count=%zu bytes_each=%zu "
                "bytes_total=%zu external=1 disjoint=1\n",
                kSlotCount, kSlotBytes, kSlotCount * kSlotBytes);
    report_memory("after_presentation_slots");

    std::printf("P4_NANO_LIVE_DISPLAY_INIT backlight=OFF num_fbs=1\n");
    esp_err_t result = p4_nano_display::display_session_initialize(
        &state.display);
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
        std::printf("P4_NANO_LIVE_SLOTS result=FAIL dsi_alias\n");
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return ESP_ERR_INVALID_STATE;
    }
    std::printf("P4_NANO_LIVE_FRAMEBUFFER result=PASS native=800x1280 "
                "bytes=%zu num_fbs=1 external=%d\n",
                p4_nano_display::kNativeFramebufferBytes,
                esp_ptr_external_ram(state.display.framebuffer) ? 1 : 0);
    report_memory("after_native_framebuffer");

    const np2video_runner_config runner_config{
        .output = runner_output,
        .output_context = nullptr,
        .ready = runner_ready,
        .stopping = runner_stopping,
        .complete = runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
    };
    result = np2video_runner_start_ex(&runner_config);
    if (result != ESP_OK) {
        std::printf("P4_NANO_LIVE_PRODUCER result=FAIL start=%s\n",
                    esp_err_to_name(result));
        (void)p4_nano_display::display_session_cleanup(&state.display);
        heap_caps_free(state.slots[0].ptr);
        heap_caps_free(state.slots[1].ptr);
        return result;
    }
    std::printf("P4_NANO_LIVE_PRODUCER result=STARTED\n");

    std::uint64_t visible_start_us = 0;
    const std::uint64_t producer_start_us =
        static_cast<std::uint64_t>(esp_timer_get_time());
    bool failed = false;
    bool final_drain = false;

    auto consume_one = [&]() -> int {
        np2_presentation_frame_view view{};
        np2_presentation_token token{};
        const np2_presentation_status acquire = np2_presentation_acquire(
            &state.publisher, &view, &token);
        if (acquire == NP2_PRESENTATION_NO_FRAME) {
            return 0;
        }
        if (acquire != NP2_PRESENTATION_OK) {
            state.publish_failed.store(true, std::memory_order_release);
            return -1;
        }
        ++state.acquired;
        if (!validate_frame(view)) {
            std::printf("P4_NANO_LIVE_FRAME result=FAIL unsupported_geometry\n");
            release_if_held(&state, &token);
            return -1;
        }

        const std::uint32_t source_crc = p4_nano_display::crc32(
            view.ptr, np2video_golden_visible_bytes);
        const auto source = std::span<const std::uint16_t>(
            reinterpret_cast<const std::uint16_t *>(view.ptr),
            p4_nano_display::kTransformSourcePixelCount);
        const auto destination = std::span<std::uint16_t>(
            state.display.framebuffer,
            p4_nano_display::kTransformDestinationPixelCount);
        const std::int64_t transform_start = esp_timer_get_time();
        const bool transformed = p4_nano_display::transform_to_native(
            source, destination,
            p4_nano_display::QuarterTurn::CounterClockwise);
        const std::uint64_t transform_us = static_cast<std::uint64_t>(
            esp_timer_get_time() - transform_start);
        if (!transformed) {
            release_if_held(&state, &token);
            return -1;
        }
        if (!state.immutable_checked) {
            const std::uint32_t after_crc = p4_nano_display::crc32(
                view.ptr, np2video_golden_visible_bytes);
            state.immutable_checked = true;
            state.immutable_pass = source_crc == after_crc;
            std::printf("P4_NANO_LIVE_FRAME_IMMUTABLE=%s before=0x%08" PRIx32
                        " after=0x%08" PRIx32 "\n",
                        state.immutable_pass ? "PASS" : "FAIL",
                        source_crc, after_crc);
            if (!state.immutable_pass) {
                release_if_held(&state, &token);
                return -1;
            }
        }
        result = p4_nano_display::display_session_sync_framebuffer(
            &state.display);
        if (result != ESP_OK) {
            release_if_held(&state, &token);
            return -1;
        }
        const std::uint32_t native_crc = p4_nano_display::crc32(
            reinterpret_cast<const std::uint8_t *>(state.display.framebuffer),
            p4_nano_display::kNativeFramebufferBytes);
        release_if_held(&state, &token);
        if (state.min_transform_us == std::numeric_limits<std::uint64_t>::max()) {
            state.min_transform_us = transform_us;
        } else if (transform_us < state.min_transform_us) {
            state.min_transform_us = transform_us;
        }
        if (transform_us > state.max_transform_us) {
            state.max_transform_us = transform_us;
        }
        state.total_transform_us += transform_us;
        ++state.transformed;
        state.final_source_generation = view.source_generation;
        state.final_source_update_sequence = view.source_update_sequence;
        state.final_published_sequence = view.published_sequence;
        state.final_source_crc = source_crc;
        state.final_native_crc = native_crc;
        if (!state.visible) {
            result = p4_nano_board::display_backlight_set(
                p4_nano_board::kBacklightConservative);
            if (result != ESP_OK) {
                return -1;
            }
            state.visible = true;
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
            vTaskDelay(kConsumerPollDelay);
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

    if (state.producer_result.status != ESP_OK ||
        state.publish_failed.load(std::memory_order_acquire) ||
        state.transformed == 0U || !state.immutable_pass ||
        state.final_source_crc != np2video_golden_crc32 ||
        state.final_source_crc != state.producer_result.source_crc32) {
        failed = true;
    }

    if (!failed && state.visible) {
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
                state.submitted, state.acquired, state.transformed,
                state.released,
                np2_presentation_coalesced_count(&state.publisher),
                np2_presentation_dropped_count(&state.publisher));
    const std::uint64_t average = state.transformed == 0U ? 0U :
        state.total_transform_us / state.transformed;
    std::printf("P4_NANO_LIVE_TRANSFORM_TIMING count=%" PRIu32
                " min_us=%" PRIu64 " max_us=%" PRIu64
                " average_us=%" PRIu64 " total_us=%" PRIu64 "\n",
                state.transformed,
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

    const esp_err_t cleanup_result =
        p4_nano_display::display_session_cleanup(&state.display);
    heap_caps_free(state.slots[0].ptr);
    heap_caps_free(state.slots[1].ptr);
    std::printf("P4_NANO_LIVE_CLEANUP result=%s backlight=OFF\n",
                cleanup_result == ESP_OK ? "PASS" : "FAIL");
    if (cleanup_result != ESP_OK) {
        return cleanup_result;
    }
    return failed ? ESP_FAIL : ESP_OK;
}

} // namespace p4_nano_live_display
