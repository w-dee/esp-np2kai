/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace p4_nano_dma2d_copy {

struct Adapter;

inline constexpr std::size_t kChunkRows = 64U;
inline constexpr std::size_t kSourceWidthPixels = 800U;
inline constexpr std::size_t kDestinationVirtualWidthPixels = 1600U;
inline constexpr std::size_t kBytesPerPixel = 2U;
inline constexpr std::size_t kSourceBytes =
    kSourceWidthPixels * kChunkRows * kBytesPerPixel;
inline constexpr std::size_t kDestinationSpanBytes =
    kDestinationVirtualWidthPixels * kChunkRows * kBytesPerPixel;
inline constexpr std::size_t kEvenXOffsetPixels = 0U;
inline constexpr std::size_t kOddXOffsetPixels = 800U;

/* Creates one retained, profile-owned DMA2D pool client. */
esp_err_t create(Adapter **ret_adapter) noexcept;

/* Copies one 800x64 RGB565 staging chunk into every other physical row.
 * dst_x_pixels must be kEvenXOffsetPixels or kOddXOffsetPixels. */
esp_err_t copy_strided(Adapter *adapter, const std::uint8_t *source,
                       std::uint8_t *destination,
                       std::size_t dst_x_pixels) noexcept;

/* Releases the adapter only after the last transfer is quiescent. */
esp_err_t destroy(Adapter *adapter) noexcept;

/* True when teardown must be deferred because callback reachability is not
 * provably quiescent.  The caller must retain the adapter in this case. */
bool lifetime_must_be_retained(const Adapter *adapter) noexcept;

} // namespace p4_nano_dma2d_copy
