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
and validation failures. Step 6A separately completes the persistent FATFS
backend and its VFS/DOSIO integration; physical UART verification, physical
microSD/SDMMC access, UART transfer to real media, input integration, and later
emulator commands remain future work after the headless core milestones.

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
ESP32-P4 hardware result is claimed. Step 6A emulator SPI-NOR FATFS storage is
complete; physical storage, peripherals, and future SoC work remain later
scopes. Pending bring-up includes formal `EXTMEM=13` runtime validation, a
real ESP32-P4 with the intended 32 MiB PSRAM, ESP32-S31, physical
microSD/SDMMC and writable media, display/audio/input/USB, board integration,
and performance or multicore work.

## Step 6A: emulator SPI-NOR FATFS persistent storage — COMPLETED

Step 6A provides hardware-independent persistent storage using the common
8 MiB esp-emu flash envelope: factory at `0x010000`/`0x100000`, the
read-only raw `np2test` oracle at `0x110000`/`0x134000`, and FATFS/WL storage
at `0x244000`/`0x5BC000`. The 8 MiB envelope is not a statement about the
physical P4-NANO flash maximum.

The mounted `/persist` namespace isolates File Transfer's `/persist/files`
from private `/persist/fixtures` and `/persist/.np2-staging` state. File
Transfer uses the neutral `storage::Storage` interface and `StorageFatfs`,
while NP2 disk access remains `FDD/XDF -> DOSIO -> generic POSIX/VFS ->
mounted filesystem` and is read-only.

The authoritative local routine is
[`tools/emu/test-step6a-ci.sh`](../tools/emu/test-step6a-ci.sh). With the
pinned ESP-IDF v5.5.4 and esp-emu v0.39.0 environment active, it covers raw
reduced Stage-1, bounded StorageFatfs provider checks, File Transfer basic,
512 KiB transfer, preloaded NoSpace, 256 KiB persistence, high-address proof,
VFS DOSIO, normal VFS NP2, and raw-poisoned VFS source independence. Extended
2 MiB transfer/replacement/persistence, the old matrix, natural-fill NoSpace,
and other development experiments remain outside routine CI because UART
stop-and-wait execution under esp-emu is slow.

The formal machine configuration remains `EXTMEM=13` and its authoritative
Ubuntu-native result is 13/13 with CRC `0x58f5b827`. Pinned esp-emu v0.39.0
cannot provide the required contiguous allocation for that formal firmware
configuration, so the Step 6A ESP32-P4 runtime result is explicitly
NON-FORMAL reduced `EXTMEM=8`. Its VFS/FATFS-backed NP2 run also reaches 13/13
with CRC `0x58f5b827`; this does not claim formal ESP32-P4 validation.

Step 6A.4 poisoned only the raw partition's first `0x1000` bytes and changed
its SHA from the golden
`3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3` to
`92ecf3e62e8ea67a2e618b58cf57e6a8db0f7a4ca5891507d53854285fe108f`, while
the FATFS storage region remained unchanged. The VFS run still passed 13/13,
demonstrating source independence without claiming arbitrary guest software
or general filesystem compatibility.

## Step 6B: physical microSD / SDMMC — FUTURE

Step 6B remains future real-hardware storage work. Its expected scope is:

- ESP32-P4-NANO or another supported real P4 board;
- physical microSD and SDMMC integration;
- real board pin mapping re-verification and card detect where available;
- card initialization, CID/CSD where useful, bus-width and speed behavior;
- physical removal/error handling;
- durability, performance, and real-media validation.

The Step 6A SPI-NOR FATFS backend is not the final physical microSD backend.
There is no real P4 hardware storage validation yet, including P4-NANO,
TAB5, or ESP32-S31.

## After physical hardware arrival: board features

The following work is intentionally scheduled after Steps 4, 5, and the
completed emulator-only Step 6A. Physical storage starts at Step 6B above.

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
