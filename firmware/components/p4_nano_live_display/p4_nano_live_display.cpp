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
#include <limits>
#include <span>

#include "sdkconfig.h"

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

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE)

constexpr std::uint32_t kBenchmarkWarmupTransforms = 8U;
constexpr std::uint32_t kBenchmarkMeasuredTransforms = 128U;
constexpr std::uint32_t kBenchmarkFinalValidationTransforms = 1U;
constexpr std::uint32_t kBenchmarkTotalTransforms =
    kBenchmarkWarmupTransforms + kBenchmarkMeasuredTransforms +
    kBenchmarkFinalValidationTransforms;
constexpr std::size_t kBenchmarkSubmitSampleCapacity = 8192U;
constexpr std::size_t kBenchmarkLatencySampleCapacity = 256U;
constexpr std::size_t kBenchmarkTimestampRingSize = 1024U;
constexpr std::uint64_t kBenchmarkWatchdogUs = 120ULL * 1000ULL * 1000ULL;

struct BenchmarkTimestamp {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> submit_start_us{0};
};

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
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> publish_failed{false};
    BenchmarkTimestamp timestamp_ring[kBenchmarkTimestampRingSize]{};
    std::array<std::uint64_t, kBenchmarkSubmitSampleCapacity> submit_samples{};
    std::array<std::uint64_t, kBenchmarkLatencySampleCapacity> latency_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> transform_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> cache_samples{};
    std::array<std::uint64_t, kBenchmarkMeasuredTransforms> service_samples{};
    std::size_t submit_stored = 0;
    std::size_t latency_stored = 0;
    std::size_t transform_stored = 0;
    std::size_t cache_stored = 0;
    std::size_t service_stored = 0;
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
    const std::uint64_t transform_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - transform_start;
    if (!transformed) {
        benchmark_release(state, &token);
        return -1;
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
    const std::uint64_t cache_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - cache_start;
    if (sync_result != ESP_OK) {
        ++state->cache_sync_failures;
        benchmark_release(state, &token);
        return -1;
    }
    ++state->cache_sync_success;
    ++state->native_framebuffer_updates;
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
    benchmark_release(state, &token);
    const std::uint64_t service_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) - service_start;
    if (transform_index >= kBenchmarkWarmupTransforms) {
        /* The measured arrays have exactly 128 entries.  Statistics are
         * calculated only after the benchmark interval. */
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

#endif

} // namespace

namespace p4_nano_live_display {

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE)
esp_err_t run_benchmark();
#endif

esp_err_t run()
{
#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE)
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
        .scene_ready = nullptr,
        .stopping = runner_stopping,
        .complete = runner_complete,
        .complete_context = &state,
        .lifecycle_context = &state,
        .stop_requested = nullptr,
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
#endif
}

#if defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE)

esp_err_t run_benchmark()
{
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

    bool failed = false;
    while (!state.producer_done.load(std::memory_order_acquire)) {
        const int consumed = benchmark_consume_one(&state);
        if (consumed < 0) {
            failed = true;
            state.stop_requested.store(true, std::memory_order_release);
        } else if (consumed == 0) {
            vTaskDelay(kConsumerPollDelay);
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
        state.timeout_reported) {
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
#ifdef CONFIG_ESP_MAIN_TASK_AFFINITY
    std::printf("P4_NANO_BENCHMARK_TASK main_task_affinity=%d freertos_cores=%d "
                "producer_creation=xTaskCreate producer_core=%d consumer_core=%d\n",
                CONFIG_ESP_MAIN_TASK_AFFINITY, CONFIG_FREERTOS_NUMBER_OF_CORES,
                state.producer_core.load(std::memory_order_relaxed),
                xPortGetCoreID());
#else
    std::printf("P4_NANO_BENCHMARK_TASK main_task_affinity=unknown "
                "freertos_cores=%d producer_creation=xTaskCreate "
                "producer_core=%d consumer_core=%d\n",
                CONFIG_FREERTOS_NUMBER_OF_CORES,
                state.producer_core.load(std::memory_order_relaxed),
                xPortGetCoreID());
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
}

#endif

} // namespace p4_nano_live_display
