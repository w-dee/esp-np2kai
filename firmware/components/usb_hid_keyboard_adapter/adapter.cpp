#include "usb_hid_keyboard_adapter/adapter.hpp"

#include <array>
#include <optional>

namespace usb_hid_keyboard_adapter {
namespace {

using np2_keyboard_input::Action;
using np2_keyboard_input::Event;
using np2_keyboard_input::Key;

constexpr std::uint8_t kLeftControlMask = 1U << 0U;
constexpr std::uint8_t kLeftAltMask = 1U << 2U;
constexpr std::uint8_t kRightControlMask = 1U << 4U;
constexpr std::uint8_t kRightAltMask = 1U << 6U;
constexpr std::uint8_t kControlMask = kLeftControlMask | kRightControlMask;
constexpr std::uint8_t kGrphMask = kLeftAltMask | kRightAltMask;

constexpr bool is_error_kind(const hid_boot_keyboard_event_kind_t kind) noexcept
{
    return kind == HID_BOOT_EVENT_ERROR_USAGE;
}

constexpr bool is_alias_usage(const std::uint8_t usage) noexcept
{
    return usage == 0xe0U || usage == 0xe2U || usage == 0xe4U ||
           usage == 0xe6U;
}

constexpr bool is_valid_kind(const hid_boot_keyboard_event_kind_t kind) noexcept
{
    return kind == HID_BOOT_EVENT_KEY_PRESS ||
           kind == HID_BOOT_EVENT_KEY_RELEASE ||
           kind == HID_BOOT_EVENT_MOD_PRESS ||
           kind == HID_BOOT_EVENT_MOD_RELEASE ||
           kind == HID_BOOT_EVENT_ERROR_USAGE;
}

std::optional<Key> map_usage(const std::uint8_t usage) noexcept
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        return static_cast<Key>(static_cast<std::uint8_t>(Key::A) +
                                (usage - 0x04U));
    }
    if (usage >= 0x1eU && usage <= 0x27U) {
        return static_cast<Key>(static_cast<std::uint8_t>(Key::Digit1) +
                                (usage - 0x1eU));
    }
    if (usage >= 0x3aU && usage <= 0x43U) {
        return static_cast<Key>(static_cast<std::uint8_t>(Key::F1) +
                                (usage - 0x3aU));
    }

    switch (usage) {
    case 0x28U:
        return Key::Enter;
    case 0x29U:
        return Key::Escape;
    case 0x2aU:
        return Key::Backspace;
    case 0x2bU:
        return Key::Tab;
    case 0x2cU:
        return Key::Space;
    case 0x2dU:
        return Key::Minus;
    case 0x2eU:
        return Key::Circumflex;
    case 0x2fU:
        return Key::At;
    case 0x30U:
        return Key::LeftBracket;
    case 0x31U:
        return Key::RightBracket;
    case 0x33U:
        return Key::Semicolon;
    case 0x34U:
        return Key::Colon;
    case 0x36U:
        return Key::Comma;
    case 0x37U:
        return Key::Period;
    case 0x38U:
        return Key::Slash;
    case 0x39U:
        return Key::CapsLock;
    case 0x46U:
        return Key::Copy;
    case 0x48U:
        return Key::Stop;
    case 0x49U:
        return Key::Insert;
    case 0x4aU:
        return Key::HomeClear;
    case 0x4bU:
        return Key::RollUp;
    case 0x4cU:
        return Key::Delete;
    case 0x4dU:
        return Key::Help;
    case 0x4eU:
        return Key::RollDown;
    case 0x4fU:
        return Key::Right;
    case 0x50U:
        return Key::Left;
    case 0x51U:
        return Key::Down;
    case 0x52U:
        return Key::Up;
    case 0x54U:
        return Key::KeypadDivide;
    case 0x55U:
        return Key::KeypadMultiply;
    case 0x56U:
        return Key::KeypadMinus;
    case 0x57U:
        return Key::KeypadPlus;
    case 0x58U:
        return Key::KeypadEnter;
    case 0x59U:
        return Key::Keypad1;
    case 0x5aU:
        return Key::Keypad2;
    case 0x5bU:
        return Key::Keypad3;
    case 0x5cU:
        return Key::Keypad4;
    case 0x5dU:
        return Key::Keypad5;
    case 0x5eU:
        return Key::Keypad6;
    case 0x5fU:
        return Key::Keypad7;
    case 0x60U:
        return Key::Keypad8;
    case 0x61U:
        return Key::Keypad9;
    case 0x62U:
        return Key::Keypad0;
    case 0x63U:
        return Key::KeypadPeriod;
    case 0x67U:
        return Key::KeypadEquals;
    case 0x85U:
        return Key::KeypadComma;
    case 0x87U:
        return Key::Underscore;
    case 0x88U:
        return Key::Kana;
    case 0x89U:
        return Key::Yen;
    case 0x8aU:
        return Key::Xfer;
    case 0x8bU:
        return Key::Nfer;
    case 0xe1U:
        return Key::LeftShift;
    case 0xe5U:
        return Key::RightShift;
    default:
        return std::nullopt;
    }
}

bool append_event(std::array<Event, kMaxNeutralEvents> &staged,
                  std::size_t &count, const Key key,
                  const Action action) noexcept
{
    if (count >= staged.size()) {
        return false;
    }
    staged[count++] = {np2_keyboard_input::kUsbKeyboardSource, key, action};
    return true;
}

bool append_alias_edge(std::array<Event, kMaxNeutralEvents> &staged,
                       std::size_t &count, const std::uint8_t old_modifiers,
                       const std::uint8_t new_modifiers,
                       const std::uint8_t mask, const Key key) noexcept
{
    const bool old_held = (old_modifiers & mask) != 0U;
    const bool new_held = (new_modifiers & mask) != 0U;
    if (old_held == new_held) {
        return true;
    }
    return append_event(staged, count, key,
                        new_held ? Action::Press : Action::Release);
}

} // namespace

void Adapter::reset() noexcept
{
    modifiers_ = 0;
}

TranslationResult Adapter::translate_report(
    const hid_boot_keyboard_event_t *transitions,
    const std::size_t transition_count, const std::uint8_t final_modifiers,
    Event *events, const std::size_t capacity) noexcept
{
    TranslationResult result{};
    if (transition_count > kMaxTransitions ||
        (transition_count != 0U && transitions == nullptr)) {
        result.status = TranslationStatus::InvalidInput;
        return result;
    }

    bool has_error_usage = false;
    for (std::size_t i = 0; i < transition_count; ++i) {
        if (!is_valid_kind(transitions[i].kind)) {
            result.status = TranslationStatus::InvalidInput;
            return result;
        }
        if (is_error_kind(transitions[i].kind)) {
            has_error_usage = true;
        }
    }
    if (has_error_usage) {
        result.status = TranslationStatus::ErrorUsage;
        return result;
    }

    std::array<Event, kMaxNeutralEvents> staged{};
    std::size_t staged_count = 0;
    bool alias_edge_emitted = false;
    for (std::size_t i = 0; i < transition_count; ++i) {
        const auto &transition = transitions[i];
        if (is_alias_usage(transition.usage)) {
            if (!alias_edge_emitted) {
                if (!append_alias_edge(staged, staged_count, modifiers_,
                                       final_modifiers, kControlMask,
                                       Key::Control) ||
                    !append_alias_edge(staged, staged_count, modifiers_,
                                       final_modifiers, kGrphMask,
                                       Key::Grph)) {
                    result.status = TranslationStatus::InvalidInput;
                    return result;
                }
                alias_edge_emitted = true;
            }
            continue;
        }

        const auto mapped = map_usage(transition.usage);
        if (!mapped.has_value()) {
            ++result.unsupported_usages;
            continue;
        }
        const Action action =
            (transition.kind == HID_BOOT_EVENT_KEY_PRESS ||
             transition.kind == HID_BOOT_EVENT_MOD_PRESS)
                ? Action::Press
                : Action::Release;
        if (!append_event(staged, staged_count, *mapped, action)) {
            result.status = TranslationStatus::InvalidInput;
            return result;
        }
    }

    if (!alias_edge_emitted &&
        (!append_alias_edge(staged, staged_count, modifiers_, final_modifiers,
                            kControlMask, Key::Control) ||
         !append_alias_edge(staged, staged_count, modifiers_, final_modifiers,
                            kGrphMask, Key::Grph))) {
        result.status = TranslationStatus::InvalidInput;
        return result;
    }

    result.required_events = staged_count;
    if (staged_count > capacity || (staged_count != 0U && events == nullptr)) {
        result.status = TranslationStatus::CapacityInsufficient;
        return result;
    }

    if (staged_count != 0U) {
        for (std::size_t i = 0; i < staged_count; ++i) {
            events[i] = staged[i];
        }
    }
    modifiers_ = final_modifiers;
    result.written_events = staged_count;
    result.status = result.unsupported_usages == 0U
                        ? TranslationStatus::Mapped
                        : TranslationStatus::Unsupported;
    return result;
}

} // namespace usb_hid_keyboard_adapter
