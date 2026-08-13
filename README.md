# Espressif SoC PC-9801 Emulator

This project will port a PC-9801 emulator based on NP2/NP2kai to Espressif
SoCs, with ESP32-P4 as the initial implementation platform. The initial
physical target is the Waveshare ESP32-P4-NANO-KIT-D, with the M5Stack TAB5 as
another planned ESP32-P4 target. ESP32-S31 / S31 Korvo-1 is a possible future
portability target, not a current implementation target.

## Status

Very early bring-up. A minimal headless ESP-IDF Hello World firmware has been
implemented and verified under `esp-emu` v0.39.0 with ESP-IDF v5.5.4. The
automated test builds the firmware, creates the merged image, boots it under
the emulator, and detects the UART marker. No NP2 or NP2kai source code has
been imported, and no physical hardware or emulator functionality beyond this
Hello World path is claimed to work.

The esp-emu test environment reports ESP32-P4 revision v3.1, so the test
configuration requires `CONFIG_ESP32P4_REV_MIN_301=y`. Physical P4-NANO and
TAB5 revision compatibility remains unverified.

The currently verified executable milestone is ESP32-P4-only. S31 Korvo-1 is
not implemented, tested, or validated.

The UART Control Plane Base skeleton is implemented but not yet verified. It
adds a framed, line-oriented JSON protocol with `protocol.hello`,
`system.ping`, and `system.info`; its esp-emu round-trip test is planned but
has not yet passed.

## Development model

The emulator core will remain portable and separate from Espressif-common,
SoC-specific, and board-specific code. Development targets are Ubuntu native
builds/tests, SoC/emulator integration where supported, the P4-NANO board, and
the TAB5. The architecture should leave room for a future ESP32-S31
implementation without making S31 current work. The future firmware will also
support headless operation for core and integration tests.

## Initial toolchain baseline

- Host OS: Ubuntu 24.04
- Current ESP-IDF baseline: v5.5.4
- Current esp-emu baseline: v0.39.0
- Current ESP target: `esp32p4`
- New platform-side C++: C++20

The project does not use a PlatformIO-installed ESP-IDF environment.

New firmware C++ is explicitly compiled as GNU C++20. C++ exceptions and RTTI
are disabled, and the firmware does not use `iostream`.

The verified emulator bring-up can be exercised with
[`tools/emu/test-hello-world.sh`](tools/emu/test-hello-world.sh). The script is
bound to ESP-IDF v5.5.4 and esp-emu v0.39.0 for this milestone.

## Validation stages

Validation is planned in three layers: fast Ubuntu native tests,
SoC/emulator integration where supported, and physical hardware validation for
peripherals and performance. The verified `esp-emu` Hello World path is
specifically an ESP32-P4 result. `esp-emu` performance will not be treated as
representative of real target hardware.

## Documentation

- [Architecture](docs/architecture.md)
- [Development environment](docs/development-environment.md)
- [Bring-up plan](docs/bringup-plan.md)
- [Licensing and provenance](docs/licensing.md)
