/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "p4_nano_display/p4_nano_display_transform.hpp"

namespace p4_nano_display {

esp_err_t run_transform_diagnostic(QuarterTurn rotation);

} // namespace p4_nano_display
