#pragma once

#include <cstdint>

#include "esp_err.h"

namespace p4_nano_board {

inline constexpr std::uint8_t kDisplayControlAddress = 0x45;
inline constexpr std::uint8_t kPanelControlRegister = 0x95;
inline constexpr std::uint8_t kBacklightRegister = 0x96;
inline constexpr std::uint8_t kBacklightOff = 0x00;
inline constexpr std::uint8_t kBacklightConservative = 0x40;

/*
 * The board service is the sole production owner of P4-NANO GPIO7/GPIO8.
 * Device-specific clients never install or delete an I2C driver themselves.
 */
esp_err_t display_control_init();
esp_err_t display_control_panel_power_on();
esp_err_t display_control_write(std::uint8_t reg, std::uint8_t value);
esp_err_t display_backlight_set(std::uint8_t value);
esp_err_t display_control_safe_off();
esp_err_t display_control_deinit();
bool display_control_is_initialized();

} // namespace p4_nano_board
