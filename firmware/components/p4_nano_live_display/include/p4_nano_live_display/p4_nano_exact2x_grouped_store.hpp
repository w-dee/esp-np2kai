/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace p4_nano_exact2x_grouped_store {

struct Input final {
    const std::uint8_t *original_source = nullptr;
    std::size_t original_source_bytes = 0U;
    const void *presentation_slot0 = nullptr;
    const void *presentation_slot1 = nullptr;
    std::size_t presentation_slot_bytes = 0U;
    const void *active_framebuffer = nullptr;
    std::size_t active_framebuffer_bytes = 0U;
};

/* Runs the isolated same-binary current-PIE versus grouped64-PIE benchmark.
 * Both phases prepare internal T128 tiles with the same blocking PPA path. */
esp_err_t run(const Input &input);

} // namespace p4_nano_exact2x_grouped_store
