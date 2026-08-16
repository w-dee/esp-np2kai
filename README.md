# Espressif SoC PC-9801 Emulator

This project will port a PC-9801 emulator based on NP2/NP2kai to Espressif
SoCs, with ESP32-P4 as the initial implementation platform. The initial
physical target is the Waveshare ESP32-P4-NANO-KIT-D, with the M5Stack TAB5 as
another planned ESP32-P4 target. ESP32-S31 / S31 Korvo-1 is a possible future
portability target, not a current implementation target.

## Status

A minimal headless ESP-IDF Hello World firmware, the UART Control Plane Base,
Binary Data Plane v1, and RAM-backed File Transfer Base are verified under
`esp-emu` v0.39.0 with ESP-IDF v5.5.4. A pinned NP2kai snapshot for the initial
i286 core baseline is vendored with verified manifest/blob metadata.

The bounded Step 4 Ubuntu-native validation is now verified: the configured
portable core closure compiles and links, and the permanent Ubuntu-native
headless runner boots the tracked formal NP2TEST Stage-1 golden. The current
golden run completes 13 tests with 13 passed, 0 failed, and result-v1 CRC
`0x58f5b827`. This validates the imported minimum portable core and formal
Stage-1 path only; it is not a claim of complete PC-9801 compatibility or
general software support. ESP32-P4 firmware execution remains a later
validation layer. NP2 source itself has not been imported.

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

Validation is organized in three layers:

1. **Ubuntu-native** — fastest portable-core and reference validation. The
   bounded Step 4 NP2kai execution belongs to this layer.
2. **Espressif `esp-emu`** — ESP32-P4 firmware and integration validation. The
   current Hello World and UART/data/file checks are results for this layer;
   Step 4 NP2kai has not been run inside `esp-emu` as part of this milestone.
3. **Real ESP32-P4 hardware** — unsupported peripherals, real timing,
   performance, and board transport validation.

Ubuntu-native success is not a substitute for `esp-emu` or hardware validation.
The verified `esp-emu` path is specifically an ESP32-P4 result, and its
performance is not representative of real target hardware.

## Documentation

- [Architecture](docs/architecture.md)
- [Development environment](docs/development-environment.md)
- [Bring-up plan](docs/bringup-plan.md)
- [Licensing and provenance](docs/licensing.md)
- [Vendored NP2kai snapshot](third_party/np2kai/README.md)
