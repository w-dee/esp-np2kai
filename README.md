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
general software support. NP2 source itself has not been imported.

Step 5 firmware/runtime integration is implemented and validated under a
non-formal ESP32-P4 `esp-emu` profile. The dedicated FreeRTOS NP2 runner now
drives the validated 124-translation-unit NP2core through the np2host boundary,
the read-only raw-NOR NP2TEST fixture, and the shared Stage-1 parser/controller.
The formal firmware profile remains `EXTMEM=13`; the reduced emulator profile
uses explicit `EXTMEM=8` only as supplementary evidence. Formal ESP32-P4
runtime validation remains blocked because esp-emu v0.39.0 exposes 16 MiB
PSRAM but cannot provide the required contiguous external block after the
NP2core external BSS placement.

Step 6A is complete: the hardware-independent persistent storage integration
uses ESP-IDF VFS/FATFS/WL on an emulator-supported SPI-NOR partition. The
FATFS-backed File Transfer service, generic DOSIO VFS path, and VFS-backed NP2
Stage-1 run are validated under esp-emu. The reduced non-formal runtime
profile reaches 13/13 with CRC `0x58f5b827`, and a raw-partition poisoning test
confirmed that the VFS run used the independent FATFS fixture source. The
P4-NANO production SDMMC backend and Host-to-Device File Transfer path are
also hardware-validated at 1.5 Mbps for bounded `zero-rle-v1` with the default
W=1 transport and opt-in W=2 bounded Go-Back-N. This File Transfer result is
separate from formal NP2TEST execution. Broader physical-media removal,
durability, and device-to-host qualification remain future work; formal
`EXTMEM=13` is tracked separately.

The current approved ESP32-P4 validation image uses an 8 MiB flash envelope:
the 2 MiB factory application partition is at `0x010000`, read-only `np2test`
is at `0x210000` with size `0x134000`, and FATFS/WL storage is at `0x344000`
with size `0x4BC000`, ending at `0x800000`. Phase 6 validated all relevant
firmware profiles, video oracles, and presentation against the generated
geometry; the largest measured profile was `storage-provider` at
`0xFC2D0`, leaving `0x103D30` of factory headroom. This is measured current
evidence, not a permanent firmware-size limit. The validated P4-NANO hardware
reported a 16 MiB physical flash device; the project intentionally retains the
existing 8 MiB validation envelope.

### Step 7A: headless guest framebuffer and video oracles — COMPLETE

Step 7A provides an implemented and verified headless RGB565LE guest
framebuffer boundary. The guest framebuffer has dynamic geometry and the ESP32-P4
implementation stores it in external PSRAM. The tested guest geometry is
640x400; the opt-in Step 7B.2a P4-NANO production scanout foundation is now
IMPLEMENTED, BUILD VALIDATED, REAL-HARDWARE UART VALIDATED, and REAL-LCD
VISUALLY VALIDATED for its bounded native diagnostic scope. It does not turn
the headless guest framebuffer into a live display consumer.

Three deterministic video oracles are approved and continuously checked:

- scene 1: deterministic text renderer, CRC `0x0a280896`;
- scene 2: deterministic direct graphics-VRAM scene, CRC `0x4fa8c690`; and
- scene 3: actual slave-GDC drawing-command VECTL scene, CRC `0x10ea77dd`.

All three have Ubuntu-native framebuffer goldens and are validated under
ESP32-P4 RISC-V `esp-emu` v0.39.0 and GitHub Actions. These oracles validate
the tested rendering paths only; they are not a claim of complete PC-98
graphics support or complete uPD7220/GDC behavior.

### Step 7B.1: portable presentation boundary — COMPLETE

Step 7B.1 safely hands renderer output to a future asynchronous platform
consumer:

```text
one mutable NP2 guest RGB565 framebuffer
        -> synchronous publication copy at scrnmng unlock
portable two-slot presentation publisher
        -> immutable ACQUIRED presentation frame
future platform/board display consumer (the Step 7B.2a native foundation is a
diagnostic path only)
```

The publisher has one producer and one consumer, uses `FREE`, `WRITING`,
`PENDING`, and `ACQUIRED` ownership states, and applies latest-frame-wins
coalescing. It never overwrites an `ACQUIRED` frame, waits for the consumer,
allocates per frame, or exposes the mutable guest framebuffer pointer to the
consumer. Detailed ownership, ordering, and resize rules are documented in
[`docs/architecture.md`](docs/architecture.md).

Step 7B.1a is the Ubuntu portable publisher contract. Step 7B.1b validates
the same contract on ESP32-P4 / FreeRTOS / `esp-emu`, including a guest
framebuffer and exactly two external-PSRAM presentation slots in the tested
640x400 case, lock-free 32-bit slot-state atomics in that toolchain, an
independent consumer, immutable acquired frames, coalescing, and resize /
generation lifetime. Step 7B.1c wires those checks into continuous CI. These
are current Ubuntu x86_64 and ESP32-P4 RISC-V / `esp-emu` results only; the
slot size and memory telemetry are test evidence, not universal constants.

## Current hardware boundary

The software-only headless video and presentation work through Step 7B.1c is
complete. The Step 7B.2a production path was run on the Waveshare
ESP32-P4-NANO-KIT-D with ESP32-P4 rev v1.3, 32 MiB PSRAM, and the JD9365
10.1-inch panel. Its native 800x1280 RGB565 scanout, one-framebuffer policy,
safe initialization, deterministic pattern, UART stages, and bounded
backlight lifecycle all passed; human inspection separately confirmed the
physical LCD output. This is scoped Step 7B.2a evidence, not blanket physical
display completion.

Step 7B.2b is COMPLETE for its bounded pixel-exact reference and static
physical-transform validation scope. The project-owned C++20 transform consumes
an immutable 640x400 RGB565 frame, applies exact nearest-neighbor 2x to a
logical 1280x800 landscape, and writes directly to one native 800x1280 RGB565
destination. It has no intermediate framebuffer, filtering, color conversion,
PPA, DMA2D, or per-frame allocation. Both quarter-turn mappings remain in the
API; the P4-NANO board policy is COUNTERCLOCKWISE, selected by human inspection
as the natural upright orientation. Host validation freezes reference CRCs
`0xdb938d53` (CW) and `0x164584cf` (CCW), while the separate physical diagnostic
source is `0x4291f7e5`, with transformed CRCs `0x37fd7262` (CW) and
`0xd98ce5d4` (CCW). Both candidates built and ran on the qualified P4-NANO;
both were visually inspected successfully. The CCW capture began after early
boot, so only its terminal runtime/cleanup result was retained.

Step 7B.2c is COMPLETE for its bounded first live integration:
**IMPLEMENTED / END-TO-END BYTE-EXACT VALIDATED / REAL-HARDWARE LIVE
PRESENTATION VALIDATED / REAL-LCD VISUALLY VALIDATED / COMPLETE**. The existing
Step 7A `np2video_runner` rendered the `NP2 VIDEO FIXTURE 7A.3A` text scene
(source CRC `0x0a280896`) through the real NP2/scrnmng path. The resulting
pipeline is:

```text
mutable NP2 framebuffer
 -> synchronous scrnmng publication hook
 -> two-slot presentation publisher
 -> immutable acquired 640x400 RGB565 frame
 -> P4-NANO live consumer
 -> exact 2x + canonical CCW transform
 -> one native 800x1280 RGB565 framebuffer
 -> physical JD9365 LCD
```

The two PSRAM presentation slots are 512,000 bytes each (1,024,000 bytes
total), external and disjoint from both the mutable guest framebuffer and the
native framebuffer. The final source CRC matched `0x0a280896`; the host-derived
and real native framebuffer CRC both matched `0xe623a22a`. The bounded hardware
counters were submitted/acquired/transformed/released `1/1/1/1`, with
coalesced/dropped `0/0`. The consumer held the acquired token through source
validation and transformation, and the runtime completed with
`P4_NANO_LIVE_RESULT=PASS` and cleanup/backlight OFF.

The operator observed the actual NP2 text scene on the physical LCD for
approximately 30 seconds and reported `HUMAN_VISUAL_RESULT=PASS`: natural
upright landscape content, expected full mapping, and no gross clipping,
mirroring, reversal, or static corruption. Because this selected fixture is
essentially monochrome, Step 7B.2c does not independently re-demonstrate live
NP2 color ordering; the same color-capable transform/display path has separate
Step 7B.2b physical RGB-order evidence. The saved UART transcript began late,
so its early immutability marker and startup lines are not retained; this is an
evidence-capture limitation, not a runtime failure.

This does not claim a general production emulator display pipeline. Sustained
multi-frame cadence, buffering/reuse, tearing, measured refresh, latency,
long-duration stability, live-load PSRAM bandwidth, PPA, and display concurrency
with SDMMC/audio remain future work. The next display milestone is Step 7B.2d:
sustained live presentation cadence and transform performance. The P4-NANO CCW
policy does not generalize to TAB5, ESP32-S31, or ESP32-S3 without an
independent geometry/orientation decision.
Physical-media removal/durability, input, audio, and TAB5 integration remain
future hardware-dependent work.

Production firmware now has explicit `p4-v1x` and `p4-v3x` build variants.
Use `tools/emu/build-production.sh --variant p4-v3x` for the existing
esp-emu v0.39.0 regressions and `--variant p4-v1x` for the compile-only v1.x
production check. Revision selection is kept out of common defaults, and the
variant-specific build trees and preflight checks prevent the two image
families from being confused. The P4-NANO board profile has scoped production
display-foundation, UART/SDMMC, and File Transfer hardware validation; that
does not imply complete validation of every production-firmware subsystem.

The currently verified executable milestone is ESP32-P4-only. S31 Korvo-1 is
not implemented, tested, or validated.

The UART Control Plane Base is verified under `esp-emu` v0.39.0 and on the
P4-NANO onboard CH343P path. It provides bounded `@ESP-NP2 ` JSON-lines
framing, the separate `ESP-NP2KAI UART CONTROL READY` marker, and the initial
read-only commands `protocol.hello`, `system.ping`, and `system.info`.
The TAB5 UART path remains unverified.

The Binary Data Plane v1 is verified under the ESP32-P4 `esp-emu` UART-TCP
environment. Its integration test transfers deterministic 64 KiB payloads in
both directions and checks CRC, duplicate handling, NACK retransmission,
corrupted-frame recovery, and text/binary resynchronization. This verifies the
full bidirectional emulator suite. Separately, the production Host-to-Device
data path used by File Transfer is hardware-validated on P4-NANO at 1.5 Mbps;
that scoped result is not a full physical bidirectional test-suite claim, and
TAB5 remains unverified.

The File Transfer Base is verified over the same UART-TCP path. It adds a
neutral streaming storage interface, a bounded 256 KiB RAM backend, logical
UTF-8 paths, paginated metadata, ranged reads, and staged complete-file writes.
Its 131,109-byte round-trip regression covers final-ACK replay, abort-safe
replacement, zero-length files, and path/error bounds. This paragraph records
the completed RAM-backed foundation; the persistent FATFS backend is the
separate Step 6A result. Production physical-SD Host-to-Device transfer is
additionally hardware-validated on P4-NANO at 1.5 Mbps with bounded
`zero-rle-v1`, W=1, and opt-in W=2. W=1 remains the production default.

## Development model

The emulator core remains portable and separate from Espressif-common,
SoC-specific, and board-specific code. Current development progresses from
Ubuntu-native core/video/presentation validation, through ESP32-P4
`esp-emu` firmware/video/presentation validation, to real ESP32-P4 board
validation. The P4-NANO, TAB5, and future ESP32-S31 targets remain distinct
platform or board scopes. The firmware remains capable of headless operation
for core and integration tests.

## Initial toolchain baseline

- Host OS: Ubuntu 24.04
- Current ESP-IDF baseline: v5.5.4
- Current esp-emu baseline: v0.39.0
- Current ESP target: `esp32p4`
- New platform-side C++: C++20

The project does not use a PlatformIO-installed ESP-IDF environment.

New firmware C++ is explicitly compiled as GNU C++20. C++ exceptions and RTTI
are disabled, and the firmware does not use `iostream`.

## Representative current validation entry points

The following commands cover the current headless video and presentation
contracts; they are representative entry points rather than an exhaustive
script catalog.

Ubuntu-native checks:

- [`tools/emu/test-step6a-ci.sh`](tools/emu/test-step6a-ci.sh) covers the
  pinned Step 6A emulator storage regression.
- `make -C host test-presentation-contract` checks the portable presentation
  publisher and the synchronous headless scrnmng publication contract.
- `make -C host test-video-runner-golden` checks the text framebuffer golden.
- `make -C host test-video-gfx-vram-golden` checks the direct-VRAM golden.
- `make -C host test-video-gdc-golden` checks the GDC drawing-command golden.
- `make -C host test-video-golden-checker` checks golden descriptor/log
  support.

ESP32-P4 presentation checks use the pinned ESP-IDF v5.5.4 and esp-emu
v0.39.0 environment:

```sh
source <pinned-ESP-IDF-v5.5.4>/export.sh
export ESP_EMU=<pinned-esp-emu-v0.39.0>
bash tools/emu/test-np2presentation-profile.sh
python3 tools/emu/test_validate_np2presentation_log.py
bash tools/emu/test-np2presentation.sh
```

ESP32-P4 video checks select and build each oracle independently:

```sh
bash tools/emu/test-np2video-golden.sh
bash tools/emu/test-np2video-golden.sh --fixture gfx-vram
bash tools/emu/test-np2video-golden.sh --fixture gdc
```

The earlier UART and file-transfer entry points retain their separate scopes:

- [`tools/emu/test-hello-world.sh`](tools/emu/test-hello-world.sh) verifies
  basic ESP-IDF build, merge, boot, and the Hello World marker.
- [`tools/emu/test-uart-control-plane.sh`](tools/emu/test-uart-control-plane.sh)
  runs the Hello World regression and verifies the bounded JSON control path.
- [`tools/emu/test-uart-binary-data-plane.sh`](tools/emu/test-uart-binary-data-plane.sh)
  runs the Control Plane regression and verifies bidirectional binary transport
  over esp-emu UART-TCP.
- [`tools/emu/test-file-transfer-base.sh`](tools/emu/test-file-transfer-base.sh)
  preserves the earlier regressions and verifies the RAM-backed file service.

These ESP32-P4 checks are bound to ESP-IDF v5.5.4 and esp-emu v0.39.0 for
this milestone.

## Validation stages

Validation is organized in three layers:

1. **Ubuntu-native** — fastest portable-core, framebuffer, and presentation
   reference validation. The bounded Step 4 NP2kai execution and Step 7A /
   Step 7B.1a contracts belong to this layer.
2. **Espressif `esp-emu`** — ESP32-P4 firmware and integration validation. The
   current Hello World, UART/data/file checks, raw fixture check, reduced
   Stage-1 check, Step 7A video oracles, and Step 7B.1b presentation contract
   are results for this layer. The reduced Stage-1 result is explicitly
   non-formal; formal `EXTMEM=13` runtime validation remains blocked by the
   current emulator memory model.
3. **Real ESP32-P4 hardware** — unsupported peripherals, real timing,
   performance, board transport, and physical display/storage validation.

Ubuntu-native success is not a substitute for `esp-emu` or hardware validation.
The verified `esp-emu` path is specifically an ESP32-P4 result, and its
performance is not representative of real target hardware.

## Documentation

- [Architecture](docs/architecture.md)
- [Development environment](docs/development-environment.md)
- [Bring-up plan](docs/bringup-plan.md)
- [Licensing and provenance](docs/licensing.md)
- [Vendored NP2kai snapshot](third_party/np2kai/README.md)
