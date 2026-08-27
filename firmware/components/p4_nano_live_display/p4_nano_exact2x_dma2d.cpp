/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_exact2x_dma2d.hpp"

#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include "np2video_golden.h"
#include "p4_nano_dma2d_copy/dma2d_copy.hpp"
#include "p4_nano_display/p4_nano_display_exact2x.hpp"
#include "p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"

namespace {

namespace exact2x = p4_nano_exact2x_internal_source;
namespace dma2d = p4_nano_dma2d_copy;

constexpr std::size_t kSourceWidth = 400U;
constexpr std::size_t kSourceHeight = 640U;
constexpr std::size_t kSourceBytes = kSourceWidth * kSourceHeight * 2U;
constexpr std::size_t kTileRows = 128U;
constexpr std::size_t kTileBytes = 102400U;
constexpr std::size_t kTileDestinationBytes = 409600U;
constexpr std::size_t kTileCount = 5U;
constexpr std::size_t kStagingBytes = 102400U;
constexpr std::size_t kChunkBytes = 51200U;
constexpr std::size_t kChunkDestinationDelta = 204800U;
constexpr std::size_t kDestinationBytes = 2048000U;
constexpr std::size_t kRequiredAlignment = 64U;
constexpr std::uint32_t kExpectedSourceCrc = 0x8dadbf82U;
constexpr std::uint32_t kExpectedRotatedCrc = 0x379511d7U;
constexpr std::uint32_t kExpectedDestinationCrc = 0xc8a10b55U;

static_assert(kTileCount * kTileDestinationBytes == kDestinationBytes);
static_assert(kTileBytes == kStagingBytes);
static_assert(kChunkBytes * 2U == kStagingBytes);
static_assert(kChunkDestinationDelta * 2U == kTileDestinationBytes);

struct RetainedResources final {
    dma2d::Adapter *adapter = nullptr;
    std::uint8_t *tile = nullptr;
    std::uint8_t *staging = nullptr;
    std::uint8_t *destination = nullptr;
};

RetainedResources s_retained_resources{};

void retain_resources(dma2d::Adapter *adapter, std::uint8_t *tile,
                      std::uint8_t *staging,
                      std::uint8_t *destination) noexcept
{
    if (s_retained_resources.adapter != nullptr) {
        return;
    }
    s_retained_resources = RetainedResources{
        .adapter = adapter,
        .tile = tile,
        .staging = staging,
        .destination = destination,
    };
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

std::uint32_t crc32(const std::uint8_t *bytes, std::size_t length) noexcept
{
    return exact2x::crc32_update(0xffffffffU, bytes, length) ^ 0xffffffffU;
}

void print_memory_preflight(const char *stage) noexcept
{
    const std::size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const std::size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const std::size_t dma_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const std::size_t dma_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    std::printf("P4_NANO_EXACT2X_DMA2D_MEMORY stage=%s internal_free=%zu "
                "internal_largest=%zu dma_internal_free=%zu "
                "dma_internal_largest=%zu\n",
                stage, internal_free, internal_largest, dma_free, dma_largest);
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

bool horizontal_matches(const std::uint8_t *source,
                        const std::uint8_t *staging) noexcept
{
    const auto *source_pixels = reinterpret_cast<const std::uint16_t *>(source);
    const auto *staging_pixels = reinterpret_cast<const std::uint16_t *>(staging);
    for (std::size_t y = 0U; y < 64U; ++y) {
        for (std::size_t x = 0U; x < 400U; ++x) {
            const std::uint16_t pixel = source_pixels[y * 400U + x];
            if (staging_pixels[y * 800U + 2U * x] != pixel ||
                staging_pixels[y * 800U + 2U * x + 1U] != pixel) {
                return false;
            }
        }
    }
    return true;
}

bool prepare_tile(ppa_client_handle_t client, const std::uint8_t *original,
                  std::uint8_t *tile, std::size_t tile_index) noexcept
{
    const ppa_srm_oper_config_t operation = exact2x::make_tile_operation(
        original, tile, tile_index, PPA_TRANS_MODE_BLOCKING, nullptr);
    return ppa_do_scale_rotate_mirror(client, &operation) == ESP_OK;
}

enum class FailureStage : std::uint8_t {
    None,
    Input,
    Ppa,
    Horizontal,
    DmaEven,
    DmaOdd,
};

struct PipelineResult final {
    bool success = false;
    FailureStage failure_stage = FailureStage::None;
    std::size_t tile_index = 0U;
    std::size_t chunk_index = 0U;
    esp_err_t status = ESP_OK;
    std::uint32_t rotated_crc = 0xffffffffU;
};

PipelineResult pipeline_failure(FailureStage stage, std::size_t tile_index,
                                std::size_t chunk_index,
                                esp_err_t status) noexcept
{
    return PipelineResult{
        .success = false,
        .failure_stage = stage,
        .tile_index = tile_index,
        .chunk_index = chunk_index,
        .status = status,
        .rotated_crc = 0xffffffffU,
    };
}

PipelineResult run_pipeline(ppa_client_handle_t client,
                            const std::uint8_t *original,
                            std::uint8_t *tile, std::uint8_t *staging,
                            std::uint8_t *destination,
                            dma2d::Adapter *adapter) noexcept
{
    PipelineResult result{};
    for (std::size_t tile_index = 0U; tile_index < kTileCount; ++tile_index) {
        if (!prepare_tile(client, original, tile, tile_index)) {
            return pipeline_failure(FailureStage::Ppa, tile_index, 0U,
                                    ESP_FAIL);
        }
        result.rotated_crc = exact2x::crc32_update(
            result.rotated_crc, tile, kTileBytes);
        for (std::size_t chunk = 0U; chunk < 2U; ++chunk) {
            const auto *chunk_source = tile + chunk * kChunkBytes;
            if ((reinterpret_cast<std::uintptr_t>(chunk_source) & 0xFU) != 0U) {
                return pipeline_failure(FailureStage::Horizontal, tile_index,
                                        chunk, ESP_ERR_INVALID_ARG);
            }
            p4_nano_display::exact2x_pie_horizontal64_aligned(
                reinterpret_cast<const std::uint16_t *>(chunk_source),
                reinterpret_cast<std::uint16_t *>(staging));
            if (!horizontal_matches(chunk_source, staging)) {
                return pipeline_failure(FailureStage::Horizontal, tile_index,
                                        chunk, ESP_FAIL);
            }
            auto *chunk_destination = destination +
                tile_index * kTileDestinationBytes +
                chunk * kChunkDestinationDelta;
            const esp_err_t even_result = dma2d::copy_strided(
                adapter, staging, chunk_destination,
                dma2d::kEvenXOffsetPixels);
            if (even_result != ESP_OK) {
                return pipeline_failure(FailureStage::DmaEven, tile_index,
                                        chunk, even_result);
            }
            const esp_err_t odd_result = dma2d::copy_strided(
                adapter, staging, chunk_destination,
                dma2d::kOddXOffsetPixels);
            if (odd_result != ESP_OK) {
                return pipeline_failure(FailureStage::DmaOdd, tile_index,
                                        chunk, odd_result);
            }
        }
    }
    result.success = true;
    return result;
}

const char *failure_stage_name(FailureStage stage) noexcept
{
    switch (stage) {
    case FailureStage::Input:
        return "input";
    case FailureStage::Ppa:
        return "ppa";
    case FailureStage::Horizontal:
        return "horizontal";
    case FailureStage::DmaEven:
        return "dma_even";
    case FailureStage::DmaOdd:
        return "dma_odd";
    case FailureStage::None:
    default:
        return "none";
    }
}

const char *failure_parity_name(FailureStage stage) noexcept
{
    return stage == FailureStage::DmaOdd ? "odd" :
           stage == FailureStage::DmaEven ? "even" : "none";
}

const char *failure_reason(const PipelineResult &result,
                           bool retained) noexcept
{
    if (retained && (result.failure_stage == FailureStage::DmaEven ||
                     result.failure_stage == FailureStage::DmaOdd)) {
        return result.status == ESP_ERR_TIMEOUT ?
            "dma_timeout_retained" : "dma_setup_failure_retained";
    }
    return result.failure_stage == FailureStage::DmaEven ||
           result.failure_stage == FailureStage::DmaOdd ?
        "dma_failure_clean" : failure_stage_name(result.failure_stage);
}

void print_pipeline_failure(const PipelineResult &result,
                            bool retained) noexcept
{
    std::printf("P4_NANO_EXACT2X_DMA2D_FAILURE stage=%s tile_index=%zu "
                "chunk_index=%zu parity=%s status=%d reason=%s retained=%d\n",
                failure_stage_name(result.failure_stage), result.tile_index,
                result.chunk_index, failure_parity_name(result.failure_stage),
                static_cast<int>(result.status),
                failure_reason(result, retained), retained ? 1 : 0);
}

} // namespace

namespace p4_nano_exact2x_dma2d {

bool lifetime_must_be_retained() noexcept
{
    return s_retained_resources.adapter != nullptr;
}

esp_err_t run(const Input &input)
{
    if (lifetime_must_be_retained()) {
        std::printf("P4_NANO_EXACT2X_DMA2D_FAILURE stage=reentry "
                    "tile_index=0 chunk_index=0 parity=none status=%d "
                    "reason=retained_reentry retained=1\n",
                    static_cast<int>(ESP_ERR_INVALID_STATE));
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL "
                    "reason=retained_reentry\n");
        return ESP_ERR_INVALID_STATE;
    }
    const bool input_valid = input.original_source != nullptr &&
        input.original_source_bytes == kSourceBytes &&
        esp_ptr_external_ram(input.original_source) &&
        input.presentation_slot0 != nullptr && input.presentation_slot1 != nullptr &&
        input.presentation_slot_bytes != 0U && input.active_framebuffer != nullptr &&
        input.active_framebuffer_bytes >= kDestinationBytes;
    std::printf("P4_NANO_EXACT2X_DMA2D_CONFIG idf=5.5.4 chunk_rows=64 "
                "ppa_tile_bytes=102400 staging_bytes=102400 "
                "destination_bytes=2048000 ppa_scale=1 "
                "horizontal_pie_qr=q0,q1 dma_even_x=0 dma_odd_x=800 "
                "dma_virtual_dst_width=1600 descriptor_chain=0 overlap=0\n");
    if (!input_valid) {
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL reason=input\n");
        return ESP_ERR_INVALID_ARG;
    }

    print_memory_preflight("before_alloc");
    auto *tile = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kTileBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    auto *staging = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kStagingBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    auto *destination = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    const bool allocations_valid = tile != nullptr && staging != nullptr &&
        destination != nullptr && esp_ptr_internal(tile) &&
        esp_ptr_internal(staging) && esp_ptr_external_ram(destination) &&
        (reinterpret_cast<std::uintptr_t>(tile) % kRequiredAlignment) == 0U &&
        (reinterpret_cast<std::uintptr_t>(staging) % kRequiredAlignment) == 0U &&
        (reinterpret_cast<std::uintptr_t>(destination) % kRequiredAlignment) == 0U &&
        !ranges_overlap(tile, kTileBytes, staging, kStagingBytes) &&
        !ranges_overlap(tile, kTileBytes, input.original_source,
                        input.original_source_bytes) &&
        !ranges_overlap(staging, kStagingBytes, input.original_source,
                        input.original_source_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.original_source,
                        input.original_source_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.presentation_slot0,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.presentation_slot1,
                        input.presentation_slot_bytes) &&
        !ranges_overlap(destination, kDestinationBytes, input.active_framebuffer,
                        input.active_framebuffer_bytes);
    std::printf("P4_NANO_EXACT2X_DMA2D_MEMORY allocation tile=%p staging=%p "
                "destination=%p tile_internal=%d staging_internal=%d "
                "destination_external=%d working_set_internal=204800 "
                "disjoint=%s\n", static_cast<void *>(tile),
                static_cast<void *>(staging), static_cast<void *>(destination),
                tile != nullptr && esp_ptr_internal(tile) ? 1 : 0,
                staging != nullptr && esp_ptr_internal(staging) ? 1 : 0,
                destination != nullptr && esp_ptr_external_ram(destination) ? 1 : 0,
                allocations_valid ? "PASS" : "FAIL");
    if (!allocations_valid) {
        heap_caps_free(tile);
        heap_caps_free(staging);
        heap_caps_free(destination);
        std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=PASS state=Idle\n");
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL reason=allocation\n");
        return ESP_ERR_NO_MEM;
    }
    print_memory_preflight("after_alloc");

    dma2d::Adapter *adapter = nullptr;
    esp_err_t result = dma2d::create(&adapter);
    if (result != ESP_OK) {
        heap_caps_free(tile);
        heap_caps_free(staging);
        heap_caps_free(destination);
        std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=PASS state=Idle\n");
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL reason=adapter_create\n");
        return result;
    }

    const ppa_client_config_t client_config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_handle_t client = nullptr;
    result = ppa_register_client(&client_config, &client);
    if (result != ESP_OK || client == nullptr) {
        const esp_err_t destroy_result = dma2d::destroy(adapter);
        if (destroy_result != ESP_OK) {
            (void)retain_resources(adapter, tile, staging, destination);
            std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=RETAINED "
                        "state=ReleaseFailed\n");
        } else {
            heap_caps_free(tile);
            heap_caps_free(staging);
            heap_caps_free(destination);
            std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=PASS "
                        "state=Idle\n");
        }
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL "
                    "reason=ppa_register\n");
        return result == ESP_OK ? ESP_FAIL : result;
    }
    std::printf("P4_NANO_EXACT2X_DMA2D_PPA status=START rotation=CCW90 "
                "scale=1 tile_count=5 queue_depth=1\n");
    std::printf("P4_NANO_EXACT2X_DMA2D_HORIZONTAL status=START rows=64 "
                "groups_per_row=50 source_offset=0,51200 q_registers=q0,q1\n");
    std::printf("P4_NANO_EXACT2X_DMA2D_DMA status=START parity_passes=2 "
                "transactions_per_tile=4 destination_pitch=3200\n");

    const std::uint32_t source_crc_before =
        crc32(input.original_source, kSourceBytes);
    PipelineResult pipeline = source_crc_before == kExpectedSourceCrc ?
        run_pipeline(client, input.original_source, tile, staging, destination,
                     adapter) :
        pipeline_failure(FailureStage::Input, 0U, 0U, ESP_ERR_INVALID_CRC);
    const std::uint32_t rotated_crc =
        pipeline.rotated_crc ^ 0xffffffffU;
    const esp_err_t unregister_result = ppa_unregister_client(client);
    const bool retain = dma2d::lifetime_must_be_retained(adapter);
    if (!pipeline.success) {
        print_pipeline_failure(pipeline, retain);
    }
    std::printf("P4_NANO_EXACT2X_DMA2D_PPA result=%s rotated_crc=0x%08" PRIx32
                " expected=0x%08" PRIx32 "\n",
                pipeline.success ? "PASS" : "FAIL", rotated_crc,
                kExpectedRotatedCrc);
    std::printf("P4_NANO_EXACT2X_DMA2D_HORIZONTAL result=%s\n",
                pipeline.success ? "PASS" : "FAIL");
    std::printf("P4_NANO_EXACT2X_DMA2D_DMA result=%s\n",
                pipeline.success ? "PASS" : "FAIL");

    if (retain) {
        std::printf("P4_NANO_EXACT2X_DMA2D_CORRECTNESS_SKIPPED "
                    "reason=ambiguous_dma_ownership\n");
        (void)retain_resources(adapter, tile, staging, destination);
        std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=RETAINED "
                    "state=RetainedAmbiguous\n");
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL "
                    "reason=ambiguous_dma_ownership\n");
        return ESP_FAIL;
    }

    if (!pipeline.success || unregister_result != ESP_OK) {
        std::printf("P4_NANO_EXACT2X_DMA2D_CORRECTNESS_SKIPPED "
                    "reason=pipeline_abort\n");
        const esp_err_t destroy_result = dma2d::destroy(adapter);
        if (destroy_result != ESP_OK) {
            (void)retain_resources(adapter, tile, staging, destination);
            std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=RETAINED "
                        "state=ReleaseFailed\n");
        } else {
            heap_caps_free(tile);
            heap_caps_free(staging);
            heap_caps_free(destination);
            std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=PASS "
                        "state=Idle\n");
        }
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL "
                    "reason=pipeline_abort\n");
        return ESP_FAIL;
    }

    bool destination_sync_ok =
        esp_cache_msync(destination, kDestinationBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
    const std::uint32_t source_crc_after =
        crc32(input.original_source, kSourceBytes);
    const std::uint32_t destination_crc =
        destination_sync_ok ? crc32(destination, kDestinationBytes) : 0U;
    auto *reference = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool reference_ok = reference != nullptr &&
        esp_ptr_external_ram(reference);
    if (reference_ok) {
        make_reference(input.original_source, reference);
    }
    const bool pixel_mapping_ok = destination_sync_ok &&
        exact2x::expected_frame_matches(
            reinterpret_cast<const std::uint16_t *>(input.original_source),
            reinterpret_cast<const std::uint16_t *>(destination));
    const bool reference_memcmp_ok = reference_ok && destination_sync_ok &&
        std::memcmp(destination, reference, kDestinationBytes) == 0;
    const bool correctness_ok = pipeline.success &&
        source_crc_after == source_crc_before &&
        source_crc_before == kExpectedSourceCrc &&
        rotated_crc == kExpectedRotatedCrc &&
        destination_crc == kExpectedDestinationCrc && pixel_mapping_ok &&
        reference_memcmp_ok;
    std::printf("P4_NANO_EXACT2X_DMA2D_CORRECTNESS source_crc_before=0x%08" PRIx32
                " source_crc_after=0x%08" PRIx32
                " rotated_crc=0x%08" PRIx32
                " destination_crc=0x%08" PRIx32
                " pixel_mapping=%s reference_memcmp=%s expected_crc=%s\n",
                source_crc_before, source_crc_after, rotated_crc,
                destination_crc, pixel_mapping_ok ? "PASS" : "FAIL",
                reference_memcmp_ok ? "PASS" : "FAIL",
                destination_crc == kExpectedDestinationCrc ? "PASS" : "FAIL");
    heap_caps_free(reference);

    const esp_err_t destroy_result = dma2d::destroy(adapter);
    if (destroy_result != ESP_OK) {
        (void)retain_resources(adapter, tile, staging, destination);
        std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=RETAINED "
                    "state=ReleaseFailed\n");
        std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=FAIL "
                    "reason=cleanup_retained\n");
        return ESP_FAIL;
    }
    heap_caps_free(tile);
    heap_caps_free(staging);
    heap_caps_free(destination);
    std::printf("P4_NANO_EXACT2X_DMA2D_CLEANUP result=PASS state=Idle\n");
    std::printf("P4_NANO_EXACT2X_DMA2D_RESULT=%s\n",
                correctness_ok ? "PASS" : "FAIL");
    return correctness_ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_exact2x_dma2d
