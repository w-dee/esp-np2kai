#pragma once

#include "esp_err.h"

namespace p4_nano_pc98_runtime {

/* Starts the production SDMMC-backed runtime and keeps consuming frames until
 * the core reports a fatal error or an external stop is requested. */
esp_err_t run_production() noexcept;

/* Starts the bounded SPI-NOR validation composition.  This profile is only a
 * validation wrapper; it never selects the production media path. */
esp_err_t run_validation() noexcept;

/* Starts the deterministic synthetic-keyboard validation composition. */
esp_err_t run_keyboard_validation() noexcept;

} // namespace p4_nano_pc98_runtime
