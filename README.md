# ESP32-P4 PC-9801 Emulator

This project will port a PC-9801 emulator based on NP2/NP2kai to ESP32-P4. The
initial hardware target is the Waveshare ESP32-P4-NANO-KIT-D, with an
architecture intended to support the M5Stack TAB5 and other ESP32-P4 boards.

## Status

Very early bring-up. A minimal headless ESP-IDF Hello World firmware has been
implemented for ESP32-P4, but it has not yet been built or executed under
`esp-emu`. No NP2 or NP2kai source code has been imported, and no physical
hardware or emulator functionality is claimed to work.

## Development model

The emulator core will remain portable and separate from ESP32-P4 platform and
board-specific code. Development targets are Ubuntu native builds/tests,
Espressif `esp-emu`, the P4-NANO board, and the TAB5. The future firmware will
also support headless operation for core and integration tests.

## Initial toolchain baseline

- Host OS: Ubuntu 24.04
- ESP-IDF: v5.5.4
- esp-emu: v0.39.0
- ESP target: `esp32p4`
- New ESP32-P4-side C++: C++20

The project does not use a PlatformIO-installed ESP-IDF environment.

New firmware C++ is explicitly compiled as GNU C++20. C++ exceptions and RTTI
are disabled, and the firmware does not use `iostream`.

The current emulator bring-up can later be exercised with
[`tools/emu/test-hello-world.sh`](tools/emu/test-hello-world.sh). The script is
implemented for future verification but has not been executed yet.

## Validation stages

Validation is planned in three layers: fast Ubuntu native tests, ESP32-P4
firmware/FreeRTOS/RISC-V integration and headless regression in `esp-emu`, and
physical hardware validation for peripherals and performance. `esp-emu`
performance will not be treated as representative of real ESP32-P4 hardware.

## Documentation

- [Architecture](docs/architecture.md)
- [Development environment](docs/development-environment.md)
- [Bring-up plan](docs/bringup-plan.md)
- [Licensing and provenance](docs/licensing.md)
