#pragma once

#include <cstdint>
#include <type_traits>

namespace np2_keyboard_input {

/*
 * Key is a host-neutral keyboard identity.  It is deliberately neither an
 * ASCII/Unicode character nor an SDL/HID/Windows value.  A producer reports
 * the key identity and the edge; the NP2 adapter performs the separate
 * frontend-ID mapping and (in a later slice) nkeytbl/device-code expansion.
 */
enum class Key : std::uint8_t {
    Unknown = 0,

    Escape,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Digit0,
    Minus,
    Circumflex,
    Yen,
    Backspace,
    Tab,

    Q,
    W,
    E,
    R,
    T,
    Y,
    U,
    I,
    O,
    P,
    At,
    LeftBracket,
    Enter,

    A,
    S,
    D,
    F,
    G,
    H,
    J,
    K,
    L,
    Semicolon,
    Colon,
    RightBracket,

    Z,
    X,
    C,
    V,
    B,
    N,
    M,
    Comma,
    Period,
    Slash,
    Underscore,
    Space,

    Xfer,
    RollUp,
    RollDown,
    Insert,
    Delete,
    Up,
    Left,
    Right,
    Down,
    HomeClear,
    Help,

    KeypadMinus,
    KeypadDivide,
    Keypad7,
    Keypad8,
    Keypad9,
    KeypadMultiply,
    Keypad4,
    Keypad5,
    Keypad6,
    KeypadPlus,
    Keypad1,
    Keypad2,
    Keypad3,
    KeypadEquals,
    Keypad0,
    KeypadComma,
    KeypadPeriod,
    KeypadEnter,

    Nfer,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,

    LeftShift,
    CapsLock,
    Kana,
    Grph,
    Control,
    RightShift,

    Stop,
    Copy,

    // Names used by PC-98 documentation remain available without making
    // their spelling part of the mapping implementation.
    STOP = Stop,
    COPY = Copy,
    GRPH = Grph,
    XFER = Xfer,
    NFER = Nfer,
    Caps = CapsLock,
    Ctrl = Control,
    Graph = Grph,
    HomeClr = HomeClear,
};

enum class Action : std::uint8_t {
    Press = 0,
    Release = 1,
};

/* A source is an opaque, small value owned by the composition layer.  Zero
 * is reserved as the invalid/default value; producers may use the named
 * values below or allocate another non-zero value in a later slice. */
struct SourceId final {
    std::uint8_t value = 0;

    constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(SourceId, SourceId) noexcept = default;
};

inline constexpr SourceId kSyntheticSource{1};
inline constexpr SourceId kUsbKeyboardSource{2};
inline constexpr SourceId kUartInjectionSource{3};

/*
 * A producer event has no timestamp, repeat bit, heap-backed identity, NP2
 * frontend ID, or PC-98 make/break byte.  It is only an independent edge for
 * one host-neutral key from one source.
 */
struct Event final {
    SourceId source{};
    Key key = Key::Unknown;
    Action action = Action::Press;
};

static_assert(sizeof(SourceId) == sizeof(std::uint8_t));
static_assert(sizeof(Event) == 3);
static_assert(std::is_trivially_copyable_v<SourceId>);
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);

} // namespace np2_keyboard_input
