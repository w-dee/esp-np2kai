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
results; physical P4-NANO-KIT-D, CH343P, and TAB5 UART paths remain unverified.

The entry point prints the stable UART marker:

```text
ESP-NP2KAI HELLO WORLD OK
```

The project target is `esp32p4`. New firmware C++ is compiled as GNU C++20;
exceptions and RTTI are disabled. The `sdkconfig.defaults` file contains the
small set of project-owned defaults, while generated `sdkconfig` remains a
local file.

The esp-emu v0.39.0 test environment reports ESP32-P4 revision v3.1, so the
defaults select `CONFIG_ESP32P4_REV_MIN_301=y`. This requirement is verified
for the emulator environment only; physical P4-NANO and TAB5 revision
compatibility remains unverified.

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
esp-emu; physical P4-NANO and TAB5 UART paths remain unverified.

The Hello World build-and-emulation check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
build the firmware, create a merged image, boot it under esp-emu, detect
`ESP-NP2KAI HELLO WORLD OK`, and preserve combined emulator/UART output in
`firmware/build/esp-emu-hello-world.log`. It will not silently change the
configured target.

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
