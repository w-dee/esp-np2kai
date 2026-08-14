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

The current roadmap reaches the portable and headless core milestones before
physical-board feature work. ESP32-S31 / S31 Korvo-1 remains a future
portability target and is not scheduled. Their interfaces and implementation
boundaries should nevertheless leave room for a future Espressif SoC
implementation without requiring P4-specific assumptions to be removed from
the emulator core.

The UART Control Plane Base is a completed control-infrastructure milestone
and is verified under `esp-emu` v0.39.0 for the ESP32-P4 emulator
environment. It contains only the neutral protocol, bounded parser, configured
console-UART transport, and three read-only commands: `protocol.hello`,
`system.ping`, and `system.info`. Physical-board UART validation and the
corresponding physical transport remain unverified.

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
remain future work after the headless core milestones.

## Steps 1-3: Completed foundations and core import

The Binary Data Plane documentation and implementation, the File Transfer Base
with virtual/RAM-backed storage, and the pinned NP2kai candidate core import
are complete. The NP2kai import is a source-inspection-based candidate source
set with snapshot integrity verification; it is not yet compile/link/run
validated. The project may follow NP2/NP2kai lineage, but the Step 3 vendored
source is NP2kai only.

## Step 4: Ubuntu-native headless minimum core

This is the next implementation stage:

- integrate the candidate NP2kai source set into an Ubuntu-native build
- establish the required portable host contracts and adapters
- compile, link, and start the headless core
- use compiler/linker results to identify missing or unnecessary dependencies
- begin executable i286 CPU, memory, timer, and UART benchmark validation

The Step 3 snapshot is not a complete dependency closure claim. Any manifest
change discovered here requires explicit allowlist, provenance, license
evidence, deterministic regeneration, verification, and human review.

## Step 5: ESP32-P4 headless core

After the Ubuntu-native headless core, connect the validated core boundary to an
ESP32-P4 headless implementation. This stage precedes physical-board feature
work and does not claim physical hardware availability or validation.

## After physical hardware arrival: board features

The following work is intentionally scheduled after Steps 4 and 5.

### Step 6: microSD FAT32 backend

- FAT32 storage backend
- D88/HDI/NHD image access as supported by the host boundary

### Step 7: UART transfer to real SD

- upload/download through the UART control/data path
- real-media durability and error handling

### Step 8: Display

The initial P4-NANO target is:

- 640x400 guest framebuffer
- RGB565
- 2x scaling to 1280x800
- 30 fps
- one framebuffer

The 2x/1280x800 arrangement is a board bring-up target, not a global emulator
assumption. The TAB5 physical display is 1280x720, so guest framebuffer
dimensions and physical display policy remain separate.

### Step 9: USB HID and input

- USB keyboard
- USB mouse
- touch coordinates where board support is available

### Step 10: Audio

- beep
- SSG
- FM
- YM2608 rhythm
- ADPCM
- PCM86
