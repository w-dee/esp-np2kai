/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace p4_nano_exact2x_dma2d {

struct Input final {
    const std::uint8_t *original_source = nullptr;
    std::size_t original_source_bytes = 0U;
    const void *presentation_slot0 = nullptr;
    const void *presentation_slot1 = nullptr;
    std::size_t presentation_slot_bytes = 0U;
    const void *active_framebuffer = nullptr;
    std::size_t active_framebuffer_bytes = 0U;
};

esp_err_t run(const Input &input);

/* True when a failed/ambiguous DMA transaction retains its adapter and
 * backing buffers for a later quiescence/teardown gate. */
bool lifetime_must_be_retained() noexcept;

} // namespace p4_nano_exact2x_dma2d
