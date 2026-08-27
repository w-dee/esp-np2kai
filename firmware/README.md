# Firmware

This directory contains the current minimal ESP32-P4 firmware application. It
is a headless Hello World, UART Control Plane, Binary Data Plane, and
RAM-backed File Transfer Base target by default. Explicit production profiles
also compose persistent FATFS and P4-NANO SDMMC storage without moving board
policy into the portable File Transfer layer. The project components also
include `storage`, `storage_ram`, and `file_transfer`. The normal `main`
component and ESP-IDF-provided
dependencies are also part of the firmware project. No external third-party
protocol dependency has been added. The Hello World, JSON UART Control Plane,
Binary Data Plane, and File Transfer Base paths are verified under esp-emu
v0.39.0 with ESP-IDF v5.5.4.

The Binary Data Plane v1 integration test verifies deterministic 64 KiB
transfers in both directions, per-frame and whole-transfer CRC, duplicate DATA
idempotency, host-generated NACK retransmission, corrupted-frame recovery, and
text/binary stream resynchronization over esp-emu UART-TCP. These are emulator
results for the data-plane integration. Separately, the P4-NANO-KIT-D CH343P
UART Control Plane path is verified on real hardware; the TAB5 UART path is not
yet verified.

The entry point prints the stable UART marker:

```text
ESP-NP2KAI HELLO WORLD OK
```

The project target is `esp32p4`. New firmware C++ is compiled as GNU C++20;
exceptions and RTTI are disabled. The `sdkconfig.defaults` file contains the
small set of project-owned defaults, while generated `sdkconfig` remains a
local file.

Production firmware builds must select a silicon-family variant explicitly:

```bash
tools/emu/build-production.sh --variant p4-v3x
tools/emu/build-production.sh --variant p4-v1x
tools/emu/build-production.sh --variant p4-v1x --board p4-nano
```

The Step 7B.2a P4-NANO display foundation is an explicit, compile-time opt-in
profile:

```bash
tools/emu/build-production.sh --variant p4-v1x --board p4-nano --display-foundation
```

It is bounded to P4 revision 1.x and the P4-NANO board. The path owns the
shared GPIO7/GPIO8 I2C service, safe JD9365 panel/power sequencing, one native
800x1280 RGB565 MIPI-DSI/DPI framebuffer, cache priming, and a deterministic
static geometry/color pattern. Step 7B.2a is now IMPLEMENTED, BUILD VALIDATED,
REAL-HARDWARE UART VALIDATED, and REAL-LCD VISUALLY VALIDATED on the
Waveshare ESP32-P4-NANO-KIT-D (ESP32-P4 rev v1.3, 32 MiB PSRAM, JD9365 ID
`93 65 04`). It remains a bounded diagnostic path and does not consume the
live NP2 presentation publisher.

The physical device reported 16 MiB flash; the project intentionally retains
the existing 8 MiB validation envelope. The production run used one
2,048,000-byte framebuffer, matched CRC32 `0x5383260a` against the host golden,
and completed the safe backlight lifecycle (`0x00` -> `0x40` -> OFF). Human
inspection confirmed the native portrait pattern twice. This does not validate
the later live emulator display pipeline.

### Historical Step 7B.2b: CPU/scalar transform reference

Step 7B.2b is COMPLETE for the bounded transform-reference and static physical
diagnostic scope. Build either diagnostic candidate with:

```bash
tools/emu/build-production.sh --variant p4-v1x --board p4-nano \
  --display-transform-diagnostic --rotation cw
tools/emu/build-production.sh --variant p4-v1x --board p4-nano \
  --display-transform-diagnostic --rotation ccw
```

The fused C++20 reference maps an immutable `640x400 RGB565` source through
exact nearest-neighbor 2x and a logical `1280x800` landscape directly into one
`800x1280 RGB565` native framebuffer. Every source pixel becomes four identical
destination pixels. There is no `1280x800` intermediate, filtering, color
conversion, PPA, DMA2D, live NP2 consumer, or per-frame transform allocation.
The API retains both CW and CCW mappings; COUNTERCLOCKWISE is the canonical
P4-NANO policy selected by human inspection as the natural upright orientation.

Host reference CRCs are CW `0xdb938d53` and CCW `0x164584cf`. The separate
physical diagnostic source CRC is `0x4291f7e5`, with transformed CRCs CW
`0x37fd7262` and CCW `0xd98ce5d4`. The two physical candidates built and ran
on the qualified ESP32-P4-NANO-KIT-D and passed human visual inspection. The
CCW UART capture began after early boot; its terminal runtime/cleanup result
was retained, but not a complete boot-to-cleanup transcript.

### Historical Step 7B.2c: first live NP2-to-LCD integration

Step 7B.2c is COMPLETE for the bounded first live NP2-to-LCD integration. Build
it with:

```bash
tools/emu/build-production.sh --variant p4-v1x --board p4-nano --live-display
```

The existing Step 7A `np2video_runner` rendered the real
`NP2 VIDEO FIXTURE 7A.3A` text scene through the synchronous scrnmng
publication hook and the Step 7B.1 two-slot publisher. The P4-NANO consumer
acquired only immutable `640x400 RGB565` frames, held the matching token through
the exact fused 2x + canonical COUNTERCLOCKWISE transform, synchronized one
native `800x1280 RGB565` framebuffer, and then released the token. Exactly two
512,000-byte external-PSRAM presentation slots were used; neither aliases the
mutable guest framebuffer or native DSI framebuffer. No `1280x800` intermediate,
second native framebuffer, PPA, DMA2D, or SIMD path is involved.

The selected source CRC was `0x0a280896`; both the host-derived and real native
destination CRCs were `0xe623a22a`. The bounded hardware counters were
submitted/acquired/transformed/released `1/1/1/1`, coalesced/dropped `0/0`, and
the runtime completed with `P4_NANO_LIVE_RESULT=PASS` and cleanup/backlight OFF.
The human operator saw the actual NP2 text scene on the physical LCD for
approximately 30 seconds and reported `HUMAN_VISUAL_RESULT=PASS` with natural
upright landscape mapping and no gross clipping, mirroring, reversal, or static
corruption.

The monochrome text fixture does not independently prove live-NP2 color
ordering; that physical path has prior color-diagnostic evidence in Step 7B.2b.
The saved UART capture began late and therefore does not retain the early
immutability marker or all startup lines, although the bounded run completed
with the immutability condition satisfied, correct CRCs/counters, PASS result,
and cleanup. This is bounded validation evidence, not a general production
display-pipeline claim. Sustained cadence, buffering/reuse, tearing, measured
refresh, latency, and long-duration stability remain bounded-scope limitations.
The later P10M profile supplies the qualified PPA -> horizontal PIE -> DMA2D
exact2x path and formal correctness/performance evidence; broader display plus
SDMMC/audio runtime coexistence remains future work.

### Historical Step 7B.2d: sustained scalar/motion validation

Step 7B.2d is now complete for the reviewed P4-NANO path. The physical
automated motion profile `--live-display-motion-validation` ran on ESP32-P4
v1.3 with the unchanged `np2video-7b2d-live-vram` fixture (SHA256
`81975ad74c7b1769a5aa63977ee9c18b020d6381e858522cb4cb7c7861f85604`) using the
production-default `-O2` CCW 2x transform and emitted
`MOTION_VALIDATION_RESULT=PASS`. It acquired 60 frames, accepted 16 clean and
16 distinct positions, passed all 16 native mappings, dropped 0 frames, and
returned from `app_main()` without watchdog, panic, or reset failure. The 44
transitional acquisitions were skipped as expected asynchronous guest-plane
states, not interpreted as a performance or displayed-frame ratio. This proves
software-visible motion through the synchronized native framebuffer, not
physical scanout of every frame; refresh/tearing, DSI/GDMA temporal behavior,
and panel timing remain future characterization.

## Current qualified P10M display path

The current P4-NANO performance/correctness profile uses the documented
PPA -> horizontal PIE -> DMA2D exact2x pipeline and LOWER2 scanout. P10M-C1C
is formal real-hardware byte-exact correctness VALID/PASS; P10M-D2C is formal
same-binary performance VALID/A-PROVISIONAL. See
[`docs/architecture.md`](../docs/architecture.md) and
[`docs/bringup-plan.md`](../docs/bringup-plan.md) for the current path and
bounded evidence.

The unqualified production profiles use a conservative 115200 baud. The
explicit `p4-nano` board overlay selects 1500000 only for the Waveshare
ESP32-P4-NANO-KIT-D onboard CH343P path validated in the development
environment; it is not a portable default for other P4 or CH343 boards.

Both variants use the same application, partition geometry, 8 MiB validation
envelope, and production PSRAM settings. The revision selector is kept in the
variant overlay rather than in the common defaults. `p4-v3x` is the variant
used by esp-emu v0.39.0, which reports ESP32-P4 revision v3.1. Generic
`p4-v1x` remains compile evidence in CI, while the explicit P4-NANO board
profile has scoped production hardware evidence for display, UART, SDMMC, and
Host-to-Device File Transfer at 1.5 Mbps. The wrapper
names the build directory and checks the generated revision bounds before
emulator or image use. The two binaries must not be treated as interchangeable.

The first production v1.x hardware smoke image is the three-image set from
`firmware/build-p4-v1x`: `bootloader/bootloader.bin`,
`partition_table/partition-table.bin`, and `esp_np2kai.bin`. It intentionally
does not include the raw `np2test` fixture. Without that fixture, the expected
headless log still includes `ESP-NP2KAI HELLO WORLD OK`, PSRAM/memory-probe
evidence, and `ESP-NP2KAI UART CONTROL READY`; the formal runner is expected to
report `NP2TEST_RUNNER_BLOCKED ... reason=fixture_unavailable`. A future real
hardware run must determine `NP2MEM_RESULT=PASS` and stability separately; no
production v1.x runtime result is claimed here.

## Step 5 headless core integration

The Step 5 runtime path is now implemented as a real firmware dependency
chain:

```text
main
  +-- uart_control_transport
  +-- np2memoryprobe
  +-- np2fixtureprobe
  +-- np2test_runner
          +-- np2core
          +-- np2host
          +-- np2fixture
          +-- shared Stage-1 configuration/parser/controller
```

`np2test_runner` owns the one-shot FreeRTOS task and directly drives
`pccore_init()`, `pccore_reset()`, `pccore_exec()`, and `pccore_term()`. The
np2core external BSS is approximately 5.69 MiB (`.ext_ram.bss=0x5af57c` in
the final map), including the 2 MiB `mem[]` buffer. The formal profile remains
`EXTMEM=13`.

The raw fixture is a dedicated read-only NOR partition containing the exact
FD1232 Stage-1 image. Runtime validation checks its partition metadata,
SHA-256, mmap/read-only DOSIO access, and vendor FDD/XDF recognition. This
deliberately avoids a FAT layer in this esp-emu oracle. Separately, P4-NANO
physical SD, writable Host-to-Device File Transfer, and formal physical-SD
NP2TEST have scoped hardware validation; broader media lifecycle work remains.

The formal Ubuntu-native result is 13/13 with CRC `0x58f5b827` and
`NP2TEST_RESULT=PASS`. The ESP32-P4 formal profile is currently blocked on
esp-emu v0.39.0's 16 MiB PSRAM model and its inability to provide the required
contiguous external allocation. The reduced profile uses explicit `EXTMEM=8`
as NON-FORMAL supplementary evidence only; it reaches 13/13, CRC
`0x58f5b827`, and `NP2REDUCED_RESULT=PASS`, and does not replace formal
`EXTMEM=13` validation.

The CI workflow keeps three independent jobs: formal fixture CI,
formal Ubuntu-native headless CI, and the NON-FORMAL ESP32-P4 esp-emu reduced
Stage-1 job. Those jobs do not claim hardware validation; separate P4-NANO
qualification covers the scoped paths documented below.

## D1 production-SD formal NP2TEST profile

The explicit `NP2_SD_NP2TEST_PROFILE` composition mounts the already accepted
physical SD fixture through the existing SDMMC slot-1 provider, FATFS/VFS, and
read-only DOSIO/FDD path. It preserves formal `EXTMEM=13` and is built with:

```bash
NP2_SD_NP2TEST_PROFILE=1 \
  tools/emu/build-production.sh --variant p4-v1x --board p4-nano
```

The fixed physical fixture path is:

```text
/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm
```

This diagnostic image starts the UART Control Plane without registering the SD
Storage API with File Transfer. D2 must first use the accepted normal
`UART_SDMMC_PROFILE` image for read-only `file.stat` and `file.sha256` checks,
then boot the dedicated image, which repeats the exact VFS size/SHA check before
FDD attachment. D1 performs no physical hardware validation, SD write, or
flash operation.

The observation-only D2 result harness is
[`tools/np2test_physical.py`](../tools/np2test_physical.py). It requires an
explicit serial port and baud, accepts only formal `13/13` with CRC
`0x58f5b827`, and never sends UART or File Transfer bytes.

This firmware is the current ESP32-P4-specific firmware baseline. The current
firmware tree and configuration do not build for ESP32-S31; future S31 work
would require a separate SoC implementation below the portable emulator and
host interfaces.

The UART Control Plane Base uses the configured ESP-IDF console UART without
changing its number, pins, or baud rate. It provides bounded `@ESP-NP2 `
JSON-lines framing and the separate readiness marker
`ESP-NP2KAI UART CONTROL READY`. The initial read-only commands are
`protocol.hello`, `system.ping`, and `system.info`. This path is verified under
esp-emu and on real P4-NANO-KIT-D hardware; the TAB5 UART path is not yet
verified.

The byte-stream transport reserves four consecutive NUL bytes as the literal
`TRANSPORT_SYNC` token:

```text
00 00 00 00
```

`control_stream` recognizes this token below the JSON/control framing layer.
It discards partial text and binary framing state, resets the byte-stream
parser to Text, and then accepts a normal text frame. Longer consecutive NUL
runs are treated as the same synchronization interval. The token does not
erase storage, reset the MCU, change UART configuration, start or abort a
transfer, or otherwise recover higher-level binary/file-transfer sessions.
Those session semantics remain a separate concern.

A host connection establishes or re-establishes synchronization by sending
`TRANSPORT_SYNC` followed by a uniquely identified `protocol.hello` and
validating the matching response. `ESP-NP2KAI UART CONTROL READY` is a
diagnostic marker only; a host may attach after boot and does not need to have
captured it. Startup UART garbage is tolerated by the transport protocol.

The reusable physical-UART smoke tool is
[`tools/uart_control_smoke.py`](../tools/uart_control_smoke.py). It does not
flush the UART input as a correctness requirement and ignores non-protocol
console output while waiting for matching responses.

The earlier real-hardware P4-NANO investigation recorded one startup `0xff`
in the UART0 receive path before driver installation. That is a measured
condition of this particular board, not a generic ESP32-P4 property. The
transport regression keeps it covered by the bounded garbage test.

The host/unit coverage for the detector and the real `control_stream.cpp`
implementation is
[`tools/test-control-stream-sync.sh`](../tools/test-control-stream-sync.sh).

The Hello World build-and-emulation check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
build the firmware, create a merged image, boot it under esp-emu, detect
`ESP-NP2KAI HELLO WORLD OK`, and preserve combined emulator/UART output in
`firmware/build-p4-v3x/esp-emu-hello-world.log`. It uses the explicit `p4-v3x`
production build path and will not silently change the configured target or
silicon family.

The verified UART Control Plane integration check is
[`tools/emu/test-uart-control-plane.sh`](../tools/emu/test-uart-control-plane.sh).
It runs the Hello World regression, starts a second esp-emu instance using the
merged image, waits for the control readiness marker, injects malformed JSON
and the three initial read-only requests, and validates the responses and
parser recovery.

The verified Binary Data Plane integration check is
[`tools/emu/test-uart-binary-data-plane.sh`](../tools/emu/test-uart-binary-data-plane.sh).
It runs the Control Plane regression first, then verifies the bounded COBS/CRC
transport and deterministic 64 KiB test endpoints over esp-emu UART-TCP.

The verified File Transfer Base check is
[`tools/emu/test-file-transfer-base.sh`](../tools/emu/test-file-transfer-base.sh).
It adds `file.stat`, `file.list`, `file.read.begin`, `file.write.begin`, and
`file.transfer.status`, backed by a bounded in-memory hierarchy. The test
performs a 131,109-byte upload/readback and verifies pagination, ranged reads,
final-ACK replay, staged replacement/abort, zero-length files, and path bounds.
That specific base regression uses test RAM only. Persistent FATFS has separate
esp-emu coverage, and production P4-NANO physical-SD Host-to-Device transfers
are hardware-validated at 1.5 Mbps for bounded `zero-rle-v1` with default W=1
and opt-in W=2. This does not make the RAM-base test a hardware test or qualify
TAB5 and all media lifecycles.

The eventual firmware should keep board-specific code outside the emulator
core and retain a headless mode for emulator-core and integration tests.
