# Espressif SoC PC-9801 Emulator

This project will port a PC-9801 emulator based on NP2/NP2kai to Espressif
SoCs, with ESP32-P4 as the initial implementation platform. The initial
physical target is the Waveshare ESP32-P4-NANO-KIT-D, with the M5Stack TAB5 as
another planned ESP32-P4 target. ESP32-S31 / S31 Korvo-1 is a possible future
portability target, not a current implementation target.

## Status

Very early bring-up. A minimal headless ESP-IDF Hello World firmware and the
UART Control Plane Base have been implemented and verified under `esp-emu`
v0.39.0 with ESP-IDF v5.5.4. The automated checks build the firmware, create
the merged image, boot it under the emulator, and validate the Hello World and
UART Control Plane paths. No NP2 or NP2kai source code has been imported, and
no physical hardware has been validated.

The esp-emu test environment reports ESP32-P4 revision v3.1, so the test
configuration requires `CONFIG_ESP32P4_REV_MIN_301=y`. Physical P4-NANO and
TAB5 revision compatibility remains unverified.

The currently verified executable milestone is ESP32-P4-only. S31 Korvo-1 is
not implemented, tested, or validated.

The UART Control Plane Base is verified under `esp-emu` v0.39.0 for the
ESP32-P4 emulator environment. It provides bounded `@ESP-NP2 ` JSON-lines
framing, the separate `ESP-NP2KAI UART CONTROL READY` marker, and the initial
read-only commands `protocol.hello`, `system.ping`, and `system.info`.
Physical P4-NANO and TAB5 UART paths remain unverified.

The Binary Data Plane v1 and its bidirectional UART-TCP integration test are
implemented but not yet verified. The binary path is intentionally separate
from the verified JSON Control Plane and uses bounded COBS/CRC framing with a
deterministic 64 KiB test endpoint.

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
