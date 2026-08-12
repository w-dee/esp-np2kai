# Bring-up Plan

The implementation starts on the ESP32-P4-NANO-KIT-D while preserving
portability to the M5Stack TAB5.

## Phase 0: Development environment

- reproducible toolchain
- repository structure
- documentation
- esp-emu installation

The first executable milestone is now implemented as a minimal headless
ESP-IDF Hello World firmware targeting ESP32-P4. It has not yet been built or
verified under `esp-emu`; the planned check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh).

The firmware uses GNU C++20 with C++ exceptions and RTTI disabled. For
`esp-emu` v0.39.0, the current configuration includes the ESP32-P4 ROM
revision 0 compatibility settings `CONFIG_ESP32P4_REV_MIN_0=y` and
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. These settings are not yet a final
physical-board configuration decision.

## Phase 1: Board function validation

- UART/control infrastructure
- microSD read/write
- UART-based SD file transfer
- MIPI color bars
- landscape display
- touch coordinates
- audio codec playback
- USB HID keyboard
- ESP32-C6 Wi-Fi

## Phase 2: Minimum NP2 port

- no sound
- no display
- i286 CPU
- memory
- timers
- UART benchmark output
- guest CPU performance measurement

## Phase 3: Display

The initial P4-NANO target is:

- 640x400 guest framebuffer
- RGB565
- 2x scaling to 1280x800
- 30 fps
- one framebuffer

The 2x/1280x800 arrangement is a board bring-up target, not a global emulator
assumption. The TAB5 physical display is 1280x720, so guest framebuffer
dimensions and physical display policy remain separate.

## Phase 4: Storage and input

- D88
- USB keyboard
- USB mouse
- HDI/NHD

## Phase 5: Audio

- beep
- SSG
- FM
- YM2608 rhythm
- ADPCM
- PCM86
