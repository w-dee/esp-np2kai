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
with virtual/RAM-backed storage, and the pinned NP2kai core import are complete.
The import remains a byte-preserved allowlist whose contents alone do not claim
a general dependency closure; the bounded Step 4 configuration below provides
the current compile/link/runtime evidence. The project may follow NP2/NP2kai
lineage, but the vendored source is NP2kai only.

## Step 4: Ubuntu-native headless minimum core

This bounded formal milestone is completed and verified. The Ubuntu-native
configuration:

- compiles and links 124 vendor plus 10 host translation units;
- leaves zero project/non-system unresolved symbols;
- boots the tracked formal NP2TEST Stage-1 FD1232 golden with the permanent
  headless runner;
- records 13 completed tests, 13 passed, 0 failed, and result-v1 CRC
  `0x58f5b827`; and
- runs as the `ubuntu-native-headless` CI contract alongside guest fixture CI.

The deterministic controller budgets are 512 pre-running returned slices and
4096 running observations/slices. The external Python supervisor has a separate
30-second wall-clock safety timeout. In the current measured golden run, first
protocol evidence appeared at returned slice 202 and terminal `PASS` at slice
203; these are runtime measurements, not architectural guarantees.

The Step 3 snapshot remains an allowlist rather than a universal closure claim.
Any future manifest change requires explicit allowlist, provenance, license
evidence, deterministic regeneration, verification, and human review. Step 4 is
bounded to the imported minimum portable core and formal Stage-1 image; it does
not validate complete PC-9801 compatibility, arbitrary software, GUI, audio,
input, or hardware behavior.

## Step 5: ESP32-P4 headless core

Step 5 firmware/runtime integration is implemented and validated under a
non-formal `EXTMEM=8` esp-emu profile. The path now uses the validated 124-TU
NP2core, the np2host boundary, PSRAM external BSS, a read-only raw-NOR
NP2TEST fixture with mmap/DOSIO access, a dedicated FreeRTOS NP2 runner, and
the shared Stage-1 configuration/parser/controller. The native formal oracle
remains 13/13 with CRC `0x58f5b827` and `NP2TEST_RESULT=PASS`.

The formal ESP32-P4 profile remains `EXTMEM=13`, but runtime validation is
blocked on esp-emu v0.39.0: its 16 MiB PSRAM model cannot provide the required
contiguous external allocation after the approximately 5.69 MiB NP2core
external BSS placement (including the 2 MiB `mem[]` buffer). The reduced
`EXTMEM=8` result reaches 13/13 with CRC `0x58f5b827` and
`NP2REDUCED_RESULT=PASS`; it is explicitly NON-FORMAL supplementary evidence
and does not replace formal `EXTMEM=13` validation.

The three CI jobs remain independent: formal fixture CI, formal Ubuntu-native
headless CI, and the NON-FORMAL ESP32-P4 esp-emu reduced Stage-1 job. No real
ESP32-P4 hardware result is claimed. Physical storage, peripherals, and
future SoC work remain later scopes. Pending bring-up includes formal
`EXTMEM=13` runtime validation, a real ESP32-P4 with the intended 32 MiB PSRAM,
ESP32-S31, microSD/FATFS and writable media, display/audio/input/USB, board
integration, and performance or multicore work.

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
