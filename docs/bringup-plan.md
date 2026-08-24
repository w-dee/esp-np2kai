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

### Step 7B.2a: P4-NANO native physical-display foundation — COMPLETE

Status: **IMPLEMENTED / BUILD VALIDATED / REAL-HARDWARE UART VALIDATED /
REAL-LCD VISUALLY VALIDATED / COMPLETE** for the bounded native diagnostic
scope.

The opt-in production profile
`tools/emu/build-production.sh --variant p4-v1x --board p4-nano --display-foundation`
now provides the smallest bounded native scanout path. It is restricted to
P4 revision 1.x and P4-NANO and leaves the existing bring-up application
untouched. The path uses the shared GPIO7/GPIO8 modern I2C service, safe
JD9365/power sequencing, one native 800x1280 RGB565 DSI/DPI framebuffer, cache
priming, and a deterministic static edge/corner/geometry pattern before
conservative backlight enable. It logs chip revision, geometry, lane/bitrate,
DPI, one-buffer size/pointer, PSRAM free/largest-block telemetry, stage
outcomes, pattern CRC, and cleanup/backlight state.

The profile was built with ESP-IDF v5.5.4 and run on a Waveshare
ESP32-P4-NANO-KIT-D. The board reported ESP32-P4 revision v1.3, a 40 MHz
crystal, a 16 MiB physical flash device, and 32 MiB PSRAM. The project
intentionally retains the existing 8 MiB validation envelope; the larger
physical flash capacity is not a Step 7B.2a failure. PSRAM initialization and
the PSRAM memory test passed.

The panel was the Waveshare 10.1-DSI-TOUCH-A with JD9365 ID `93 65 04`. The
production scanout configuration was native 800x1280 RGB565, stride 1600
bytes, framebuffer size 2,048,000 bytes, exactly one DSI/DPI framebuffer, two
MIPI-DSI lanes at 1500 Mbps/lane, and an 80 MHz DPI clock. The approximately
68.66 Hz value is a calculated nominal refresh, not a measured refresh result.

The configuration follows the preserved known-good diagnostic record in
[`hardware/bringup/esp32-p4/p4-nano/display/README.md`](../hardware/bringup/esp32-p4/p4-nano/display/README.md),
including observed JD9365 ID `93 65 04`, two lanes at 1500 Mbps/lane, the
0x95 `0x11 -> 0x17` control sequence, and conservative backlight value
`0x40`. That bring-up directory and its safe adapter remain untouched.

The real production run completed every expected stage:

- `shared_i2c_init`
- `panel_control_power_on`
- `dsi_phy_ldo`
- `dsi_bus`
- `dbi_io`
- `jd9365_panel_create`
- `dpi_framebuffer_acquire`
- `black_cache_sync`
- `panel_reset_init_display_on`
- `static_pattern_cache_sync`
- `backlight_enable`

The validated backlight lifecycle was register `0x96 = 0x00` during setup,
`0x96 = 0x40` for the bounded visible test, and OFF during cleanup. No
`0x96 = 0xff` write was observed. No panic, watchdog, reset loop, DSI
underrun, I2C error, or framebuffer allocation failure was observed. No
unvalidated panel power-off register sequence was introduced.

The runtime static-pattern CRC32 was `0x5383260a`, matching the host golden
`0x5383260a`. This proves byte-level agreement between the host-generated
reference pattern and the runtime framebuffer; CRC agreement alone does not
prove physical LCD correctness.

Measured PSRAM telemetry was:

- before display init: free `33,551,868`, largest block `33,030,144` bytes;
- after one native framebuffer acquisition: free `31,502,616`, largest block
  `31,457,280` bytes.

The reported free-heap delta was `2,049,252` bytes while the framebuffer
payload was `2,048,000` bytes. The small difference may include allocator,
alignment, and driver bookkeeping; it must not be extrapolated to double-
buffer viability. One native DMA-capable framebuffer was successfully
allocated on the real 32 MiB PSRAM hardware with substantial remaining
capacity.

The human operator visually inspected the physical LCD output twice and
reported `HUMAN VISUAL RESULT: PASS`. The bounded approximately five-second
pattern showed the expected native portrait image: top red, right green,
bottom blue, left white; top-left yellow, top-right magenta, bottom-left cyan,
and bottom-right orange. All four edges and corners, native portrait geometry,
and RGB ordering were observed without clipping or obvious static corruption.
This is not long-duration stability, tearing, measured-refresh, or performance
validation.

Step 7B.2a proves:

```text
production firmware
 -> safe shared-I2C ownership
 -> safe JD9365/P4-NANO initialization
 -> one native 800x1280 RGB565 framebuffer
 -> cache synchronization
 -> deterministic static scanout
 -> bounded backlight lifecycle
 -> real physical LCD output
```

It does not implement or validate the later live emulator display pipeline.
The following remain FUTURE / UNVALIDATED: live NP2 presentation consumption,
640x400 guest-frame presentation to the LCD, exact 2x scaling, 90-degree
rotation, double buffering, framebuffer switch/reuse semantics, tearing under
animation, measured physical refresh, long-duration display stability, PSRAM
bandwidth under live emulator load, PPA scaling/rotation, display plus SDMMC
or audio concurrency, touch, LVGL, OSD, TAB5 physical display, ESP32-S31, and
ESP32-S3 display backends.

### Step 7B.2b: Pixel-exact landscape-to-native transform reference — COMPLETE

Status: **IMPLEMENTED / HOST BYTE-EXACT VALIDATED / ESP32-P4 BUILD VALIDATED /
REAL-HARDWARE TRANSFORM VALIDATED / REAL-LCD VISUALLY VALIDATED / COMPLETE**
for the bounded transform-reference and static physical-diagnostic scope.

The project-owned C++20 reference consumes an immutable `640x400 RGB565`
source, applies exact nearest-neighbor 2x to a logical `1280x800` landscape,
and writes directly to one native `800x1280 RGB565` destination:

```text
immutable 640x400 RGB565 source
 -> exact nearest-neighbor 2x
 -> logical 1280x800 landscape
 -> quarter-turn
 -> native 800x1280 RGB565 destination
```

The implementation is fused: there is no `1280x800` intermediate framebuffer,
filtering, bilinear interpolation, color conversion, RGB565 modification, PPA,
DMA2D, per-frame dynamic allocation inside the transform, live NP2 consumer, or
presentation-slot consumer. Every source pixel becomes exactly four identical
destination pixels.

The API intentionally retains both mathematical mappings:

```text
CLOCKWISE:
dst_x = 799 - (2 * sy + oy)
dst_y =       2 * sx + ox

COUNTERCLOCKWISE:
dst_x =       2 * sy + oy
dst_y = 1279 - (2 * sx + ox)
```

where `ox, oy ∈ {0, 1}`. The host coordinate-rich test exhaustively compares
both directions with an independent inverse-mapping oracle over every
destination pixel. It covers source/destination geometry, the 2,048,000-byte
destination, exact 2x duplication, full destination coverage, corners,
edge-center points, asymmetric interior points, bounds/canaries, and invalid
input sizes. Frozen reference CRC32 values are:

- CLOCKWISE: `0xdb938d53`;
- COUNTERCLOCKWISE: `0x164584cf`.

These CRCs are stable regression goldens, not the sole correctness proof.

The physical diagnostic uses a separate real `640x400 RGB565` source with
distinct four edges, four corners, and asymmetric interior markers. The source
CRC32 is `0x4291f7e5`; transformed diagnostic CRC32 values are CW
`0x37fd7262` and CCW `0xd98ce5d4`. The diagnostic starts from this source image
and transforms it into the native destination; it does not construct a final
`800x1280` source image directly. Its expected source semantics are top red,
right green, bottom blue, left white; top-left yellow, top-right magenta,
bottom-left cyan, bottom-right orange; plus an asymmetric interior marker. The
pattern detects wrong quarter-turn, mirroring, 180-degree reversal, RGB
ordering, and clipping.

The qualified real-hardware environment was the Waveshare
ESP32-P4-NANO-KIT-D with Waveshare 10.1-DSI-TOUCH-A/JD9365 display path,
ESP32-P4 revision v1.3, 40 MHz crystal, 32 MiB PSRAM, ESP-IDF v5.5.4, native
800x1280 RGB565 output, one 2,048,000-byte DSI framebuffer, `num_fbs=1`, two
MIPI-DSI lanes at 1500 Mbps/lane, and an 80 MHz DPI clock. The diagnostic path
was:

```text
640x400 RGB565 PSRAM source
 -> transform_to_native()
 -> one existing 800x1280 RGB565 DSI framebuffer
 -> full framebuffer cache synchronization
 -> physical JD9365 output
```

Both candidates were built and executed on the real P4-NANO. The CW candidate
had firmware-build PASS, runtime transform/CRC PASS, bounded display lifecycle
PASS, cleanup PASS, and human visual inspection PASS. The CCW candidate had
firmware-build PASS, captured terminal runtime/cleanup PASS, and human visual
inspection PASS. The human operator selected CCW as the natural upright
orientation on the installed P4-NANO/JD9365 assembly.

The retained CW PSRAM telemetry was:

- before source allocation: free `33,551,868`, largest `33,030,144`;
- after the 512,000-byte source allocation: free `33,035,768`, largest
  `33,030,144`;
- after native framebuffer acquisition: free `30,986,520`, largest `30,932,992`.

The CCW run established the same 512,000-byte source plus one native
2,048,000-byte framebuffer allocation model on the real 32 MiB PSRAM device,
but its early telemetry lines were not retained. These static measurements do
not establish double-buffer viability, sustained bandwidth, or live-emulator
performance.

The diagnostic held each candidate image visible for approximately 30 seconds
as validation-harness behavior only. This is not a production cadence,
framebuffer lifetime requirement, refresh interval, or presentation timeout.
The earlier Step 7B.2a native foundation retains its separate approximately
five-second bounded hold.

For the CCW physical run, UART capture began after early boot, so the initial
startup lines were not preserved in the saved capture. The CCW terminal
runtime/cleanup result was captured, physical display output was observed by
the human operator, and the CCW visual result was PASS. This is a recorded
evidence limitation, not a Step 7B.2b blocking failure; it must not be
described as a complete boot-to-cleanup UART transcript.

The architectural result is now explicit:

```text
P4-NANO CANONICAL ROTATION: COUNTERCLOCKWISE

640x400 RGB565
 -> exact nearest-neighbor 2x
 -> logical 1280x800 landscape
 -> 90-degree COUNTERCLOCKWISE rotation
 -> physical 800x1280 portrait framebuffer
```

CCW is a P4-NANO board/display policy result, not a universal rule for future
display backends. The reference remains capable of both CW and CCW; TAB5,
ESP32-S31, and ESP32-S3 must not inherit CCW without their own geometry and
orientation decision. No speculative global orientation abstraction is added.

Step 7B.2a remains independently COMPLETE for the native display foundation:
safe JD9365/P4-NANO initialization, one native framebuffer, static native
diagnostic, and safe bounded backlight lifecycle. Step 7B.2b adds the
pixel-exact landscape-to-native transform reference and physical orientation;
neither step completes the live display pipeline.

### Step 7B.2c: immutable presentation frame -> physical display consumer — COMPLETE

Status: **IMPLEMENTED / END-TO-END BYTE-EXACT VALIDATED / REAL-HARDWARE LIVE
PRESENTATION VALIDATED / REAL-LCD VISUALLY VALIDATED / COMPLETE** for the
bounded one-frame Step 7A text-scene integration scope.

The first actual NP2-to-LCD path is now connected:

```text
NP2 renderer
 -> mutable 640x400 guest framebuffer
 -> synchronous scrnmng publication hook
 -> Step 7B.1 two-slot presentation publisher
 -> immutable ACQUIRED 640x400 RGB565 frame
 -> P4-NANO live display consumer
 -> exact 2x + canonical COUNTERCLOCKWISE transform
 -> one native 800x1280 RGB565 DSI framebuffer
 -> physical JD9365 LCD
```

The bounded producer was the existing Step 7A `np2video_runner`, which executes
the real NP2 core through `pccore_exec()` and the real scrnmng rendering and
publication path. The selected scene was the existing `NP2 VIDEO FIXTURE 7A.3A`
text fixture with source framebuffer golden CRC32 `0x0a280896`. This producer
was useful because it is deterministic, exercises the real NP2 path, avoids
adding SDMMC/audio/input/touch concurrency, and terminates in a bounded manner.
The synthetic `np2presentation_probe` was not used as the physical producer.

The Step 7B.1 ownership contract remains intact. Exactly two presentation slots
were allocated from PSRAM, 512,000 bytes each (1,024,000 bytes total). Both
slots were external and disjoint, and did not alias the mutable guest
framebuffer or the native DSI framebuffer. The consumer read only a frame
returned by `np2_presentation_acquire()` and held its matching token throughout
source validation and transformation.

The validated consumer sequence was:

```text
acquire immutable frame
 -> validate 640x400 RGB565 metadata
 -> calculate source CRC
 -> exact fused 2x + CCW transform
 -> native 800x1280 framebuffer
 -> verify source remained unchanged
 -> full native framebuffer cache synchronization
 -> release presentation token
```

The P4-NANO mapping is:

```text
640x400 RGB565
 -> exact nearest-neighbor 2x
 -> logical 1280x800 landscape
 -> 90-degree COUNTERCLOCKWISE
 -> physical 800x1280 RGB565
```

There is no `1280x800` intermediate framebuffer, second native framebuffer,
PPA, DMA2D, SIMD optimization, or per-frame allocation. The display uses
`num_fbs=1` and one native framebuffer of 2,048,000 bytes.

#### End-to-end CRC and lifecycle evidence

The final NP2 source CRC32 was `0x0a280896`, matching the existing Step 7A
golden:

```text
FINAL SOURCE GOLDEN RESULT: PASS
```

The host-derived expected CCW native framebuffer CRC32 was `0xe623a22a`, and
the real P4-NANO final native framebuffer CRC32 was also `0xe623a22a`:

```text
FINAL NATIVE TRANSFORM GOLDEN RESULT: PASS
```

This is byte-exact evidence across the selected bounded scene's NP2 source,
presentation copy, immutable acquired frame, CPU reference transform, and
native framebuffer. The physical LCD result below is separate human evidence.

The observed hardware counters were:

```text
submitted=1 acquired=1 transformed=1 released=1 coalesced=0 dropped=0
```

These counters prove one complete producer-to-consumer lifecycle only. They do
not characterize sustained multi-frame behavior, and zero coalescing/dropping
does not imply that those values remain zero under a continuous workload.

The presentation token remained held during consumption and the source was
checked before and after transformation. The integration completed with
`P4_NANO_LIVE_FRAME_IMMUTABLE=PASS`; however, the saved UART capture began late
and does not retain that early marker or all startup lines. The retained final
`P4_NANO_LIVE_RESULT=PASS`, final CRCs, counters, transform timing, and cleanup
are consistent with the satisfied condition. This is an evidence-capture
limitation, not a runtime failure, and the saved transcript is not a complete
boot-to-cleanup UART log.

#### Real hardware and visual result

The validation environment was the Waveshare ESP32-P4-NANO-KIT-D with ESP32-P4
revision v1.3, 32 MiB PSRAM, Waveshare 10.1-DSI-TOUCH-A/JD9365 display path,
and ESP-IDF v5.5.4. The native output was 800x1280 RGB565, 2,048,000 bytes,
with `num_fbs=1` and canonical COUNTERCLOCKWISE rotation.

The CPU reference transform timing was one sample: count=1, min=107,725 us,
max=107,725 us, average=107,725 us (approximately 107.725 ms). This is a
correctness baseline and future optimization input, not an achieved display
framerate, LCD refresh measurement, or sustained workload result. The
measured interval brackets `transform_to_native()` only; UART logging, source
immutability re-checks, and framebuffer cache synchronization occur outside this
interval. The theoretical serial transform-only ceiling of approximately 9.28 transforms/s
must not be treated as an achieved FPS value.

The backlight remained OFF until a valid frame had been acquired, transformed,
and cache-synchronized. The qualified conservative value `0x40` was then used;
the final frame remained visible for approximately 30 seconds for human
inspection. Cleanup completed with backlight OFF and
`P4_NANO_LIVE_DISPLAY_RESULT=PASS`. The 30-second interval is validation-harness
behavior only, not production cadence, refresh period, presentation timeout, or
frame lifetime.

The operator physically inspected the LCD and reported
`HUMAN_VISUAL_RESULT=PASS`. The actual NP2-generated `NP2 VIDEO FIXTURE 7A.3A`
content was naturally upright, occupied the expected full mapping, and showed
no gross clipping, unexpected mirroring, 180-degree reversal, or obvious static
corruption. No tearing or corruption was reported during this bounded
observation. The selected text fixture is essentially black/white, so this run
did not independently visually prove live-NP2 RGB component ordering. Step
7B.2b independently supplied physical RGB-order evidence using a color
diagnostic through the same transform and native display path; this is prior
path evidence, not a claim that colored live NP2 content was visually validated
here.

#### Fixture provisioning and scope boundary

The selected Step 7A fixture image SHA256 was
`f4ae6584339cbdb94e80e6fb48f9a27724fee7a9f350668b618d33b2794c8eca`. Hardware
provisioning wrote only the required normal firmware images and approved
`np2test`/video-fixture region. No full-flash erase, FATFS/storage modification,
or SD-card modification was performed. This does not validate storage behavior.

Step 7B.2c does not claim arbitrary PC-98 software display, sustained
multi-frame behavior, production framerate, sustained transform throughput,
presentation-to-display latency, animated tearing characterization or
elimination, measured panel refresh, framebuffer switching/reuse, second native
framebuffer viability, long-duration stability, live-load PSRAM bandwidth,
optimized transform path, PPA acceleration, display plus SDMMC/audio
concurrency, touch, LVGL, OSD, TAB5, ESP32-S31, or ESP32-S3 display backends.

Step 7B.2d sustained live presentation and transform validation is complete for
the reviewed P4-NANO path. The transform TU is promoted to `-O2` by default for
normal live, LIVE benchmark, isolated benchmark, and transform-diagnostic
profiles; explicit `--transform-opt debug` remains the `-Og` reference escape
hatch. Physical evidence measured approximately 39.3% isolated and 49.9% LIVE
transform improvement, while producer-associated shared execution/memory
contention decreased from approximately 78.6 ms to 28.0 ms. Correctness,
scheduler/TWDT safety, and host CRC checks passed, with a 16-byte LIVE
benchmark app increase. These are transform processing capacity results, not
guest/display FPS or raw-PSRAM-bandwidth measurements. Long-duration stability,
refresh/tearing characterization, PPA, and display plus SDMMC/audio concurrency
remain future work; the Step 7B.2b CPU-fused implementation remains the
correctness oracle.
Motion validation is intentionally separate and now has the automated
`--live-display-motion-validation` profile. It uses **AUTOMATED PROBE FIRST**:
guest multi-frame content, presentation sequence/content progression,
moving-bar ROI, and native-framebuffer ROI are checked in a bounded one-shot
run; camera/video analysis and human visual confirmation remain fallback-only.
This software profile does not claim that a physical panel displayed every
frame.

## Remaining hardware boundary

The software-only portion through Step 7B.1c and the bounded physical display
milestones through Step 7B.2c are complete. Remaining Step 6B/6C lifecycle
cases, sustained live display behavior, arbitrary guest software, colored live
NP2 visual validation, performance characterization, tearing and refresh
measurement, framebuffer reuse/synchronization, long-duration stability,
live-load PSRAM bandwidth, PPA, display plus SDMMC/audio concurrency, touch,
LVGL, OSD, TAB5 physical display, ESP32-S31, and ESP32-S3 display backends
remain FUTURE / UNVALIDATED. Portable emulator, host, and documentation work
can continue independently.

## Step 8: USB HID / input — FUTURE, hardware required

The future input stage covers USB keyboard, USB mouse, and touch coordinates
where board support is available. No physical input transport is validated.

## Step 9: audio — FUTURE, hardware required

The future audio stage covers beep, SSG, FM, YM2608 rhythm, ADPCM, and PCM86
output policy and board integration. No physical audio path is validated.
