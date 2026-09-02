/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h"

namespace p4_nano_audio86_physical {

/* waiter_slot remains owned by the guest runtime and must outlive the sink. */
esp_err_t create_idf(struct p4_nano_audio86_physical_sink **sink,
                     TaskHandle_t *waiter_slot) noexcept;

#if defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)
esp_err_t create_lifecycle_test(
    struct p4_nano_audio86_physical_sink **sink,
    TaskHandle_t *waiter_slot) noexcept;
bool lifecycle_test_evidence_valid() noexcept;
void emit_lifecycle_test_backend_evidence() noexcept;
#endif

} // namespace p4_nano_audio86_physical
