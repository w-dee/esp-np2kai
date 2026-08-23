/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "p4_nano_display/p4_nano_display_transform.hpp"

namespace p4_nano_display {

inline constexpr std::size_t kTransformSourceBytes =
    kTransformSourcePixelCount * sizeof(std::uint16_t);

bool fill_transform_source_pattern(std::span<std::uint16_t> pixels);
std::uint32_t transform_source_pattern_crc32(
    std::span<const std::uint16_t> pixels);

} // namespace p4_nano_display
