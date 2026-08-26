/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"

namespace p4_nano_pie_preemption {

/* Runs the headless P10K-B0 grouped-PIE correctness harness. */
esp_err_t run();

} // namespace p4_nano_pie_preemption
