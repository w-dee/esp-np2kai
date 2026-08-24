#include <array>
#include <cassert>
#include <cstdint>
#include <type_traits>

#include "np2_keyboard_input/keyboard_input.hpp"
#include "np2_keyboard_input/np2_key_mapping.hpp"

namespace {

using np2_keyboard_input::Action;
using np2_keyboard_input::Event;
using np2_keyboard_input::Key;
using np2_keyboard_input::SourceId;
using np2_keyboard_input::mapping::to_frontend_key_id;

struct MappingCase {
    Key key;
    std::uint8_t expected;
};

void test_event_contract()
{
    static_assert(std::is_trivially_copyable_v<SourceId>);
    static_assert(std::is_trivially_copyable_v<Event>);
    static_assert(std::is_standard_layout_v<Event>);
    static_assert(std::is_same_v<decltype(Event::source), SourceId>);
    static_assert(std::is_same_v<decltype(Event::key), Key>);
    static_assert(std::is_same_v<decltype(Event::action), Action>);
    static_assert(sizeof(Event) == 3);

    constexpr Event sequence[] = {
        {np2_keyboard_input::kSyntheticSource, Key::LeftShift, Action::Press},
        {np2_keyboard_input::kSyntheticSource, Key::A, Action::Press},
        {np2_keyboard_input::kSyntheticSource, Key::A, Action::Release},
        {np2_keyboard_input::kSyntheticSource, Key::LeftShift, Action::Release},
    };
    static_assert(sequence[0].key == Key::LeftShift);
    static_assert(sequence[0].action == Action::Press);
    static_assert(sequence[1].key == Key::A);
    static_assert(sequence[2].action == Action::Release);
    static_assert(sequence[3].key == Key::LeftShift);

    assert(sequence[0].source == np2_keyboard_input::kSyntheticSource);
    assert(np2_keyboard_input::kSyntheticSource.valid());
    assert(!SourceId{}.valid());
}

void test_representative_mapping()
{
    // Values are the NP2 frontend IDs, not PC-98 make/break bytes.
    constexpr std::array cases{
        MappingCase{Key::A, 0x1d},
        MappingCase{Key::Enter, 0x1c},
        MappingCase{Key::KeypadEnter, 0x1c},
        MappingCase{Key::Space, 0x34},
        MappingCase{Key::LeftShift, 0x70},
        MappingCase{Key::RightShift, 0x7d},
        MappingCase{Key::Control, 0x74},
        MappingCase{Key::Stop, 0x60},
        MappingCase{Key::Copy, 0x61},
        MappingCase{Key::Grph, 0x73},
        MappingCase{Key::Xfer, 0x35},
        MappingCase{Key::Nfer, 0x51},
        MappingCase{Key::CapsLock, 0x71},
        MappingCase{Key::Kana, 0x72},
        MappingCase{Key::F1, 0x62},
        MappingCase{Key::F10, 0x6b},
        MappingCase{Key::Keypad7, 0x42},
        MappingCase{Key::HomeClear, 0x3e},
        MappingCase{Key::Help, 0x3f},
        MappingCase{Key::RollUp, 0x36},
        MappingCase{Key::RollDown, 0x37},
    };

    for (const auto &entry : cases) {
        const auto mapped = to_frontend_key_id(entry.key);
        assert(mapped.has_value());
        assert(static_cast<std::uint8_t>(*mapped) == entry.expected);
    }

    // Keep the ordinary key expansion shape available for future producers.
    assert(static_cast<std::uint8_t>(to_frontend_key_id(Key::Z).value()) == 0x29);
    assert(static_cast<std::uint8_t>(to_frontend_key_id(Key::Digit0).value()) == 0x0a);
    assert(static_cast<std::uint8_t>(to_frontend_key_id(Key::F5).value()) == 0x66);
}

void test_unsupported_and_layer_separation()
{
    assert(!to_frontend_key_id(Key::Unknown).has_value());
    assert(!to_frontend_key_id(static_cast<Key>(0xff)).has_value());

    constexpr Event press{
        np2_keyboard_input::kSyntheticSource, Key::A, Action::Press};
    constexpr Event release{
        np2_keyboard_input::kSyntheticSource, Key::A, Action::Release};
    static_assert(press.key == release.key);
    static_assert(press.action != release.action);
    // The producer event exposes Key, never FrontendKeyId or a raw byte.
    static_assert(std::is_same_v<decltype(press.key), Key>);
}

} // namespace

int main()
{
    test_event_contract();
    test_representative_mapping();
    test_unsupported_and_layer_separation();
    return 0;
}
