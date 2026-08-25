/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "p4_nano_live_display/p4_nano_ppa_rotation_reference.hpp"

namespace p4_nano_ppa_rotation {

struct Input final {
    const std::uint8_t *source = nullptr;
    std::size_t source_bytes = 0U;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::size_t source_pitch_bytes = 0U;
    std::uint32_t source_bpp = 0U;
    const void *presentation_slot0 = nullptr;
    const void *presentation_slot1 = nullptr;
    std::size_t presentation_slot_bytes = 0U;
    const void *native_framebuffer = nullptr;
    std::size_t native_framebuffer_bytes = 0U;
};

/* Runs the P9B-only blocking PPA rotation-only benchmark.  The caller must keep
 * source immutable and retain the existing isolated producer pause while
 * this function executes. */
esp_err_t run(const Input &input);

} // namespace p4_nano_ppa_rotation
