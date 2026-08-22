# Firmware

This directory contains the current minimal ESP32-P4 firmware application. It
is a headless Hello World, UART Control Plane, Binary Data Plane, and
RAM-backed File Transfer Base target with no board-specific code or
peripherals. The project components also include `storage`, `storage_ram`, and
`file_transfer`. The normal `main` component and ESP-IDF-provided
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

The unqualified production profiles use a conservative 115200 baud. The
explicit `p4-nano` board overlay selects 1500000 only for the Waveshare
ESP32-P4-NANO-KIT-D onboard CH343P path validated in the development
environment; it is not a portable default for other P4 or CH343 boards.

Both variants use the same application, partition geometry, 8 MiB validation
envelope, and production PSRAM settings. The revision selector is kept in the
variant overlay rather than in the common defaults. `p4-v3x` is the variant
used by esp-emu v0.39.0, which reports ESP32-P4 revision v3.1. `p4-v1x` is
compile-only production evidence at this stage; the separate P4-NANO
diagnostics are not a production-firmware validation result. The wrapper
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
deliberately avoids a FAT layer under esp-emu; physical microSD/FATFS and
writable media remain future storage work.

The formal Ubuntu-native result is 13/13 with CRC `0x58f5b827` and
`NP2TEST_RESULT=PASS`. The ESP32-P4 formal profile is currently blocked on
esp-emu v0.39.0's 16 MiB PSRAM model and its inability to provide the required
contiguous external allocation. The reduced profile uses explicit `EXTMEM=8`
as NON-FORMAL supplementary evidence only; it reaches 13/13, CRC
`0x58f5b827`, and `NP2REDUCED_RESULT=PASS`, and does not replace formal
`EXTMEM=13` validation.

The CI workflow keeps three independent jobs: formal fixture CI,
formal Ubuntu-native headless CI, and the NON-FORMAL ESP32-P4 esp-emu reduced
Stage-1 job. No real ESP32-P4 hardware validation is claimed.

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
The backend is test RAM only; microSD, FATFS, and physical UART remain future
work.

The eventual firmware should keep board-specific code outside the emulator
core and retain a headless mode for emulator-core and integration tests.
