#pragma once

#include <cstdint>
#include <optional>

#include "np2_keyboard_input/keyboard_input.hpp"

namespace np2_keyboard_input::mapping {

/*
 * This is an adapter-facing strong type, not a producer event value.  These
 * values are NP2 *frontend key IDs* (the refs accepted by keystat_keydown/up
 * in the later adapter).  They are not PC-98 device make/break bytes:
 *
 *   Key -> FrontendKeyId -> nkeytbl mapping -> PC-98 key code(s)
 *
 * The optional result makes an unsupported Key explicit; no arbitrary enum
 * value is ever converted into an NP2 ID.
 */
enum class FrontendKeyId : std::uint8_t {
    Escape = 0x00,
    Digit1 = 0x01,
    Digit2 = 0x02,
    Digit3 = 0x03,
    Digit4 = 0x04,
    Digit5 = 0x05,
    Digit6 = 0x06,
    Digit7 = 0x07,
    Digit8 = 0x08,
    Digit9 = 0x09,
    Digit0 = 0x0a,
    Minus = 0x0b,
    Circumflex = 0x0c,
    Yen = 0x0d,
    Backspace = 0x0e,
    Tab = 0x0f,
    Q = 0x10,
    W = 0x11,
    E = 0x12,
    R = 0x13,
    T = 0x14,
    Y = 0x15,
    U = 0x16,
    I = 0x17,
    O = 0x18,
    P = 0x19,
    At = 0x1a,
    LeftBracket = 0x1b,
    Enter = 0x1c,
    A = 0x1d,
    S = 0x1e,
    D = 0x1f,
    F = 0x20,
    G = 0x21,
    H = 0x22,
    J = 0x23,
    K = 0x24,
    L = 0x25,
    Semicolon = 0x26,
    Colon = 0x27,
    RightBracket = 0x28,
    Z = 0x29,
    X = 0x2a,
    C = 0x2b,
    V = 0x2c,
    B = 0x2d,
    N = 0x2e,
    M = 0x2f,
    Comma = 0x30,
    Period = 0x31,
    Slash = 0x32,
    Underscore = 0x33,
    Space = 0x34,
    Xfer = 0x35,
    RollUp = 0x36,
    RollDown = 0x37,
    Insert = 0x38,
    Delete = 0x39,
    Up = 0x3a,
    Left = 0x3b,
    Right = 0x3c,
    Down = 0x3d,
    HomeClear = 0x3e,
    Help = 0x3f,
    KeypadMinus = 0x40,
    KeypadDivide = 0x41,
    Keypad7 = 0x42,
    Keypad8 = 0x43,
    Keypad9 = 0x44,
    KeypadMultiply = 0x45,
    Keypad4 = 0x46,
    Keypad5 = 0x47,
    Keypad6 = 0x48,
    KeypadPlus = 0x49,
    Keypad1 = 0x4a,
    Keypad2 = 0x4b,
    Keypad3 = 0x4c,
    KeypadEquals = 0x4d,
    Keypad0 = 0x4e,
    KeypadComma = 0x4f,
    KeypadPeriod = 0x50,
    Nfer = 0x51,
    F1 = 0x62,
    F2 = 0x63,
    F3 = 0x64,
    F4 = 0x65,
    F5 = 0x66,
    F6 = 0x67,
    F7 = 0x68,
    F8 = 0x69,
    F9 = 0x6a,
    F10 = 0x6b,
    LeftShift = 0x70,
    CapsLock = 0x71,
    Kana = 0x72,
    Grph = 0x73,
    Control = 0x74,
    Stop = 0x60,
    Copy = 0x61,
    RightShift = 0x7d,
};

using Np2FrontendKeyId = FrontendKeyId;

std::optional<FrontendKeyId> to_frontend_key_id(Key key) noexcept;

inline std::optional<FrontendKeyId> to_np2_frontend_key_id(Key key) noexcept
{
    return to_frontend_key_id(key);
}

} // namespace np2_keyboard_input::mapping
