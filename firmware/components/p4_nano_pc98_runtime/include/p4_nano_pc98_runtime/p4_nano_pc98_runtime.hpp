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

/* Starts the bounded physical USB Boot Keyboard proof on P4-NANO. */
esp_err_t run_usb_keyboard_validation() noexcept;

/* Dedicated opt-in, headless P4 profile.  The existing p4_nano_pc98 task
 * remains the sole Core-1 producer; it binds the real i286 guest to audio. */
esp_err_t run_audio86_real_guest() noexcept;

} // namespace p4_nano_pc98_runtime
