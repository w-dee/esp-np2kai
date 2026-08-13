# Bring-up Plan

The implementation starts on the ESP32-P4-NANO-KIT-D while preserving
portability to the M5Stack TAB5.

## Phase 0: Development environment

- reproducible toolchain
- repository structure
- documentation
- esp-emu installation

The first executable milestone is implemented and verified as a minimal
headless ESP-IDF Hello World firmware targeting ESP32-P4. Under ESP-IDF
v5.5.4 and esp-emu v0.39.0, the automated check builds the firmware, creates
the merged image, boots it, reaches `app_main()`, detects the UART marker
`ESP-NP2KAI HELLO WORLD OK`, and reports PASS with exit status 0. The check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh).

The firmware uses GNU C++20 with C++ exceptions and RTTI disabled. For
`esp-emu` v0.39.0, the emulator reports ESP32-P4 revision v3.1 and the
configuration therefore requires `CONFIG_ESP32P4_REV_MIN_301=y`. This is an
emulator verification result, not a final physical-board configuration
decision. P4-NANO and TAB5 revision compatibility remains unverified.

The current bring-up phases validate ESP32-P4 first and do not schedule
ESP32-S31 or S31 Korvo-1 work. Their interfaces and implementation boundaries
should nevertheless leave room for a future Espressif SoC implementation
without requiring P4-specific assumptions to be removed from the emulator
core.

The UART Control Plane Base is the first Phase 1 control-infrastructure
milestone and is verified under `esp-emu` v0.39.0 for the ESP32-P4 emulator
environment. It contains only the neutral protocol, bounded parser, configured
console-UART transport, and three read-only commands: `protocol.hello`,
`system.ping`, and `system.info`. Physical-board UART validation and the
remaining Phase 1 work are not complete.

The Binary Data Plane v1 base is completed and verified under ESP-IDF v5.5.4
and esp-emu v0.39.0 for the ESP32-P4 emulator environment. It provides a
bounded COBS/CRC byte path, one stop-and-wait transfer manager, and a
deterministic bidirectional 64 KiB test. The integration test directly covers
duplicate DATA handling, corrupted CRC recovery, NACK-driven retransmission,
and text/binary resynchronization. Timeout retry, shared retry-budget,
retry-exhaustion, and mismatched-NACK abort are implemented v1 semantics; they
are not separately claimed as injected runtime cases here.

The File Transfer Base is also completed and verified under esp-emu v0.39.0.
It generalizes Binary Data Plane endpoints, adds final Host-to-Device ACK
replay, and provides logical file operations through a neutral storage API and
a bounded RAM backend. The integration check covers a 131,109-byte round trip,
pagination, ranges, staged replacement/abort, zero-length files, UTF-8 names,
and validation failures. Physical UART verification, microSD/FATFS access,
UART transfer to real media, input integration, and later emulator commands
remain incomplete Phase 1 work.

## Phase 1: Board function validation

- UART/control infrastructure
- microSD read/write
- UART-based SD file transfer (RAM-backed service base verified; real SD pending)
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
