#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "hid_boot_keyboard/hid_boot_keyboard.h"
#include "np2_keyboard_input/keyboard_input.hpp"
#include "usb_hid_keyboard_adapter/adapter.hpp"

namespace {

using usb_hid_keyboard_adapter::Adapter;
using usb_hid_keyboard_adapter::TranslationStatus;
using np2_keyboard_input::Action;
using np2_keyboard_input::Event;
using np2_keyboard_input::Key;

constexpr std::size_t kEventCapacity =
    usb_hid_keyboard_adapter::kMaxNeutralEvents;

void fill_report(std::uint8_t *report, const std::uint8_t modifiers,
                 const std::uint8_t usage = 0U)
{
    std::memset(report, 0, HID_BOOT_KEYBOARD_REPORT_SIZE);
    report[0] = modifiers;
    report[2] = usage;
}

struct Fixture final {
    hid_boot_keyboard_state_t parser{};
    Adapter adapter{};
    std::array<Event, kEventCapacity> events{};

    Fixture() noexcept { hid_boot_keyboard_init(&parser); }

    usb_hid_keyboard_adapter::TranslationResult feed(
        const std::uint8_t *report, const std::size_t capacity = kEventCapacity)
    {
        std::array<hid_boot_keyboard_event_t, HID_BOOT_KEYBOARD_MAX_EVENTS>
            transitions{};
        const std::size_t transition_count = hid_boot_keyboard_process(
            &parser, report, HID_BOOT_KEYBOARD_REPORT_SIZE, transitions.data(),
            transitions.size());
        return adapter.translate_report(
            transitions.data(), transition_count, report[0], events.data(),
            capacity);
    }
};

void expect_event(const Event &event, const Key key, const Action action)
{
    assert(event.source == np2_keyboard_input::kUsbKeyboardSource);
    assert(event.key == key);
    assert(event.action == action);
}

void expect_press_release(const std::uint8_t usage, const Key key)
{
    Fixture fixture;
    std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    fill_report(report, 0, usage);
    auto result = fixture.feed(report);
    assert(result.status == TranslationStatus::Mapped);
    assert(result.required_events == 1);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], key, Action::Press);

    fill_report(report, 0);
    result = fixture.feed(report);
    assert(result.status == TranslationStatus::Mapped);
    assert(result.required_events == 1);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], key, Action::Release);
}

void test_supported_usage_mapping()
{
    for (std::uint8_t usage = 0x04; usage <= 0x1d; ++usage) {
        expect_press_release(
            usage, static_cast<Key>(static_cast<std::uint8_t>(Key::A) +
                                    usage - 0x04U));
    }
    for (std::uint8_t usage = 0x1e; usage <= 0x27; ++usage) {
        expect_press_release(
            usage, static_cast<Key>(static_cast<std::uint8_t>(Key::Digit1) +
                                    usage - 0x1eU));
    }
    for (std::uint8_t usage = 0x3a; usage <= 0x43; ++usage) {
        expect_press_release(
            usage, static_cast<Key>(static_cast<std::uint8_t>(Key::F1) +
                                    usage - 0x3aU));
    }

    const std::initializer_list<std::pair<std::uint8_t, Key>> supported{
        {0x28, Key::Enter},
        {0x29, Key::Escape},
        {0x2a, Key::Backspace},
        {0x2b, Key::Tab},
        {0x2c, Key::Space},
        {0x2d, Key::Minus},
        {0x2e, Key::Circumflex},
        {0x2f, Key::At},
        {0x30, Key::LeftBracket},
        {0x31, Key::RightBracket},
        {0x33, Key::Semicolon},
        {0x34, Key::Colon},
        {0x36, Key::Comma},
        {0x37, Key::Period},
        {0x38, Key::Slash},
        {0x39, Key::CapsLock},
        {0x46, Key::Copy},
        {0x48, Key::Stop},
        {0x49, Key::Insert},
        {0x4a, Key::HomeClear},
        {0x4b, Key::RollUp},
        {0x4c, Key::Delete},
        {0x4d, Key::Help},
        {0x4e, Key::RollDown},
        {0x4f, Key::Right},
        {0x50, Key::Left},
        {0x51, Key::Down},
        {0x52, Key::Up},
        {0x54, Key::KeypadDivide},
        {0x55, Key::KeypadMultiply},
        {0x56, Key::KeypadMinus},
        {0x57, Key::KeypadPlus},
        {0x58, Key::KeypadEnter},
        {0x59, Key::Keypad1},
        {0x5a, Key::Keypad2},
        {0x5b, Key::Keypad3},
        {0x5c, Key::Keypad4},
        {0x5d, Key::Keypad5},
        {0x5e, Key::Keypad6},
        {0x5f, Key::Keypad7},
        {0x60, Key::Keypad8},
        {0x61, Key::Keypad9},
        {0x62, Key::Keypad0},
        {0x63, Key::KeypadPeriod},
        {0x67, Key::KeypadEquals},
        {0x85, Key::KeypadComma},
        {0x87, Key::Underscore},
        {0x88, Key::Kana},
        {0x89, Key::Yen},
        {0x8a, Key::Xfer},
        {0x8b, Key::Nfer},
    };
    for (const auto &[usage, key] : supported) {
        expect_press_release(usage, key);
    }
}

void test_shift_and_alias_edges()
{
    Fixture fixture;
    std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];

    fill_report(report, 0x02);
    auto result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::LeftShift, Action::Press);
    fill_report(report, 0x00);
    result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::LeftShift, Action::Release);

    fill_report(report, 0x01);
    result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Press);
    fill_report(report, 0x11);
    result = fixture.feed(report);
    assert(result.written_events == 0);
    fill_report(report, 0x10);
    result = fixture.feed(report);
    assert(result.written_events == 0);
    fill_report(report, 0x00);
    result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Release);

    /* Both aliases changing in one report produce exactly one edge. */
    fixture = Fixture{};
    fill_report(report, 0x11);
    result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Press);
    fill_report(report, 0x00);
    result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Release);
}

void test_alias_handoffs()
{
    const auto check_handoff = [](const std::uint8_t first,
                                  const std::uint8_t second,
                                  const Key key) {
        Fixture fixture;
        std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
        fill_report(report, first);
        assert(fixture.feed(report).written_events == 1);
        expect_event(fixture.events[0], key, Action::Press);
        fill_report(report, second);
        assert(fixture.feed(report).written_events == 0);
        fill_report(report, 0);
        assert(fixture.feed(report).written_events == 1);
        expect_event(fixture.events[0], key, Action::Release);
    };

    check_handoff(0x01, 0x10, Key::Control);
    check_handoff(0x10, 0x01, Key::Control);
    check_handoff(0x04, 0x40, Key::Grph);
    check_handoff(0x40, 0x04, Key::Grph);
}

void test_unsupported_and_error_usage()
{
    constexpr std::array<std::uint8_t, 11> unsupported{
        0x32, 0x35, 0x44, 0x45, 0x47, 0x53, 0x64, 0x65, 0x66, 0xe3,
        0xe7};
    for (const auto usage : unsupported) {
        Fixture fixture;
        std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
        fill_report(report, 0, usage);
        const auto result = fixture.feed(report);
        assert(result.status == TranslationStatus::Unsupported);
        assert(result.written_events == 0);
        assert(result.unsupported_usages == 1);
    }

    const std::array<std::uint8_t, 3> errors{0x01, 0x02, 0x03};
    for (const auto usage : errors) {
        Fixture fixture;
        std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
        fill_report(report, 0, 0x04);
        assert(fixture.feed(report).written_events == 1);
        fill_report(report, 0xfe, usage);
        const auto result = fixture.feed(report);
        assert(result.status == TranslationStatus::ErrorUsage);
        assert(result.written_events == 0);
        fill_report(report, 0, 0);
        assert(fixture.feed(report).written_events == 1);
        expect_event(fixture.events[0], Key::A, Action::Release);
    }

    /* An error report is transactional for alias state as well. */
    Fixture alias_fixture;
    std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    fill_report(report, 0x01);
    assert(alias_fixture.feed(report).written_events == 1);
    expect_event(alias_fixture.events[0], Key::Control, Action::Press);
    fill_report(report, 0xfe, 0x01);
    const auto alias_error = alias_fixture.feed(report);
    assert(alias_error.status == TranslationStatus::ErrorUsage);
    assert(alias_error.written_events == 0);
    fill_report(report, 0);
    assert(alias_fixture.feed(report).written_events == 1);
    expect_event(alias_fixture.events[0], Key::Control, Action::Release);
}

void test_capacity_transactionality()
{
    Fixture fixture;
    std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    fill_report(report, 0xff);
    for (std::size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        report[2U + i] = static_cast<std::uint8_t>(0x04U + i);
    }
    assert(fixture.feed(report).written_events == 10);

    fill_report(report, 0, 0);
    fixture.events.fill(Event{np2_keyboard_input::kUsbKeyboardSource,
                              Key::Copy, Action::Press});
    const auto sentinel = fixture.events;
    const auto result = fixture.feed(report, 9);
    assert(result.status == TranslationStatus::CapacityInsufficient);
    assert(result.required_events == 10);
    assert(result.written_events == 0);
    assert(std::memcmp(fixture.events.data(), sentinel.data(),
                       sizeof(fixture.events)) == 0);

    const auto retry = fixture.feed(report);
    assert(retry.status == TranslationStatus::Mapped);
    assert(retry.written_events == retry.required_events);
}

void test_reset_and_raw_report_integration()
{
    Fixture fixture;
    std::uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    fill_report(report, 0x01);
    assert(fixture.feed(report).written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Press);

    fixture.adapter.reset();
    hid_boot_keyboard_init(&fixture.parser);
    fill_report(report, 0x10);
    assert(fixture.feed(report).written_events == 1);
    expect_event(fixture.events[0], Key::Control, Action::Press);

    fixture = Fixture{};
    fill_report(report, 0, 0x04);
    const auto result = fixture.feed(report);
    assert(result.written_events == 1);
    expect_event(fixture.events[0], Key::A, Action::Press);
    fill_report(report, 0);
    assert(fixture.feed(report).written_events == 1);
    expect_event(fixture.events[0], Key::A, Action::Release);
}

void test_special_integration()
{
    const std::initializer_list<std::pair<std::uint8_t, Key>> cases{
        {0x46, Key::Copy},
        {0x48, Key::Stop},
        {0x88, Key::Kana},
        {0x89, Key::Yen},
        {0x8a, Key::Xfer},
        {0x8b, Key::Nfer},
    };
    for (const auto &[usage, key] : cases) {
        expect_press_release(usage, key);
    }
}

} // namespace

int main()
{
    test_supported_usage_mapping();
    test_shift_and_alias_edges();
    test_alias_handoffs();
    test_unsupported_and_error_usage();
    test_capacity_transactionality();
    test_reset_and_raw_report_integration();
    test_special_integration();
    return 0;
}
