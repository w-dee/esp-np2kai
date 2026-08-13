# Espressif SoC PC-9801 Emulator

This project will port a PC-9801 emulator based on NP2/NP2kai to Espressif
SoCs, with ESP32-P4 as the initial implementation platform. The initial
physical target is the Waveshare ESP32-P4-NANO-KIT-D, with the M5Stack TAB5 as
another planned ESP32-P4 target. ESP32-S31 / S31 Korvo-1 is a possible future
portability target, not a current implementation target.

## Status

Very early bring-up. A minimal headless ESP-IDF Hello World firmware, the UART
Control Plane Base, Binary Data Plane v1, and RAM-backed File Transfer Base
have been implemented and
verified under `esp-emu` v0.39.0 with ESP-IDF v5.5.4. The automated checks
build the firmware, create the merged image, boot it under the emulator, and
validate these milestones independently. No NP2 or NP2kai source code has been
imported, and no physical hardware has been validated.

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

The Binary Data Plane v1 is verified under the ESP32-P4 `esp-emu` UART-TCP
environment. Its integration test transfers deterministic 64 KiB payloads in
both directions and checks CRC, duplicate handling, NACK retransmission,
corrupted-frame recovery, and text/binary resynchronization. This verifies the
emulator byte path only; physical P4-NANO, CH343P, and TAB5 UART paths remain
unverified.

The File Transfer Base is verified over the same UART-TCP path. It adds a
neutral streaming storage interface, a bounded 256 KiB RAM backend, logical
UTF-8 paths, paginated metadata, ranged reads, and staged complete-file writes.
Its 131,109-byte round-trip regression covers final-ACK replay, abort-safe
replacement, zero-length files, and path/error bounds. This is not microSD,
FATFS, or physical-media validation.

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

The four verified emulator checks retain separate scopes:

- [`tools/emu/test-hello-world.sh`](tools/emu/test-hello-world.sh) verifies
  basic ESP-IDF build, merge, boot, and the Hello World marker.
- [`tools/emu/test-uart-control-plane.sh`](tools/emu/test-uart-control-plane.sh)
  runs the Hello World regression and verifies the bounded JSON control path.
- [`tools/emu/test-uart-binary-data-plane.sh`](tools/emu/test-uart-binary-data-plane.sh)
  runs the Control Plane regression and verifies bidirectional binary transport
  over esp-emu UART-TCP.
- [`tools/emu/test-file-transfer-base.sh`](tools/emu/test-file-transfer-base.sh)
  preserves the earlier regressions and verifies the RAM-backed file service.

All four checks are bound to ESP-IDF v5.5.4 and esp-emu v0.39.0 for this
milestone.

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
