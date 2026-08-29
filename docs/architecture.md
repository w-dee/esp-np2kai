# Architecture

The project separates the portable PC-9801 emulation model from Espressif
platform and board details. ESP32-P4 is the initial implementation platform;
the architecture should also leave room for a future ESP32-S31 implementation
without making S31 current work.

## Dependency direction

The conceptual dependency direction is:

```text
NP2 / NP2kai emulator core
        |
        v
portable host services and interfaces
        |
        v
Espressif-common platform
        |
        +-- SoC-specific implementation: ESP32-P4
        |       |
        |       +-- board-specific implementation: P4-NANO
        |       +-- board-specific implementation: TAB5
        |
        +-- SoC-specific implementation: ESP32-S31
                |
                +-- board-specific implementation: S31 Korvo-1
                    future, not implemented
```

The emulator core depends only on portable emulator and host-facing interfaces.
The Espressif-common layer may depend on those interfaces, SoC implementations
may depend on the common layer, and board implementations may depend on their
SoC and common platform layers. ESP32-P4-specific or ESP32-S31-specific types,
APIs, peripheral assumptions, pin assignments, memory mechanisms, and driver
objects must not leak above their respective SoC implementation layers.

P4-NANO and TAB5 use the same ESP32-P4 SoC family, so their differences belong
primarily in board-specific implementations. S31 Korvo-1 represents a
different SoC family, so a board-only abstraction would not be sufficient for
future support.

## Future source organization

The following is a non-binding future organization sketch, not a current
implementation requirement:

```text
firmware/
└── components/
    ├── np2core/
    ├── np2host/
    └── platform/
        └── espressif/
            ├── common/
            ├── soc/
            │   ├── esp32p4/
            │   └── esp32s31/       # future
            └── boards/
                ├── p4_nano/
                ├── tab5/
                └── s31_korvo_1/    # future
```

These empty directories must not be created at this stage. Introduce the
organization incrementally as the implementation grows and concrete
dependency boundaries require it.

## Vendored NP2kai core boundary

Step 3 establishes a fixed, byte-preserved NP2kai snapshot under
`third_party/np2kai/`. It is not a Git submodule or an upstream working-tree
copy. The machine-authoritative import definition is
[`import-manifest.json`](../third_party/np2kai/import-manifest.json); its
upstream SHA, not the observed `wx_alpha` branch name, is the reproducibility
anchor. The vendor-local
[`README.md`](../third_party/np2kai/README.md) is the generated human-readable
summary.

The imported tree is an inspected, byte-preserved source set for the initial
baseline. The allowlist alone is not a general dependency-closure guarantee:
different feature configurations may require different sources. The current
Step 4 Ubuntu-native configuration has nevertheless proven its bounded
closure: 124 vendor translation units plus 10 host translation units compile,
link as a relocatable unit, and leave zero project/non-system unresolved
symbols.

The baseline intent is:

- i286 CPU
- guest video core included; host display backend absent
- guest sound state retained at the source-inspection-selected minimum;
  sound generation and host audio backend absent
- frontend absent, network disabled, and minimum single-thread operation

Desktop frontends, i386/HAXM and other optimized cores, network/VST domains,
optional FM/MAME sound generators, optional Cirrus/TGUI graphics backends, and
ROM or disk-image assets are outside this boundary. The exact machine-readable
exclusion list is maintained in the manifest rather than duplicated here.

Optional implementation domains are excluded where they are not required by
the Step 3 baseline. Exact file membership is authoritative in the manifest;
Step 4 compiler/linker evidence may show that the candidate source set needs to
change. Any such change requires an explicit allowlist update, provenance and
license-evidence review, deterministic regeneration, verification, and human
review. This source-set policy does not establish optional device runtime
support.

Host contracts and adapters remain outside the vendor tree. The Step 4
configuration validates compile/link and bounded startup/CPU execution through
the Ubuntu-native headless path. Display, audio, input integration, broader
software compatibility, target/physical storage integration such as physical
microSD/SDMMC and real media, ESP32-P4 firmware integration, and hardware
validation remain separate later scopes. At the Step 4 milestone, storage was
limited to the read-only tracked FD1232 FDD image path used to boot the formal
Stage-1 golden; Step 6A later adds a separate emulator FATFS/VFS path without
changing that oracle.

## Ubuntu-native execution boundary

The formal NP2TEST Stage-1 guest publishes a 128-byte `result-v1` memory block.
The Ubuntu-native execution pipeline is:

```text
formal NP2TEST guest
        |
        v
result-v1 memory block
        |
        v
result-v1 parser
        |
        v
deterministic execution controller
        |
        v
Ubuntu-native headless runner orchestration
```

The parser validates the guest block and the controller owns returned-slice
budgets and normalized outcomes. A separate Python external supervisor bounds
the host process and wall clock. The deterministic pre-running/running limits
(512 and 4096 returned slices) measure guest/protocol progress; the supervisor's
30-second limit handles a non-returning `pccore_exec(FALSE)` call or another
process-level failure. These timeout domains are intentionally distinct.

The current formal golden run is measured evidence: 13/13 tests pass with
result-v1 CRC `0x58f5b827`. First protocol evidence was observed at returned
slice 202 and terminal `PASS` at returned slice 203; those observations are not
architectural guarantees.

## ESP32-P4 headless core integration

The implemented firmware dependency graph is:

```text
main
  +-- uart_control_transport
  +-- np2memoryprobe -> np2core
  +-- np2fixtureprobe -> np2fixture -> np2host
  +-- np2test_runner -> np2core + np2host + np2fixture
                         + shared Stage-1 configuration/parser/controller
```

The dedicated runner directly owns the one-shot NP2 lifecycle through
`pccore_init()`, `pccore_reset()`, `pccore_exec()`, and `pccore_term()`. The
validated 124-TU NP2core is connected through the ESP32-P4 np2host boundary,
with PSRAM external BSS support. External BSS placement is provided by
ESP-IDF linker facilities rather than vendor-source ESP-specific attributes.

The formal NP2TEST fixture is stored in a dedicated read-only NOR partition.
Its raw access path uses mmap/read-only DOSIO and validates the vendor FDD/XDF
path; it remains an independent oracle and is not replaced by FATFS. The
formal Stage-1 configuration, result-v1 parser, and execution controller are
shared with the native headless oracle, while the ESP32-P4 runner remains a
platform-side adapter around the core lifecycle. Step 6A additionally mounts
FATFS/WL and exercises the same NP2 path through generic VFS/DOSIO.

## Validation-layer boundary

Ubuntu-native execution validates portable core behavior and is the fastest
reference layer. It is not the target ESP32-P4 firmware. `esp-emu` validates
actual ESP32-P4 firmware and integration, while real ESP32-P4 hardware is the
layer for unsupported peripherals, real timing, performance, and board
transport. The firmware integration is implemented and validated under the
non-formal reduced emulator profile. Formal `EXTMEM=13` runtime evidence is
tracked separately. Real-hardware evidence is now scoped rather than wholly
pending: P4-NANO UART/SDMMC and production Host-to-Device File Transfer are
validated at 1.5 Mbps, while display, input, audio, TAB5, removal/durability,
and other unqualified paths remain pending.

## Conceptual layers

- **NP2/NP2kai emulator core**: guest CPU, memory, timers, I/O devices, disk
  formats, and emulation state. Imported core code should remain substantially
  C unless a compelling reason requires a change.
- **Portable host services and interfaces**: coarse boundaries for display,
  audio, storage, keyboard/mouse input, touch input, connectivity, timing,
  useful memory allocation services, and debug/control. These interfaces
  describe emulator and guest operations rather than a specific SoC.
- **Espressif-common platform**: common scheduling, clocks, timing, logging,
  capability-aware resource handling, and other services shared across
  Espressif SoC implementations where appropriate.
- **SoC-specific implementation**: ESP32-P4 or a future ESP32-S31 implementation
  containing the target's CPU integration, memory mechanisms, peripheral
  access, interrupt details, and SoC-specific drivers.
- **Board-specific implementation**: board discovery, pin assignments,
  physical display and codec wiring, storage connections, input devices, and
  other operations specific to P4-NANO, TAB5, S31 Korvo-1, or another board.

## Portable host interfaces

The emulator should expose coarse platform-independent boundaries for:

- display output
- audio output
- storage and file access
- keyboard and mouse input
- touch input
- network or other connectivity
- timing and scheduling services
- memory allocation services where an abstraction is useful
- debug and control operations

The documentation defines these boundaries and dependency direction only. It
does not require a particular C++ class hierarchy.

The eventual emulator should not care whether a keyboard or mouse event comes
from physical USB HID, UART debug/control injection, or another board-specific
input path. The transport and device details remain below the portable input
boundary.

## Video framebuffer and presentation boundary

Step 7A implements one mutable guest framebuffer owned by `scrnmng`. It uses
RGB565LE data, dynamic guest geometry, and an ESP32-P4 implementation backed by
external PSRAM. Step 7B.1 implements the portable presentation boundary below
the renderer and above any physical display policy:

```text
NP2 / NP2kai renderer
        |
        v
scrnmng single mutable guest framebuffer
  RGB565LE
        |
        | synchronous borrowed view during scrnmng_surfunlock()
        v
portable np2_presentation publisher
  two caller-owned slots
        |
        v
immutable ACQUIRED frame
        |
        v
platform / SoC presentation consumer
  (P4-NANO qualified through P10M; other backends future)
        |
        v
board display policy
  (P4-NANO CCW/JD9365 qualified through P10M; other boards future)
        |
        v
physical panel
  (P4-NANO JD9365 qualified in bounded scopes; other panels future)
```

The first four stages are implemented and verified by Ubuntu-native tests and
ESP32-P4 / FreeRTOS / `esp-emu` probes. For the P4-NANO target, the final three
stages have bounded physical validation through Step 7B.2c and the current
P10M correctness/performance profiles through the JD9365 panel. The ESP32-P4
consumer probe remains a separate ownership and concurrency regression; it is
not the physical display driver or panel validation. Other boards, display
policies, and panels remain future.

The portable publisher has exactly two caller-owned presentation slots in the
tested contract. It uses the states `FREE`, `WRITING`, `PENDING`, and
`ACQUIRED`, with one producer and one consumer. The producer reclaims or
replaces `PENDING` first, otherwise uses `FREE`, and never waits for or
overwrites `ACQUIRED`. The consumer holds at most one `ACQUIRED` frame, acquires
the latest pending frame, and releases it back to `FREE`. This is the
latest-frame-wins policy: pending frames may coalesce, while an acquired frame
remains immutable. The publisher performs no per-frame allocation, has no
unbounded queue, and never gives the consumer the guest framebuffer pointer.

### Synchronous scrnmng publication contract

The implemented hook ordering is:

```text
renderer completes framebuffer writes
        -> synchronous publish hook
        -> scrnmng unlock state cleared
        -> Step 7A surface_update_sequence increment
        -> pending resize may execute
```

`SCRNMNG_PUBLISH_VIEW` contains a borrowed guest pointer. It is valid only for
the synchronous callback and must not be retained. The callback may perform a
bounded copy, but must not wait for the consumer, allocate, perform normal
frame logging, or call `scrnmng` reentrantly. The publisher copies compact
visible RGB565LE rows into its caller-owned slots; its published frame has a
packed pitch of `width * 2`.

### Framebuffer ownership and lifetime

The guest framebuffer and presentation slots are separate storage domains:

- the guest framebuffer is one mutable renderer-owned `SCRNSURF`, has dynamic
  geometry, may be reallocated on resize, and is stored in ESP32-P4 PSRAM;
- presentation storage consists of two independent caller-owned buffers with
  disjoint backing ranges and an independent lifetime from the guest buffer.

The tested 640x400 RGB565LE contract uses 512000 bytes per presentation slot.
This is a geometry-derived test value, not a universal allocation constant.
An already acquired frame remains valid across a guest resize because it owns
its copied slot. A subsequent publication carries the new source generation.
If a geometry exceeds the fixed slot capacity, publication may fail or drop
without becoming a guest renderer failure.

### Presentation sequences and concurrency

The counters have distinct meanings:

- `surface_update_sequence` is the existing Step 7A scrnmng completion/update
  counter;
- `published_sequence` advances for successful presentation publications;
- `coalesced_count` counts pending presentation frames replaced before acquire;
- `dropped_count` counts submissions that were not published.

None of these is a physical refresh, VSYNC, or displayed-frame count. A future
physical backend may add a separate presented/displayed sequence.

The publisher is a C11 single-producer/single-consumer implementation using
32-bit atomic slot states with acquire/release publication ordering. The
ESP32-P4 / `esp-emu` evidence shows that these 32-bit state atomics are
lock-free in the tested ESP-IDF v5.5.4 toolchain/runtime. That result is not an
automatic guarantee for every future architecture. The accepted bounded
32-bit lease-token wrap domain is an implementation safety limit, not a
physical display guarantee.

## Physical display boundary

Guest framebuffer geometry remains independent of P4-NANO's panel, TAB5's
panel, PPA, MIPI-DSI, LCD driver type, and board pin assignments. The current
qualified P4-NANO path is downstream of the immutable presentation frame and
uses the P10M exact2x transform pipeline described below. The earlier physical
foundation and scalar transform remain valid historical milestones for their
bounded scopes.

### Historical Step 7B.2a physical foundation

The real run used a Waveshare ESP32-P4-NANO-KIT-D with ESP32-P4 rev v1.3,
40 MHz crystal, 32 MiB PSRAM, and a JD9365 panel reporting ID `93 65 04`.
The physical flash reported 16 MiB while the project intentionally retained
the existing 8 MiB validation envelope. The production path completed shared
I2C, panel power, LDO, DSI, DBI, panel, framebuffer, BLACK/cache, display-on,
static-pattern/cache, and backlight stages without panic, watchdog, reset loop,
DSI underrun, I2C error, or framebuffer allocation failure.

The native diagnostic scanout was 800x1280 RGB565, stride 1600 bytes, one DSI/DPI
framebuffer of 2,048,000 bytes, two lanes at 1500 Mbps/lane, and an 80 MHz DPI
clock. The calculated refresh value is nominally approximately 68.66 Hz; it is
not a measured refresh result. Runtime CRC32 `0x5383260a` matched the host
golden. This proves byte-level agreement for the generated framebuffer, not
physical LCD correctness; human inspection separately passed the LCD output
twice with all four edge colors, corner markers, portrait geometry, RGB order,
and no clipping or obvious static corruption.

Measured PSRAM telemetry was 33,551,868 free / 33,030,144 largest block before
display init and 31,502,616 free / 31,457,280 largest block after the one native
framebuffer. The reported free-heap delta was 2,049,252 bytes versus the
2,048,000-byte payload; allocator, alignment, and driver bookkeeping must not
be inferred as double-buffer viability.

### Historical Step 7B.2b: CPU/scalar transform reference

Step 7B.2b is COMPLETE for the bounded pixel-exact transform reference and
static physical-transform validation. The project-owned C++20 implementation
consumes an immutable `640x400 RGB565` source, applies exact nearest-neighbor
2x to a logical `1280x800` landscape, and writes directly to one native
`800x1280 RGB565` destination:

```text
640x400 RGB565 -> exact 2x -> logical 1280x800 landscape
               -> quarter-turn -> native 800x1280 RGB565
```

The transform is fused. It has no `1280x800` intermediate framebuffer, filtering,
bilinear interpolation, color conversion, RGB565 modification, PPA, DMA2D,
per-frame dynamic allocation, live NP2 consumer, or presentation-slot consumer.
Each input pixel becomes exactly four identical output pixels. The API retains
both mathematical mappings:

```text
CLOCKWISE:
dst_x = 799 - (2 * sy + oy)
dst_y =       2 * sx + ox

COUNTERCLOCKWISE:
dst_x =       2 * sy + oy
dst_y = 1279 - (2 * sx + ox)
```

where `ox, oy ∈ {0, 1}`. The host reference test exhaustively compares both
directions against an independent inverse-mapping oracle over every destination
pixel, including full coverage, corners, edge centers, asymmetric interior
points, canaries, bounds, exact 2x duplication, geometry, destination bytes,
and invalid input sizes. Frozen reference CRCs are CW `0xdb938d53` and CCW
`0x164584cf`; CRCs are regression goldens, not the sole correctness proof.

The separate physical diagnostic uses a real deterministic `640x400 RGB565`
source with four distinct edges, four distinct corners, and asymmetric interior
markers. Its source CRC is `0x4291f7e5`; transformed diagnostic CRCs are CW
`0x37fd7262` and CCW `0xd98ce5d4`. The source is transformed into the native
destination rather than constructing a final `800x1280` source image directly.

The physical validation used the qualified Waveshare ESP32-P4-NANO-KIT-D and
Waveshare 10.1-DSI-TOUCH-A/JD9365 path: ESP32-P4 revision v1.3, 40 MHz crystal,
ESP-IDF v5.5.4, 32 MiB PSRAM, native 800x1280 RGB565, one 2,048,000-byte DSI
framebuffer, `num_fbs=1`, two MIPI-DSI lanes at 1500 Mbps/lane, and an 80 MHz
DPI clock. Both CW and CCW candidates built and ran; both had successful
bounded display cleanup and human visual inspection. COUNTERCLOCKWISE is the
canonical P4-NANO rotation because the human operator identified it as the
natural upright orientation on the installed assembly. This is a board/display
policy result, not a universal rule for future backends; TAB5, ESP32-S31, and
ESP32-S3 require independent geometry/orientation decisions.

The diagnostic holds each candidate image visibly for approximately 30 seconds
as validation-harness behavior only. This is not a production cadence,
framebuffer lifetime, refresh interval, or presentation timeout. The earlier
Step 7B.2a native diagnostic retains its separate approximately five-second
hold.

For the CW run, the retained runtime evidence includes source and destination
geometry/CRC, one framebuffer, PSRAM allocation telemetry, cache synchronization,
backlight `0x40`, 30-second hold, and final cleanup. The CCW UART capture began
after early boot, so its initial startup lines were not retained; the terminal
runtime/cleanup result and human physical result were retained. This is an
evidence limitation, not a blocking failure, because the CCW path is also
independently host-validated and its firmware build passed.

The CW telemetry measured PSRAM free/largest-block values of `33,551,868 /
33,030,144` before the source allocation, `33,035,768 / 33,030,144` after the
512,000-byte source allocation, and `30,986,520 / 30,932,992` after the native
framebuffer acquisition. The CCW run established the same allocation model on
the real 32 MiB PSRAM hardware, but its early telemetry lines were not retained.
These static measurements do not establish double-buffer viability, sustained
bandwidth, or live-emulator performance.

### Historical Step 7B.2c: first live NP2-to-LCD integration

Step 7B.2c is COMPLETE for the bounded first live NP2-to-LCD integration:

**IMPLEMENTED / END-TO-END BYTE-EXACT VALIDATED / REAL-HARDWARE LIVE
PRESENTATION VALIDATED / REAL-LCD VISUALLY VALIDATED / COMPLETE**

The completed boundary is:

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

The bounded producer was the existing Step 7A `np2video_runner`, executing the
real NP2 core through `pccore_exec()` and the real scrnmng render/publication
path. It used the existing deterministic `NP2 VIDEO FIXTURE 7A.3A` text scene,
whose source framebuffer golden is `0x0a280896`. This avoided introducing
SDMMC, audio, input, or touch concurrency while providing a bounded first live
producer; the synthetic `np2presentation_probe` was not the physical producer.

The Step 7B.1 ownership boundary remains unchanged. Exactly two external-PSRAM
presentation slots were used, 512,000 bytes each (1,024,000 bytes total), and
the slots were disjoint from one another, the mutable guest framebuffer, and
the native DSI framebuffer. The consumer reads only a frame returned by
`np2_presentation_acquire()`. Its matching token remains held for the complete
source validation and transform operation and is released only after the
source is no longer read.

The consumer sequence is:

```text
acquire immutable frame
    -> validate 640x400 RGB565 metadata
    -> calculate source CRC
    -> exact fused 2x + CCW transform
    -> native 800x1280 framebuffer
    -> verify source immutability
    -> full native framebuffer cache synchronization
    -> release presentation token
```

The P4-NANO mapping remains:

```text
640x400 RGB565
    -> exact nearest-neighbor 2x
    -> logical 1280x800 landscape
    -> 90-degree COUNTERCLOCKWISE
    -> native 800x1280 RGB565
```

There is no `1280x800` intermediate framebuffer, second native framebuffer, PPA,
DMA2D, SIMD optimization, or per-frame allocation. The native display remains
`num_fbs=1` with one 2,048,000-byte framebuffer.

The final NP2 source CRC was `0x0a280896`, matching the existing Step 7A
golden. The host-derived expected CCW native CRC and the real P4-NANO final
native CRC both were `0xe623a22a`:

```text
FINAL SOURCE GOLDEN RESULT: PASS
FINAL NATIVE TRANSFORM GOLDEN RESULT: PASS
```

This is byte-exact evidence across the selected bounded scene's NP2 source,
presentation copy, immutable acquisition, CPU reference transform, and native
framebuffer. The physical LCD result is separate human evidence.

The observed bounded hardware counters were submitted=1, acquired=1,
transformed=1, released=1, coalesced=0, and dropped=0. They prove one complete
producer-to-consumer lifecycle only; they do not characterize sustained
multi-frame behavior or imply that coalescing/dropping will remain zero under a
continuous workload. The integration retained the token during consumption and
completed its source-before/after immutability condition with
`P4_NANO_LIVE_FRAME_IMMUTABLE=PASS`. The saved UART transcript began late and
does not retain that early marker or all startup lines; the final
`P4_NANO_LIVE_RESULT=PASS`, CRCs, counters, and cleanup are retained. This is an
evidence-capture limitation, not a runtime failure.

The real-hardware scope was Waveshare ESP32-P4-NANO-KIT-D, ESP32-P4 revision
v1.3, 32 MiB PSRAM, Waveshare 10.1-DSI-TOUCH-A/JD9365, and ESP-IDF v5.5.4.
The CPU reference transform measured one sample: count=1, min/max/average
107,725 microseconds (approximately 107.725 ms). This is a correctness
baseline and optimization input, not an achieved frame rate, LCD refresh
measurement, or sustained workload result. The display stayed backlight-OFF
until a valid frame was transformed and cache-synchronized, then used the
qualified `0x40` value, remained visible for approximately 30 seconds, and
finished with `P4_NANO_LIVE_DISPLAY_RESULT=PASS` and backlight OFF.

The operator separately inspected the physical LCD and reported
`HUMAN_VISUAL_RESULT=PASS`: the actual NP2 text scene was naturally upright,
mapped across the expected display area, and showed no gross clipping,
mirroring, 180-degree reversal, or obvious static corruption. No tearing or
corruption was reported during this bounded observation. The selected text
fixture is essentially black/white, so this run did not independently visually
re-demonstrate live-NP2 RGB component ordering. Step 7B.2b already supplied
physical RGB-order evidence using a color diagnostic through the same transform
and display path; this is prior path evidence, not a claim that colored live NP2
content was validated here.

### Historical Step 7B.2d: sustained scalar/motion validation

Step 7B.2d sustained live presentation and transform validation is complete for
the reviewed P4-NANO path. Transform processing is `-O2` by default for
transform-using profiles, while explicit `--transform-opt debug` preserves the
validated `-Og` reference path. Isolated transform improvement was
approximately 39.3% (`-Og`/`-O2`: 107.7/65.4 ms); LIVE improvement was
approximately 49.9% (186.3/93.4 ms), and producer-associated shared
execution/memory contention decreased from roughly 78.6 ms to 28.0 ms.
Correctness, scheduler/TWDT safety, and host CRC regressions passed with an
approximately 16-byte LIVE benchmark app increase. These are transform
processing-capacity results, not guest/display FPS or a measured raw-PSRAM-
bandwidth improvement.

The `--live-display-motion-validation` profile is also physically validated on
the ESP32-P4 v1.3 P4-NANO with fixture `np2video-7b2d-live-vram`, SHA256
`81975ad74c7b1769a5aa63977ee9c18b020d6381e858522cb4cb7c7861f85604`, and the
production-default `-O2` CCW 2x transform. The authoritative result was
`MOTION_VALIDATION_RESULT=PASS` with `acquired=60`, `clean=16`, `distinct=16`,
`repeated=0`, `transitional=44`, `invalid_position=0`, `native_pass=16`,
`native_fail=0`, `submitted=61`, `released=61`, `coalesced=0`, `dropped=0`,
published/source sequences `1..60`, and `reason=NONE`; the run returned from
`app_main()` without TWDT, idle starvation, panic, Guru Meditation, or reset.
Sixteen distinct positions were detected from genuine acquired presentation
frames, and every corresponding native framebuffer band passed the same-frame
CCW mapping. The independent oracle was
`guest_x_start = guest_bar_pos * 8`,
`native_y_min = 1152 - 16 * guest_bar_pos`, and
`native_y_max = 1279 - 16 * guest_bar_pos`.

The 44 transitional/non-clean samples are expected from sequential guest
graphics-plane updates and asynchronous presentation. They were skipped, not
treated as failures or performance/displayed-frame ratios. The validator
propagates each sample's detected uniform nonzero RGB565 color and does not
require one fixed palette value; multiple colors were observed. This is a
spatial/structural motion proof, not final palette or RGB-order evidence.

The motion PASS proves software-visible motion through the guest framebuffer,
immutable presentation lease, transform, cache synchronization, and native
framebuffer. It does not prove that every native frame was physically scanned
out, tear-free behavior, refresh-boundary coherence, panel refresh rate, or
DSI/GDMA temporal behavior. There is no independent reproducible evidence that
the panel is frozen, so an earlier missed human observation does not open a
scanout-debug milestone. At the close of Step 7B.2d, long-duration stability,
characterization, second native framebuffer, PPA, further transform structural
optimization, display plus SDMMC/audio concurrency, arbitrary guest
applications, touch, LVGL, OSD, TAB5, ESP32-S31, and ESP32-S3 display backends
remained FUTURE / UNVALIDATED. The policy remains **AUTOMATED PROBE FIRST**;
camera automation is unimplemented and unnecessary for this PASS, with human
visual confirmation retained as fallback-only.

### Current P10M exact2x path

The currently qualified P10M exact2x candidate path is:

```text
NP2 mutable 640x400 RGB565LE framebuffer
    -> synchronous scrnmng publication
    -> two-slot presentation publisher
    -> immutable ACQUIRED presentation frame
    -> P4-NANO display consumer
    -> PPA CCW90 rotation (400x128 internal tile)
    -> q0/q1 horizontal-only PIE exact x2 (800x64 internal staging)
    -> DMA2D EVEN/ODD strided vertical duplication
    -> one native 800x1280 RGB565 DSI framebuffer
    -> JD9365 LCD
```

The semantic mapping is exact nearest-neighbor 2x to logical 1280x800, followed
by COUNTERCLOCKWISE rotation to native 800x1280. There is no interpolation or
1280x800 intermediate framebuffer. The native policy is `num_fbs=1`; there is
no second native framebuffer.

The current P10M performance-qualified scanout is LOWER2: PLL_F240M / 7,
approximately 34.285713 MHz DPI, approximately 29.426767 Hz calculated
refresh, H total 880, V total 1324, two DSI lanes at 500 Mbps/lane, RGB565,
and one native framebuffer. Approximate scanout traffic is 57.47 MiB/s. The
refresh value is calculated/predicted, not a physical panel-refresh
measurement. This path is not yet the normal `--live-display` production
transform; that profile remains separately selected. The profile source is
[`p4_nano_display_timing_profiles.hpp`](../firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_timing_profiles.hpp).

P10M-C1C is **FORMAL REAL-HARDWARE BYTE-EXACT DMA2D CORRECTNESS = VALID / PASS**.
Each frame performs five PPA operations, ten horizontal PIE calls, and twenty
DMA2D transactions (ten EVEN and ten ODD). Retained formal evidence records
source CRC `0x8dadbf82` before and after, rotated CRC `0x379511d7`, and
destination CRC `0xc8a10b55`, with source immutability, scalar pixel mapping,
full reference `memcmp`, Idle cleanup, and no retained ambiguous lifetime.

P10M-D2C is **FORMAL SAME-BINARY DMA2D PERFORMANCE = VALID**, classified
**A-PROVISIONAL** because DMA timer-control perturbation remained YELLOW. The
retained formal real-hardware evidence reports:

| Metric | Result |
|---|---:|
| CONTROL transform CPU-unavailability proxy average | 25.041 ms |
| DMA2D transform CPU-unavailability proxy average | 2.690 ms |
| Proxy reduction | 89.258% |
| DMA2D exact2x average | 6.849 ms |
| DMA2D total-transform-service average / p99 | 16.343 / 16.431 ms |
| DMA2D blocked wait average | 4.121 ms |
| Blocked wait / exact2x service | 60.169% |
| Neutral PPA sentinel drift | 0.000% |

These exact values are supported by retained formal evidence but are not
machine-readable in tracked documentation. The CPU values are transform
CPU-unavailability proxies, not CPU utilization. Blocked wait is a potential
schedulable-opportunity upper bound, not free CPU time. CONTROL and DMA2D phase
PPA timings differed substantially while the neutral PPA sentinel remained
stable; this is method-coupled system-level evidence, not proof that DMA2D
causally made PPA faster.

The accepted path uses the ESP-IDF v5.5.4 private DMA2D API only through the
project-owned adapter. It shares the DMA2D pool with PPA but runs sequentially
as PPA -> PIE -> DMA. The formal path has no descriptor chaining and no
overlap; `dma2d_force_end()` is intentionally not used. If timeout, setup, or
start failure leaves callback/DMA ownership ambiguous, callback-reachable
resources are retained until one-shot shutdown rather than torn down
unsafely. Post-DMA destination synchronization uses
`ESP_CACHE_MSYNC_FLAG_DIR_M2C`; `DIR_M2C | UNALIGNED` is
rejected by the pinned IDF v5.5.4 and must not be prescribed.

The retained Step 7B.2a 80 MHz DPI, 1500 Mbps/lane, and nominal ~68.66 Hz
values are historical initial-foundation diagnostics, not the current LOWER2
configuration. Step 7B.2b remains the CPU/scalar correctness reference, 7B.2c
the first bounded one-frame live integration, and 7B.2d the sustained scalar
and motion validation milestone.

P10M does not establish tear-free physical scanout, refresh-boundary coherence,
that every transformed frame was physically scanned out, production viability
of a second native framebuffer, long-duration full emulator+display+audio
stability, final audio/FM coexistence performance, arbitrary guest-application
compatibility, TAB5/S31/S3 display qualification, or production touch/LVGL/OSD
integration. These remain FUTURE / UNVALIDATED.

## Audio boundary

Emulated sound generation feeds a generic audio output boundary. Codec type,
I2S/TDM implementation, DMA details, and board wiring belong below that
boundary. The scoped P4-NANO A3 path is now integrated and physically validated
for I2S0/APLL -> ES8311 -> NS4150B -> H4 on the Waveshare
ESP32-P4-NANO-KIT-D; see
[`p4-audio-a3-completion.md`](development/p4-audio-a3-completion.md). This
does not qualify TAB5 or broader audio/display coexistence.

## Storage, input, and connectivity

SD-card implementation details and USB-host implementation details remain below
portable storage and input interfaces. The core should not depend on whether
storage is provided by an SD card or another future backend.

Connectivity is described as a generic platform service. A particular ESP32-C6
path, radio, transport, or board wiring belongs in the relevant SoC or board
implementation rather than in the portable emulator layer.

## Realtime and language policy

New platform-side C++ may use C++20. C++ exceptions and RTTI are disabled, and
`iostream` is not used. RAII, `constexpr`, `enum class`, `std::array`,
`std::span`, `std::string_view`, `std::optional`, and `std::unique_ptr` are
preferred where appropriate; `std::shared_ptr` is avoided.

Imported NP2/NP2kai core code should remain substantially C. C/C++ linkage
boundaries are explicit with `extern "C"` where required.

There must be no heap allocation in realtime or hot paths such as guest CPU
execution, audio generation, video processing/transfer, or ISRs. Those paths
also avoid filesystem access and normal logging. Non-trivial global
constructors and static initialization-order dependencies are avoided.

Virtual dispatch is acceptable at coarse host or platform abstraction
boundaries, but not in inner CPU, audio, or video loops or ISRs. SoC-specific
virtual calls must not enter those hot paths.

## Memory policy

ESP-IDF capability-aware allocation is used when needed, with internal RAM,
DMA-capable RAM, and PSRAM treated as distinct resources. The emulator core
must not depend directly on ESP32-P4 memory APIs. SoC-specific allocation
capabilities are hidden below platform interfaces where necessary, and any
future ESP32-S31 memory model follows the same boundary.

## Debug/control plane

The debug/control plane remains SoC-independent at the command and protocol
level. Future commands such as the following describe guest or emulator
operations, not ESP32-P4-specific operations:

```text
input.key
input.mouse
emulator.reset
emulator.pause
emulator.resume
file.read
file.write
debug.memory.read
debug.memory.write
debug.io.read
debug.io.write
debug.cpu.get_registers
```

Conceptually, the protocol is separate from its transport:

```text
debug/control protocol
        |
        v
transport abstraction
   +-- UART
   +-- future USB transport
   +-- future network transport
```

The future guest/emulator commands listed above are documented here only and
are not implemented in the initial repository setup. The UART Control Plane
Base provides the first neutral transport/protocol/dispatcher path and is
verified under `esp-emu` v0.39.0 for the ESP32-P4 emulator environment. Its
protocol is independent of the configured console UART and of any
ESP32-P4-specific API. The configured-console path is also validated on the
P4-NANO onboard CH343P route at 1.5 Mbps; TAB5 remains unverified.

The skeleton uses a bounded `@ESP-NP2 ` JSON-lines frame, a central command
dispatcher, and a separate configured-console-UART transport. The existing
`ESP-NP2KAI HELLO WORLD OK` marker remains the Hello World milestone marker;
`ESP-NP2KAI UART CONTROL READY` is the separate control-task readiness marker.

## UART Binary Data Plane v1

The Binary Data Plane v1 is verified under ESP-IDF v5.5.4 and esp-emu v0.39.0
for the ESP32-P4 emulator environment. This verification covers the complete
bidirectional byte-path suite over esp-emu UART-TCP. The production
Host-to-Device subset used by File Transfer is additionally hardware-validated
on P4-NANO at 1.5 Mbps; this is not a claim that the complete bidirectional
suite or TAB5 transport has been physically qualified.

The responsibilities remain separate:

```text
UART RX -> control_stream
              +-- text   -> control_plane
              +-- binary -> binary_data_plane

control_plane -- JSON lifecycle/test commands --> binary_data_plane

JSON responses and binary frames -> common raw machine writer
                                  -> uart_control_transport
```

`control_stream` owns the bounded TEXT/START_ZERO/BINARY_COLLECT/
BINARY_DISCARD multiplexer. `binary_data_plane` owns portable framing, CRC,
transfer identity, sequence/offset state, generic endpoint lifecycles, and stop-and-
wait behavior. `uart_control_transport` composes these components with the
configured ESP-IDF console UART. The portable components do not contain board
pin policy or ESP32-P4-specific protocol types.

The binary envelope is exactly:

```text
00 00 COBS(decoded-frame) 00
```

The byte-stream layer also reserves exactly four consecutive NUL bytes as
`TRANSPORT_SYNC` (`00 00 00 00`). This token is detected by `control_stream`,
below the JSON/control framing parser. On completion it discards partial text
and COBS candidates and returns the stream to Text/seeking state. A longer NUL
run remains in that synchronization interval until a non-NUL byte arrives.
`TRANSPORT_SYNC` has no storage, reset, UART-configuration, or
TransferManager-session side effects; recovery of a higher-level binary or
file-transfer session is separate.

The host handshake is `TRANSPORT_SYNC`, then a uniquely identified
`protocol.hello`, then validation of its matching response. The READY line is
diagnostic only, so a host may attach after boot and need not have observed it.
The reserved token is collision-free for the current binary envelope because
COBS emits no NUL in the encoded body: one frame has at most two leading and
one trailing NUL, and directly adjacent frames have at most three at their
boundary. Valid JSON control frames contain no raw NUL.

The decoded Binary Data Plane v1 frame layout is:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | magic = `0x4e 0x42` (`NB`) |
| 2 | 1 | Binary Data Plane version = 1 |
| 3 | 1 | frame type |
| 4 | 2 | flags |
| 6 | 2 | header length = 28 |
| 8 | 4 | transfer ID |
| 12 | 4 | sequence |
| 16 | 8 | absolute offset |
| 24 | 2 | payload length |
| 26 | 2 | status / NACK reason |
| 28 | N | payload |
| 28+N | 4 | CRC-32/ISO-HDLC |

All multibyte fields are explicitly little-endian; native struct layout is not
serialized. The fixed header is 28 bytes, the maximum DATA payload is 1024
bytes, the maximum decoded frame is 1056 bytes, the maximum COBS body is 1061
bytes, and the maximum complete wire frame is 1064 bytes. CRC covers the
decoded header and payload and excludes the CRC field and COBS delimiters.
Future incompatible field widths, offsets, or bounds require a new Binary Data
Plane version rather than a silent v1 change.

The v1 frame types are `DATA = 0x01`, `ACK = 0x02`, and `NACK = 0x03`. JSON
remains responsible for lifecycle/control commands, which currently are:

```text
binary.test.rx.begin
binary.test.tx.begin
binary.transfer.status
binary.transfer.abort
```

The advertised additive capability is `binary.data-plane.v1`. These commands
remain deterministic transport tests; application services attach through the
generic bounded endpoint contract. v1 permits one active transfer total and
uses stop-and-wait.
Transfer IDs identify binary sessions and are independent of JSON request IDs.

ACK advances the receiver's acknowledged sequence and offset. A matching NACK
for the outstanding DATA frame causes immediate retransmission of that same
frame without advancing transfer progress or reapplying its payload CRC.
Timeout and NACK retries use one shared retransmission budget. A mismatched
NACK sequence or offset aborts the active session as protocol desynchronization;
it does not create a NACK loop, rewind, or arbitrary resume. Duplicate
Host-to-ESP32 DATA is applied once and receives an idempotent ACK.
The most recent successfully completed Host-to-Device transfer also retains a
bounded identity record for its exact final DATA frame. An identical replay
receives the same final progress ACK without reopening, consuming, finishing,
or committing the endpoint again; the record is replaced when a new transfer
successfully begins.

The integration test directly verifies deterministic 64 KiB transfers in both
directions, exact bytes, per-frame and whole-transfer CRC, Host-to-ESP32
duplicate DATA, corrupted CRC and `BAD_CRC` recovery, host-generated
Device-to-Host NACK retransmission of an identical DATA frame, and false-NUL
delimiter recovery followed by a successful JSON `system.ping`. Timeout retry,
shared retry-budget exhaustion, and mismatched-NACK abort are implemented v1
semantics established by the source, but are not claimed as separately
injected runtime cases by this test.

## File Transfer Base

The `file-transfer.v1` capability is verified under esp-emu v0.39.0 using this
composition:

```text
control_plane -> file_transfer -> storage <- storage_ram
                      |
                      v
             binary_data_plane -> UART
```

`file_transfer` owns canonical logical paths, metadata commands, ranges,
write replacement policy, and file/transport summaries. `storage` is a neutral
streaming interface with stat/list/read/write-session operations.
`storage_ram` is a deterministic test backend with a 256 KiB arena, 32 entries,
a 192 KiB per-file limit, one read session, and one staged write session.
Staged writes become visible only after commit; abort preserves an existing
target. No filesystem handle or ESP-IDF storage type crosses the interface.

The JSON commands are `file.stat`, `file.list`, `file.read.begin`,
`file.write.begin`, and `file.transfer.status`. Paths are absolute UTF-8 logical
paths rooted at `/`; traversal, repeated separators, controls, backslashes,
oversized paths/components, and malformed UTF-8 are rejected. Listing uses an
unsigned UTF-8 byte-order name cursor that must be one valid component; `/`,
`\`, controls, DEL, `.`, and `..` are not valid inside that cursor. Listing
also has a count limit of 1..16 and a 768-byte response budget. Nonempty
reads/writes use Binary Data Plane bytes; zero-length operations complete
synchronously without a transfer ID.

The RAM backend and esp-emu UART-TCP path are the completed File Transfer Base
foundation. Step 6A also verifies a persistent `StorageFatfs` backend under
the same neutral contract. Production `StorageSdmmc` Host-to-Device writes are
hardware-validated on P4-NANO physical SD at 1.5 Mbps with raw and bounded
`zero-rle-v1`; W=1 remains the default and W=2 bounded Go-Back-N is opt-in.
This scoped result does not cover every real-media lifecycle or TAB5.

## Step 6A persistent storage architecture

Step 6A is the completed, hardware-independent persistent storage integration
using emulator-supported SPI-NOR plus ESP-IDF FATFS/Wear Levelling (WL). It is
distinct from the physical microSD/SDMMC implementation validated separately
under Step 6B; Step 6B remains partial with removal and durability work
outstanding.

The common validation image uses the current approved 8 MiB flash envelope.
This is the common esp-emu validation envelope, not a claim that every future
board has only 8 MiB of flash or that it is the physical maximum of P4-NANO:

```text
nvs           [0x009000, 0x00F000)  size 0x006000
phy_init      [0x00F000, 0x010000)  size 0x001000
factory       [0x010000, 0x210000)  size 0x200000
np2test       [0x210000, 0x344000)  size 0x134000  type/subtype 0x40/0x01  read-only
storage       [0x344000, 0x800000)  size 0x4BC000
total common flash envelope: 0x800000 / 8 MiB
```

The factory application partition is now 2 MiB. It was expanded to provide
headroom for future production growth, especially OSD/UI and display-side
functionality, while preserving the common 8 MiB validation envelope. The
additional 1 MiB was taken from the internal FATFS test/storage partition;
the read-only `np2test` fixture size was deliberately preserved. This FATFS
partition is primarily a development and validation fixture, not the intended
final large user-media store. The production P4-NANO profile now uses
microSD/SDMMC for real user files; other boards and media remain separate
integration scopes.

The offsets and sizes originate from `firmware/partitions.csv`. Operational
tooling validates the generated ESP-IDF partition table through the shared
geometry helper rather than treating duplicated offsets as authoritative.

The measured storage fixture under this geometry is:

```text
storage partition  0x4BC000 / 4,964,352 bytes
WL sectors         1212
WL metadata        0xC000
plain FAT volume   0x4B0000
FAT type           FAT12
sector size        4096
sectors/cluster    1
cluster size       4096
usable data        1193 clusters
clean allocated    320 clusters
clean free         873 clusters
```

These are measured current test-fixture values, not product API guarantees.
Phase 6 freshly measured all relevant profiles below the generated 2 MiB
factory size; the largest was `storage-provider` at `0xFC2D0` (1,032,912
bytes), leaving `0x103D30` (1,064,240 bytes). This is evidence from the
current codebase, not a stable ABI limit or a promise that future firmware
will remain below 1 MiB.

The mounted namespace is:

```text
/persist
├── files          File Transfer visible namespace
├── fixtures       private NP2 fixture namespace
└── .np2-staging   private replacement/staging state
```

File Transfer logical `/` maps to `/persist/files`. The `fixtures` and
`.np2-staging` namespaces are therefore not exposed through File Transfer;
this is namespace isolation, not arbitrary filesystem access.

The storage dependencies are deliberately layered:

```text
File Transfer
    -> storage::Storage
    -> StorageFatfs
    -> ESP-IDF VFS/FATFS/WL
    -> SPI NOR

NP2 / FDD / XDF
    -> DOSIO
    -> generic POSIX/VFS backend
    -> mounted filesystem
```

`np2host`, `np2fixture`, and `np2test_runner` do not depend on
`storage_fatfs`. The Step 6A application composition owns the FATFS mount and
provides the generic VFS path to the runner. This keeps a future SDMMC FATFS
mount, or another mounted filesystem, out of NP2 host/core-specific logic.

The raw and VFS disk sources have distinct roles. The raw source is the
read-only `np2test` partition at logical `./np2test-fd1232.hdm`. The VFS source
is `/persist/fixtures/np2test-fd1232.hdm`, mapped through DOSIO to the same
logical NP2 path. Both use the same FDD/XDF, NP2 CPU, Stage-1 guest, and
result-v1 parser/controller. The raw source remains a permanent deterministic
independent oracle.

Step 6A File Transfer service capacity remains 2 MiB. Routine CI uses a
bounded 262145-byte upload/download workload, while the routine persistence
case uses 4097 bytes to cross the 4 KiB FAT cluster boundary. A clean image
using the approved 2 MiB-app / 8 MiB validation geometry accepts
a new 2 MiB file upload, readback, and ranged reads.
That protocol maximum does not guarantee same-size replacement of a full 2 MiB
file on the internal FATFS fixture: replacement temporarily needs both the
existing target and a staging file. The extended replacement/rollback path is
therefore validated with a 419-cluster (`0x1A3000`) payload and a 35-cluster
safety margin. This capacity distinction is not a firmware bug and does not
lower the 2 MiB protocol limit. These File Transfer writes do not make the NP2
guest disk writable: the NP2 disk path remains read-only through DOSIO.

The NoSpace contract is geometry-derived rather than a fixed 4 MiB prefill:
the 1 MiB request needs 256 clusters, the target is left with 255 free
clusters, and under the current approved geometry, the measured result derives
a 618-cluster
(`0x26A000` / 2,531,328-byte) prefill. Begin/preallocation fails with
`NO_SPACE` before any payload frame (`payload_frames=0`); the pre-existing file
remains intact, staging cleanup succeeds, and the endpoint recovers. The
`0x26A000` value is this measured result, not a permanent source-of-truth
constant.

Validated persistent behavior also includes metadata/stat, listing/pagination,
ranged reads, UTF-8, FAT case-insensitive collision handling, FAT-invalid
names, staged replacement, abort preservation, actual FAT NoSpace mapping,
cross-process persistence, and high-address physical flash persistence. The
high-address invariant is semantic: the marker must be at physical offset
`>= 0x400000` and inside the storage partition. Phase 6 observed `0x68D000` as
evidence, not as a permanent required address.

During Step 6A.1 development, `StorageFatfs` direct underlying POSIX
`pread`/`pwrite` operations of 4096 bytes were observed to hang under the
pinned esp-emu/IDF combination, while 512-byte operations passed. The current
512-byte bound is therefore an emulator-observed local compatibility
workaround. It is not a FATFS requirement, a physical ESP requirement, or a
proven exact failure threshold. Step 6A.3 separately established that generic
DOSIO VFS reads can perform 4096-byte POSIX reads successfully. A separate
formal P4-NANO physical-SD NP2TEST run validates the production DOSIO/VFS/SD
path; it does not turn the esp-emu-specific 512-byte observation into a
physical constraint.

The strongest Step 6A.4 source-independence proof used a temporary 8 MiB image
copy. Only the raw partition's first `0x1000` bytes at offset `0x210000` were
poisoned. Its SHA changed from
`3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3` to
`92ecf3e62e8ea67a2e618b58cf57e6a8db0f7a4ca5891507d53854285fe108f`, while
the FATFS storage region remained unchanged. The VFS-backed NP2 run still
completed 13 tests with 13 passed, 0 failed, and CRC `0x58f5b827`. This is
evidence of source independence, not a claim of arbitrary filesystem or guest
software compatibility.

The current provider split is:

```text
File Transfer -> storage::Storage -> StorageFatfs -> VFS/FATFS/WL -> SPI NOR
NP2 -> FDD/XDF -> DOSIO -> POSIX/VFS -> mounted FATFS -> SPI NOR

File Transfer -> storage::Storage -> StorageSdmmc -> VFS/FATFS -> SDMMC
NP2 -> FDD/XDF -> DOSIO -> POSIX/VFS -> mounted FATFS -> SDMMC
```

No FATFS-specific logic is introduced into NP2 core/host boundaries.

Machine-readable JSON and binary frames use the raw UART driver path so
stdout/VFS newline conversion cannot change arbitrary binary bytes. Normal
human-readable logging remains on stdout/VFS. The common machine writer holds
the stdout FILE lock while flushing pending stdout data and issuing the raw
UART write, preventing interleaving with normal stdout writers that participate
in the same FILE locking discipline:

```text
machine JSON/binary frame
        |
        v
flockfile(stdout)
        |
        v
fflush(stdout)
        |
        v
raw uart_write_bytes()
        |
        v
funlockfile(stdout)
```

This is not an absolute serialization guarantee against direct UART writes
outside the common machine writer, ROM output, panic/constrained-environment
output, or other paths that bypass normal stdout FILE locking. Future
project-owned machine-readable output should use the common machine writer
rather than introducing an independent raw UART path.
