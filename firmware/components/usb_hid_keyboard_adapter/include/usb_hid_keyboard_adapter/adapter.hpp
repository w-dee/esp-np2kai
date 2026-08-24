#pragma once

#include <cstddef>
#include <cstdint>

#include "hid_boot_keyboard/hid_boot_keyboard.h"
#include "np2_keyboard_input/keyboard_input.hpp"

namespace usb_hid_keyboard_adapter {

inline constexpr std::size_t kMaxTransitions =
    HID_BOOT_KEYBOARD_MAX_EVENTS;
inline constexpr std::size_t kMaxNeutralEvents =
    HID_BOOT_KEYBOARD_MAX_EVENTS;

enum class TranslationStatus : std::uint8_t {
    Mapped,
    Unsupported,
    ErrorUsage,
    CapacityInsufficient,
    InvalidInput,
};

struct TranslationResult final {
    TranslationStatus status = TranslationStatus::Mapped;
    std::size_t required_events = 0;
    std::size_t written_events = 0;
    std::size_t unsupported_usages = 0;
};

class Adapter final {
public:
    Adapter() noexcept = default;

    void reset() noexcept;

    /* Translate one complete parser batch.  The modifier byte is the final
     * modifier state of that Boot report, not the value copied into each D0
     * transition.  State commits only after the output fits. */
    TranslationResult translate_report(
        const hid_boot_keyboard_event_t *transitions,
        std::size_t transition_count,
        std::uint8_t final_modifiers,
        np2_keyboard_input::Event *events,
        std::size_t capacity) noexcept;

private:
    std::uint8_t modifiers_ = 0;
};

} // namespace usb_hid_keyboard_adapter
