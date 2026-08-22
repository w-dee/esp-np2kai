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

The firmware uses GNU C++20 with C++ exceptions and RTTI disabled. Production
firmware selects `p4-v1x` or `p4-v3x` explicitly through the build wrapper;
revision selection is not placed in common defaults. The existing
`esp-emu` v0.39.0 regressions use `p4-v3x`. Generic `p4-v1x` remains compile
evidence in CI, while the explicit P4-NANO board profile now has scoped
production UART/SDMMC/File Transfer hardware evidence. This is not a claim
that every production subsystem has been physically validated.

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
`system.ping`, and `system.info`. The P4-NANO onboard CH343P path is also
hardware-validated at 1.5 Mbps; TAB5 remains unverified.

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
backend and its VFS/DOSIO integration. P4-NANO physical SD and production
Host-to-Device File Transfer are now hardware-validated at 1.5 Mbps for
bounded `zero-rle-v1` with W=1 and opt-in W=2. Removal/durability,
device-to-host qualification, TAB5, input integration, and later emulator
commands remain future work.

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
headless CI, and the NON-FORMAL ESP32-P4 esp-emu reduced Stage-1 job. Those CI
jobs do not themselves claim hardware validation. Separate P4-NANO evidence
now covers SDMMC, production Host-to-Device File Transfer, and a formal
physical-SD NP2TEST 13/13 run with CRC `0x58f5b827`; the File Transfer
qualification is a separate test and must not be described as NP2TEST.
Pending bring-up still includes unqualified `EXTMEM=13` configurations,
ESP32-S31, removal/durability and broader writable-media behavior,
display/audio/input/USB, TAB5 integration, and multicore work.

## Step 6A: emulator SPI-NOR FATFS persistent storage — COMPLETED

Step 6A provides hardware-independent persistent storage using the approved
8 MiB esp-emu flash envelope: factory at `0x010000`/`0x200000`, the
read-only raw `np2test` oracle at `0x210000`/`0x134000`, and FATFS/WL storage
at `0x344000`/`0x4BC000`, ending at `0x800000`. The 8 MiB envelope is not a
statement that every future board has only 8 MiB of flash or that it is the
physical P4-NANO flash maximum. The offsets are generated from
`firmware/partitions.csv` and checked from the generated ESP-IDF table.

The factory partition was expanded to 2 MiB to provide future production
headroom, especially for OSD/UI and display-side functionality. The additional
1 MiB was taken from the internal FATFS development/validation fixture while
the `np2test` fixture remained unchanged. Real large user disk/image storage is
expected to use removable or external media such as microSD/SDMMC where
appropriate; this 8 MiB envelope is the current common validation baseline.

The mounted `/persist` namespace isolates File Transfer's `/persist/files`
from private `/persist/fixtures` and `/persist/.np2-staging` state. File
Transfer uses the neutral `storage::Storage` interface and `StorageFatfs`,
while NP2 disk access remains `FDD/XDF -> DOSIO -> generic POSIX/VFS ->
mounted filesystem` and is read-only.

The authoritative local routine is
[`tools/emu/test-step6a-ci.sh`](../tools/emu/test-step6a-ci.sh). With the
pinned ESP-IDF v5.5.4 and esp-emu v0.39.0 environment active, it covers raw
reduced Stage-1, bounded StorageFatfs provider checks, File Transfer basic,
262145-byte transfer, preloaded NoSpace, 4097-byte persistence, high-address
proof, VFS DOSIO, normal VFS NP2, and raw-poisoned VFS source independence.
The default build uses one shared incremental tree across the four non-reduced
profiles (storage-provider, UART FATFS, DOSIO, and VFS); raw/reduced remains
separate and each profile still produces its own application binary.
`STEP6A_PROFILE_BUILD_MODE=isolated` provides a separate-tree fallback for
diagnosis. Emulator process lifecycles remain independent. Extended 2 MiB
transfer/replacement/persistence, the old matrix, natural-fill NoSpace, and
other development experiments remain outside routine CI because UART
stop-and-wait execution under esp-emu is slow.

The File Transfer service limit remains 2 MiB. A clean image using the approved
2 MiB-app / 8 MiB validation geometry accepts a new 2 MiB
file upload/readback/ranged-read workload, but the protocol
maximum does not guarantee same-size replacement of a full 2 MiB file because
the old target and staging file temporarily coexist. The extended
replacement/rollback validation uses 419 clusters (`0x1A3000`) and a
35-cluster safety margin; this capacity distinction does not reduce the
protocol maximum.

NoSpace validation is geometry-derived: a 1 MiB request needs 256 clusters,
the target is left with 255 free clusters, and under the current approved
geometry, the measured result uses a 618-cluster (`0x26A000`) prefill.
Begin/preallocation fails with
`NO_SPACE` before payload transfer (`payload_frames=0`), while the existing
file remains intact and staging cleanup/endpoint recovery pass. The storage
fixture currently measures 1193 usable clusters, with 320 clean allocated and
873 clean free at a 4096-byte cluster size. These are validation measurements,
not product capacity guarantees.

High-address validation requires a marker at physical offset `>= 0x400000`
inside storage; the observed `0x68D000` is evidence only. Normal and
raw-poisoned VFS runs pass: poisoning changes only the raw `np2test` range,
while storage remains unchanged, demonstrating source independence without
claiming a general security boundary.

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

## Step 6B: physical microSD / SDMMC — PARTIALLY HARDWARE VALIDATED

The P4-NANO SDMMC slot-1 production backend is implemented and validated with
a physical SD card. Current evidence includes mount/access, production
Host-to-Device writes, exact size/SHA verification, and the separate formal
physical-SD NP2TEST 13/13 result. Remaining scope includes:

- real board pin mapping re-verification and card detect where available;
- physical removal/error handling;
- durability, performance, and real-media validation.

The Step 6A SPI-NOR FATFS backend is not the final physical microSD backend.
P4-NANO evidence does not generalize to TAB5, ESP32-S31, arbitrary cards, or
the remaining lifecycle cases.

## Step 6C: UART File Transfer to physical media — HOST-TO-DEVICE HW VALIDATED

The production P4-NANO path is hardware-validated at 1.5 Mbps for
Host-to-Device physical-SD uploads using bounded `zero-rle-v1`, default W=1,
and opt-in W=2 bounded Go-Back-N. Exact device-side SHA and transport event
checks passed. This is File Transfer validation, not formal NP2TEST execution.
Remaining scope covers:

- broader upload/download qualification through the UART control/data path;
- real-media durability, removal, and error handling.

TAB5 and other boards remain unqualified. W=1 remains the production default.

## Step 7A: headless guest framebuffer and deterministic video oracles — COMPLETE

Step 7A is implemented and verified without a physical display. The guest
surface is RGB565LE with dynamic geometry; 640x400 is the approved tested
geometry, and the ESP32-P4 surface is stored in external PSRAM. Ubuntu-native
and ESP32-P4 / `esp-emu` validation covers three descriptor-selected scenes:

- scene 1, deterministic text rendering;
- scene 2, deterministic direct graphics-VRAM writes; and
- scene 3, actual slave-GDC drawing commands.

The GDC scene uses the vendor-backed command path rather than direct-VRAM
emulation and currently covers VECTL-based primitives only. VECTR, circles,
fills, GRCG, EGC, and GDC text drawing remain outside the approved scope. Step
7A is not a claim of complete PC-98 graphics or complete uPD7220 behavior.

## Step 7B: presentation boundary

Step 7B separates the mutable guest framebuffer from a future asynchronous
platform consumer. The software-only presentation boundary is complete through
Step 7B.1c; physical display output is Step 7B.2 below.

### Step 7B.1a: portable two-slot publisher / Ubuntu contract — COMPLETE

The Ubuntu-native C11 publisher provides two caller-owned slots, latest-frame-
wins ownership, pending-frame coalescing, immutable acquired frames, bounded
synchronization, and resize/generation lifetime independent of the guest
framebuffer.

### Step 7B.1b: ESP32-P4 FreeRTOS / esp-emu integration — COMPLETE

The ESP32-P4 presentation probe validates external-PSRAM guest and presentation
storage, a dedicated FreeRTOS producer/consumer path, lock-free 32-bit slot
state atomics in the tested toolchain, acquired-frame immutability, coalescing,
and the 320x200 resize/generation case. This is emulator evidence only and is
not physical display validation.

### Step 7B.1c: GitHub Actions continuous validation — COMPLETE

The existing Ubuntu and ESP32-P4 video jobs now include the portable
presentation contract, ESP log-validator self-test, profile-isolation check,
and ESP presentation runtime contract. The CI configuration reuses the pinned
ESP-IDF v5.5.4 and esp-emu v0.39.0 environment.

### Step 7B.2: physical ESP32-P4 display output — BLOCKED / DEFERRED UNTIL REAL HARDWARE ARRIVES

The next P4-NANO bring-up stage is intentionally paused until physical
ESP32-P4 hardware is available. The first tasks are:

- confirm the actual board revision;
- verify actual PSRAM size and allocation behavior;
- verify formal `EXTMEM=13` runtime on real 32 MiB PSRAM hardware;
- inspect and bring up the actual LCD/panel wiring;
- apply the planned PPA exact 640x400 -> 1280x800 2x policy;
- choose the physical output-buffer strategy;
- initialize MIPI-DSI and the panel;
- verify cache/DMA coherency;
- consume presentation slots asynchronously;
- measure tearing behavior, timing, and bandwidth; and
- pursue an approximately 30 displayed-fps bring-up target.

The 30 fps figure is a bring-up target, not a proven result. PPA, MIPI-DSI,
panel operation, physical timing, tearing-free output, bandwidth, and hardware
performance have not been validated.

## Remaining hardware boundary

The software-only portion through Step 7B.1c is complete. P4-NANO UART, SDMMC,
and Host-to-Device File Transfer now have hardware evidence. Remaining Step
6B/6C lifecycle cases, Step 7B.2 physical display, Step 8 physical input, Step
9 physical audio, and TAB5 still require separate hardware work. The portable
emulator, host, and documentation work can continue independently.

## Step 8: USB HID / input — FUTURE, hardware required

The future input stage covers USB keyboard, USB mouse, and touch coordinates
where board support is available. No physical input transport is validated.

## Step 9: audio — FUTURE, hardware required

The future audio stage covers beep, SSG, FM, YM2608 rhythm, ADPCM, and PCM86
output policy and board integration. No physical audio path is validated.
