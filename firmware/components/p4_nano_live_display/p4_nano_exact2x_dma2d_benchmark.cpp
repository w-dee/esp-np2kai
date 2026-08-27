/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_exact2x_dma2d_benchmark.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_nano_dma2d_copy/dma2d_copy.hpp"
#include "p4_nano_display/p4_nano_display_exact2x.hpp"
#include "p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"

#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#error "P10M-D1 requires fixed CPU frequency"
#endif
#if !defined(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) || \
    CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ != 360
#error "P10M-D1 requires CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360"
#endif

namespace {

namespace exact2x = p4_nano_exact2x_internal_source;
namespace dma2d = p4_nano_dma2d_copy;

constexpr std::size_t kTileBytes = exact2x::kTileBytes;
constexpr std::size_t kTileDestinationBytes = exact2x::kTileDestinationBytes;
constexpr std::size_t kTileCount = exact2x::kTileCount;
constexpr std::size_t kDestinationBytes = exact2x::kDestinationBytes;
constexpr std::size_t kStagingBytes = 102400U;
constexpr std::size_t kChunkBytes = 51200U;
constexpr std::size_t kChunkDestinationDelta = 204800U;
constexpr std::size_t kRequiredAlignment = exact2x::kRequiredAlignmentBytes;
constexpr std::size_t kWarmups = exact2x::kWarmupSamples;
constexpr std::size_t kSamples = exact2x::kMeasuredSamples;
constexpr std::uint32_t kCpuMHz = 360U;
constexpr TickType_t kHealthDelay = 1U;

static_assert(kTileCount == 5U);
static_assert(kTileBytes == kStagingBytes);
static_assert(kChunkBytes * 2U == kStagingBytes);
static_assert(kChunkDestinationDelta * 2U == kTileDestinationBytes);
static_assert(kCpuMHz == CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

struct MetricStats final {
    std::array<std::uint64_t, kSamples> values{};
    std::size_t stored = 0U;

    void reset() noexcept { values.fill(0U); stored = 0U; }
    bool add(std::uint64_t value) noexcept
    {
        if (stored == values.size()) {
            return false;
        }
        values[stored++] = value;
        return true;
    }
};

struct ControlStats final {
    MetricStats ppa_common_wall;
    MetricStats pie_full_active;
    MetricStats destination_c2m;
    MetricStats cpu_lower_bound;
    MetricStats exact2x_service;
    MetricStats total_transform_service;
};

struct DmaStats final {
    MetricStats ppa_common_wall;
    MetricStats pie_horizontal_active;
    MetricStats copy_total_wall;
    MetricStats blocked_wait;
    MetricStats nonblocked_task_wall;
    MetricStats callback_during_wait;
    MetricStats cpu_lower_bound;
    MetricStats exact2x_service;
    MetricStats total_transform_service;
};

struct TimerControlStats final {
    MetricStats control;
    MetricStats dma;
};

/* All sample storage is static: run() only carries scalar/pointer locals. */
ControlStats s_control_stats{};
DmaStats s_dma_stats{};
TimerControlStats s_timer_control_stats{};

struct FrameMetrics final {
    std::uint64_t ppa_us = 0U;
    std::uint64_t pie_us = 0U;
    std::uint64_t cache_us = 0U;
    std::uint64_t dma_total_us = 0U;
    std::uint64_t dma_wait_us = 0U;
    std::uint64_t callback_wait_cycles = 0U;
    std::uint64_t exact_us = 0U;
    std::uint64_t total_us = 0U;
    std::uint32_t rotated_crc = 0xffffffffU;
};

void reset(ControlStats *stats) noexcept
{
    stats->ppa_common_wall.reset(); stats->pie_full_active.reset();
    stats->destination_c2m.reset(); stats->cpu_lower_bound.reset();
    stats->exact2x_service.reset(); stats->total_transform_service.reset();
}

void reset(DmaStats *stats) noexcept
{
    stats->ppa_common_wall.reset(); stats->pie_horizontal_active.reset();
    stats->copy_total_wall.reset(); stats->blocked_wait.reset();
    stats->nonblocked_task_wall.reset(); stats->callback_during_wait.reset();
    stats->cpu_lower_bound.reset(); stats->exact2x_service.reset();
    stats->total_transform_service.reset();
}

bool normalize_destination(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
}

bool destination_writeback(std::uint8_t *destination) noexcept
{
    return esp_cache_msync(destination, kDestinationBytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                               ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}

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

bool prepare_tile(ppa_client_handle_t client, const std::uint8_t *original,
                  std::uint8_t *tile, std::size_t tile_index) noexcept
{
    const ppa_srm_oper_config_t operation = exact2x::make_tile_operation(
        original, tile, tile_index, PPA_TRANS_MODE_BLOCKING, nullptr);
    return ppa_do_scale_rotate_mirror(client, &operation) == ESP_OK;
}

std::uint32_t crc32(const std::uint8_t *bytes, std::size_t length) noexcept
{
    return exact2x::crc32_update(0xffffffffU, bytes, length) ^ 0xffffffffU;
}

bool horizontal_matches(const std::uint8_t *source,
                        const std::uint8_t *staging) noexcept
{
    const auto *in = reinterpret_cast<const std::uint16_t *>(source);
    const auto *out = reinterpret_cast<const std::uint16_t *>(staging);
    for (std::size_t y = 0U; y < 64U; ++y) {
        for (std::size_t x = 0U; x < 400U; ++x) {
            if (out[y * 800U + 2U * x] != in[y * 400U + x] ||
                out[y * 800U + 2U * x + 1U] != in[y * 400U + x]) {
                return false;
            }
        }
    }
    return true;
}

void make_reference(const std::uint8_t *original,
                    std::uint8_t *reference) noexcept
{
    const auto *source = reinterpret_cast<const std::uint16_t *>(original);
    auto *destination = reinterpret_cast<std::uint16_t *>(reference);
    for (std::size_t source_y = 0U; source_y < 400U; ++source_y) {
        for (std::size_t source_x = 0U; source_x < 640U; ++source_x) {
            const std::uint16_t pixel = source[source_y * 640U + source_x];
            const std::size_t output_x = source_y;
            const std::size_t output_y = 639U - source_x;
            for (std::size_t oy = 0U; oy < 2U; ++oy) {
                for (std::size_t ox = 0U; ox < 2U; ++ox) {
                    destination[(2U * output_y + oy) * 800U +
                                (2U * output_x + ox)] = pixel;
                }
            }
        }
    }
}

bool run_control_frame(ppa_client_handle_t client, const std::uint8_t *original,
                       std::uint8_t *tile, std::uint8_t *destination,
                       bool final_validation, FrameMetrics *metrics) noexcept
{
    *metrics = {};
    const std::uint64_t total_start = esp_timer_get_time();
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        const std::uint64_t ppa_start = esp_timer_get_time();
        const bool prepared = prepare_tile(client, original, tile, tile_index);
        const std::uint64_t ppa_end = esp_timer_get_time();
        if (!prepared || ppa_end < ppa_start) {
            return false;
        }
        metrics->ppa_us += ppa_end - ppa_start;
        if (final_validation) {
            metrics->rotated_crc = exact2x::crc32_update(
                metrics->rotated_crc, tile, kTileBytes);
        }
        const std::uint64_t pie_start = esp_timer_get_time();
        p4_nano_display::exact2x_pie_tile128_aligned(
            reinterpret_cast<const std::uint16_t *>(tile),
            reinterpret_cast<std::uint16_t *>(destination +
                tile_index * kTileDestinationBytes));
        const std::uint64_t pie_end = esp_timer_get_time();
        if (pie_end < pie_start) {
            return false;
        }
        metrics->pie_us += pie_end - pie_start;
    }
    const std::uint64_t cache_start = esp_timer_get_time();
    const bool cache_ok = destination_writeback(destination);
    const std::uint64_t cache_end = esp_timer_get_time();
    const std::uint64_t total_end = esp_timer_get_time();
    if (!cache_ok || cache_end < cache_start || total_end < total_start) {
        return false;
    }
    metrics->cache_us = cache_end - cache_start;
    metrics->exact_us = metrics->pie_us + metrics->cache_us;
    metrics->total_us = total_end - total_start;
    metrics->rotated_crc ^= 0xffffffffU;
    return true;
}

bool run_dma_frame(ppa_client_handle_t client, const std::uint8_t *original,
                   std::uint8_t *tile, std::uint8_t *staging,
                   std::uint8_t *destination, dma2d::Adapter *adapter,
                   bool final_validation, FrameMetrics *metrics) noexcept
{
    *metrics = {};
    const std::uint64_t total_start = esp_timer_get_time();
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        const std::uint64_t ppa_start = esp_timer_get_time();
        const bool prepared = prepare_tile(client, original, tile, tile_index);
        const std::uint64_t ppa_end = esp_timer_get_time();
        if (!prepared || ppa_end < ppa_start) {
            return false;
        }
        metrics->ppa_us += ppa_end - ppa_start;
        if (final_validation) {
            metrics->rotated_crc = exact2x::crc32_update(
                metrics->rotated_crc, tile, kTileBytes);
        }
        for (std::size_t chunk = 0U; chunk < 2U; ++chunk) {
            const auto *chunk_source = tile + chunk * kChunkBytes;
            const std::uint64_t pie_start = esp_timer_get_time();
            p4_nano_display::exact2x_pie_horizontal64_aligned(
                reinterpret_cast<const std::uint16_t *>(chunk_source),
                reinterpret_cast<std::uint16_t *>(staging));
            const std::uint64_t pie_end = esp_timer_get_time();
            if (pie_end < pie_start ||
                (final_validation && !horizontal_matches(chunk_source, staging))) {
                return false;
            }
            metrics->pie_us += pie_end - pie_start;
            auto *chunk_destination = destination +
                tile_index * kTileDestinationBytes + chunk * kChunkDestinationDelta;
            for (const std::size_t parity : {dma2d::kEvenXOffsetPixels,
                                              dma2d::kOddXOffsetPixels}) {
                dma2d::CopyTiming timing{};
                if (dma2d::copy_strided(adapter, staging, chunk_destination,
                                         parity, &timing) != ESP_OK ||
                    timing.total_wall_us < timing.blocked_wait_us) {
                    return false;
                }
                metrics->dma_total_us += timing.total_wall_us;
                metrics->dma_wait_us += timing.blocked_wait_us;
                metrics->callback_wait_cycles += timing.on_job_cycles_during_wait +
                    timing.eof_cycles_during_wait;
            }
        }
    }
    const std::uint64_t total_end = esp_timer_get_time();
    if (total_end < total_start) {
        return false;
    }
    metrics->total_us = total_end - total_start;
    if (metrics->total_us < metrics->ppa_us) {
        return false;
    }
    /* This outer-frame subtraction retains loop/DMA API work while excluding
     * the five separately bracketed PPA calls. */
    metrics->exact_us = metrics->total_us - metrics->ppa_us;
    metrics->rotated_crc ^= 0xffffffffU;
    return true;
}

bool add_control_sample(const FrameMetrics &m) noexcept
{
    const std::uint64_t cpu = m.pie_us + m.cache_us;
    return s_control_stats.ppa_common_wall.add(m.ppa_us) &&
        s_control_stats.pie_full_active.add(m.pie_us) &&
        s_control_stats.destination_c2m.add(m.cache_us) &&
        s_control_stats.cpu_lower_bound.add(cpu) &&
        s_control_stats.exact2x_service.add(m.exact_us) &&
        s_control_stats.total_transform_service.add(m.total_us);
}

bool add_dma_sample(const FrameMetrics &m) noexcept
{
    if (m.dma_total_us < m.dma_wait_us) {
        return false;
    }
    const std::uint64_t nonblocked = m.dma_total_us - m.dma_wait_us;
    const std::uint64_t callback_us = m.callback_wait_cycles / kCpuMHz;
    const std::uint64_t cpu = m.pie_us + nonblocked + callback_us;
    return s_dma_stats.ppa_common_wall.add(m.ppa_us) &&
        s_dma_stats.pie_horizontal_active.add(m.pie_us) &&
        s_dma_stats.copy_total_wall.add(m.dma_total_us) &&
        s_dma_stats.blocked_wait.add(m.dma_wait_us) &&
        s_dma_stats.nonblocked_task_wall.add(nonblocked) &&
        s_dma_stats.callback_during_wait.add(callback_us) &&
        s_dma_stats.cpu_lower_bound.add(cpu) &&
        s_dma_stats.exact2x_service.add(m.exact_us) &&
        s_dma_stats.total_transform_service.add(m.total_us);
}

template <typename FrameRunner>
bool run_samples(FrameRunner runner, bool control) noexcept
{
    for (std::size_t index = 0U; index < kWarmups + kSamples; ++index) {
        FrameMetrics metrics{};
        if (!runner(false, &metrics)) {
            return false;
        }
        if (index >= kWarmups &&
            !(control ? add_control_sample(metrics) : add_dma_sample(metrics))) {
            return false;
        }
        if ((index + 1U) % 64U == 0U) {
            vTaskDelay(kHealthDelay);
        }
    }
    return true;
}

std::uint64_t percentile(MetricStats &stats, std::size_t percent) noexcept
{
    const std::size_t index = std::min(stats.stored - 1U,
        (stats.stored * percent + 99U) / 100U - 1U);
    return stats.values[index];
}

void print_metric(const char *phase, const char *name, MetricStats *stats,
                  bool primary) noexcept
{
    std::sort(stats->values.begin(), stats->values.begin() + stats->stored);
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < stats->stored; ++index) {
        total += stats->values[index];
    }
    const std::uint64_t p99 = percentile(*stats, 99U);
    const std::uint64_t maximum = stats->values[stats->stored - 1U];
    std::printf("P4_NANO_EXACT2X_DMA2D_BENCHMARK_METRIC phase=%s metric=%s "
                "count=%zu min_us=%" PRIu64 " average_us=%" PRIu64
                " p50_us=%" PRIu64 " p95_us=%" PRIu64
                " p99_us=%" PRIu64 " max_us=%" PRIu64
                " max_minus_p99_us=%" PRIu64 " primary=%d\n",
                phase, name, stats->stored, stats->values.front(),
                total / stats->stored, percentile(*stats, 50U),
                percentile(*stats, 95U), p99, maximum, maximum - p99,
                primary ? 1 : 0);
}

void run_timer_control_one(MetricStats *stats, std::size_t timer_reads,
                           std::size_t cycle_reads) noexcept
{
    stats->reset();
    std::uint64_t sink = 0U;
    for (std::size_t sample = 0U; sample < kWarmups + kSamples; ++sample) {
        const std::uint64_t start = esp_timer_get_time();
        for (std::size_t read = 0U; read < timer_reads; ++read) {
            sink += static_cast<std::uint64_t>(esp_timer_get_time());
        }
        for (std::size_t read = 0U; read < cycle_reads; ++read) {
            sink += static_cast<std::uint64_t>(esp_cpu_get_cycle_count());
        }
        const std::uint64_t elapsed = esp_timer_get_time() - start;
        if (sample >= kWarmups) {
            (void)stats->add(elapsed);
        }
    }
    if (sink == 0U) {
        std::printf("P4_NANO_EXACT2X_DMA2D_TIMER_CONTROL_SINK=0\n");
    }
}

bool final_valid(const std::uint8_t *source, std::uint8_t *destination,
                 const std::uint8_t *reference, std::uint32_t source_crc_before,
                 const FrameMetrics &frame) noexcept
{
    return frame.rotated_crc == exact2x::kExpectedRotatedCrc &&
        source_crc_before == exact2x::kExpectedOriginalCrc &&
        crc32(source, exact2x::kOriginalSourceBytes) == source_crc_before &&
        crc32(destination, kDestinationBytes) == exact2x::kExpectedDestinationCrc &&
        exact2x::expected_frame_matches(
            reinterpret_cast<const std::uint16_t *>(source),
            reinterpret_cast<const std::uint16_t *>(destination)) &&
        std::memcmp(destination, reference, kDestinationBytes) == 0;
}

ppa_client_handle_t create_ppa_client() noexcept
{
    const ppa_client_config_t config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    return ppa_register_client(&config, &client) == ESP_OK ? client : nullptr;
}

} // namespace

namespace p4_nano_exact2x_dma2d_benchmark {

esp_err_t run(const Input &input)
{
    const bool input_ok = input.original_source != nullptr &&
        input.original_source_bytes == exact2x::kOriginalSourceBytes &&
        esp_ptr_external_ram(input.original_source) &&
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.active_framebuffer != nullptr;
    if (!input_ok) {
        return ESP_ERR_INVALID_ARG;
    }
    std::printf("P4_NANO_EXACT2X_DMA2D_BENCHMARK_CONFIG "
                "profile=lower2 fixture=np2video-7b2d-live-vram scene=2 "
                "cpu_mhz=%u warmups=8 measured=128 final_validation=1 "
                "control=full_q0q1 dma=horizontal_q0q1 even_odd "
                "ppa_operations=5 horizontal_operations=10 dma_transactions=20 "
                "overlap=0 descriptor_chain=0\n",
                static_cast<unsigned int>(kCpuMHz));
    auto *tile = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kTileBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
        MALLOC_CAP_8BIT));
    auto *staging = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kStagingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
        MALLOC_CAP_8BIT));
    auto *destination = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes, MALLOC_CAP_SPIRAM |
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    auto *control_snapshot = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes, MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT));
    auto *reference = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes, MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT));
    const bool allocation_ok = tile != nullptr && staging != nullptr &&
        destination != nullptr && control_snapshot != nullptr && reference != nullptr &&
        esp_ptr_internal(tile) && esp_ptr_internal(staging) &&
        esp_ptr_external_ram(destination) && esp_ptr_external_ram(control_snapshot) &&
        esp_ptr_external_ram(reference) &&
        !ranges_overlap(destination, kDestinationBytes, control_snapshot,
                        kDestinationBytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.original_source,
                        input.original_source_bytes);
    if (!allocation_ok) {
        heap_caps_free(tile); heap_caps_free(staging); heap_caps_free(destination);
        heap_caps_free(control_snapshot); heap_caps_free(reference);
        return ESP_ERR_NO_MEM;
    }
    make_reference(input.original_source, reference);
    const std::uint32_t source_crc_before =
        crc32(input.original_source, exact2x::kOriginalSourceBytes);
    reset(&s_control_stats); reset(&s_dma_stats);
    bool ok = normalize_destination(destination);
    ppa_client_handle_t control_client = create_ppa_client();
    if (control_client == nullptr) { ok = false; }
    if (ok) {
        ok = run_samples([&](bool final, FrameMetrics *m) {
            return run_control_frame(control_client, input.original_source, tile,
                                     destination, final, m);
        }, true);
    }
    FrameMetrics control_final{};
    if (ok) {
        ok = run_control_frame(control_client, input.original_source, tile,
                               destination, true, &control_final) &&
            final_valid(input.original_source, destination, reference,
                        source_crc_before, control_final);
    }
    if (control_client != nullptr && ppa_unregister_client(control_client) != ESP_OK) {
        ok = false;
    }
    if (ok) {
        std::memcpy(control_snapshot, destination, kDestinationBytes);
        ok = esp_cache_msync(control_snapshot, kDestinationBytes,
                             ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                 ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
    }

    dma2d::Adapter *adapter = nullptr;
    ppa_client_handle_t dma_client = nullptr;
    if (ok && normalize_destination(destination)) {
        ok = dma2d::create(&adapter) == ESP_OK && adapter != nullptr;
        dma_client = ok ? create_ppa_client() : nullptr;
        ok = dma_client != nullptr;
    }
    if (ok) {
        ok = run_samples([&](bool final, FrameMetrics *m) {
            return run_dma_frame(dma_client, input.original_source, tile, staging,
                                 destination, adapter, final, m);
        }, false);
    }
    FrameMetrics dma_final{};
    if (ok) {
        ok = run_dma_frame(dma_client, input.original_source, tile, staging,
                           destination, adapter, true, &dma_final) &&
            final_valid(input.original_source, destination, reference,
                        source_crc_before, dma_final) &&
            std::memcmp(destination, control_snapshot, kDestinationBytes) == 0;
    }
    const bool retained = dma2d::lifetime_must_be_retained(adapter);
    if (dma_client != nullptr && ppa_unregister_client(dma_client) != ESP_OK) {
        ok = false;
    }
    if (adapter != nullptr && !retained && dma2d::destroy(adapter) != ESP_OK) {
        ok = false;
    }
    if (retained) { ok = false; }

    if (ok) {
        run_timer_control_one(&s_timer_control_stats.control, 24U, 0U);
        run_timer_control_one(&s_timer_control_stats.dma, 112U, 80U);
        print_metric("CONTROL", "PPA_COMMON_WALL", &s_control_stats.ppa_common_wall, false);
        print_metric("CONTROL", "PIE_FULL_ACTIVE", &s_control_stats.pie_full_active, false);
        print_metric("CONTROL", "CONTROL_DESTINATION_C2M", &s_control_stats.destination_c2m, false);
        print_metric("CONTROL", "CPU_UNAVAILABLE_LOWER_BOUND_PROXY", &s_control_stats.cpu_lower_bound, true);
        print_metric("CONTROL", "EXACT2X_SERVICE", &s_control_stats.exact2x_service, true);
        print_metric("CONTROL", "TOTAL_TRANSFORM_SERVICE", &s_control_stats.total_transform_service, true);
        print_metric("DMA2D", "PPA_COMMON_WALL", &s_dma_stats.ppa_common_wall, false);
        print_metric("DMA2D", "PIE_HORIZONTAL_ACTIVE", &s_dma_stats.pie_horizontal_active, false);
        print_metric("DMA2D", "DMA_COPY_TOTAL_WALL", &s_dma_stats.copy_total_wall, false);
        print_metric("DMA2D", "DMA_BLOCKED_WAIT", &s_dma_stats.blocked_wait, true);
        print_metric("DMA2D", "DMA_NONBLOCKED_TASK_WALL", &s_dma_stats.nonblocked_task_wall, false);
        print_metric("DMA2D", "PROJECT_CALLBACK_PRE_SIGNAL_CPU_DURING_WAIT", &s_dma_stats.callback_during_wait, false);
        print_metric("DMA2D", "CPU_UNAVAILABLE_LOWER_BOUND_PROXY", &s_dma_stats.cpu_lower_bound, true);
        print_metric("DMA2D", "EXACT2X_SERVICE", &s_dma_stats.exact2x_service, true);
        print_metric("DMA2D", "TOTAL_TRANSFORM_SERVICE", &s_dma_stats.total_transform_service, true);
        print_metric("CONTROL", "TIMER_CONTROL", &s_timer_control_stats.control, false);
        print_metric("DMA2D", "TIMER_CONTROL", &s_timer_control_stats.dma, false);
    }
    std::printf("P4_NANO_EXACT2X_DMA2D_BENCHMARK_ACCOUNTING "
                "control_cpu=pie_full_plus_destination_c2m "
                "dma_cpu=pie_horizontal_plus_nonblocked_plus_callback_pre_signal_during_wait "
                "private_driver_isr_cpu_outside_project_callbacks=UNMEASURED "
                "semaphore_give_wakeup_cpu=UNMEASURED_NOT_INCLUDED "
                "dma_blocked_wait=potential_schedulable_opportunity_upper_bound "
                "timer_reads_control=24 timer_reads_dma=112 cycle_reads_dma=80\n");
    std::printf("P4_NANO_EXACT2X_DMA2D_BENCHMARK_DIRECT_EQUIVALENCE "
                "reference=control byte_exact=%d result=%s\n",
                ok ? 1 : 0, ok ? "PASS" : "FAIL");
    heap_caps_free(tile); heap_caps_free(staging); heap_caps_free(destination);
    heap_caps_free(control_snapshot); heap_caps_free(reference);
    return ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_exact2x_dma2d_benchmark
