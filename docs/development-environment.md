# Development Environment

## Baseline

The initial project baseline is:

| Item | Version or target |
| --- | --- |
| Host OS | Ubuntu 24.04 |
| Current ESP-IDF baseline | v5.5.4 |
| Current esp-emu baseline | v0.39.0 |
| Current ESP target | `esp32p4` |
| New platform-side C++ | C++20 |

The versions are recorded in [`tools/versions.env`](../tools/versions.env).
That file is informational and must not modify the user's shell automatically.

## Environment boundaries

The project must not use or depend on a PlatformIO-installed ESP-IDF
environment. The ESP-IDF and `esp-emu` setup should be reproducible and
explicitly selected by the developer or CI environment. This initial setup
does not install system packages, ESP-IDF, `esp-emu`, or other external
software.

The minimal headless Hello World application exists under `firmware/` and has
been built and executed successfully under ESP-IDF v5.5.4 and esp-emu v0.39.0.
No component dependencies have been added.

The firmware targets `esp32p4` through `firmware/sdkconfig.defaults`. New
firmware C++ is explicitly compiled as GNU C++20 in the `main` component.
C++ exceptions and RTTI are disabled through ESP-IDF configuration, and no
`iostream` is used.

The `esp-emu` v0.39.0 test environment reports ESP32-P4 revision v3.1. The
current defaults therefore select `CONFIG_ESP32P4_REV_MIN_301=y`. This setting
is verified for the emulator environment; physical P4-NANO and TAB5 revision
compatibility remains unverified and must be reviewed during physical-board
bring-up.

This is the current ESP32-P4 baseline, not a universal future baseline for
every Espressif SoC target. No ESP32-S31 toolchain, target, or emulator
requirement has been established.

## Development targets

The intended progression is:

1. Ubuntu native builds and tests for emulator-core and logic work.
2. `esp-emu` for ESP32-P4 firmware, FreeRTOS, RISC-V integration, and
   headless regression.
3. Physical ESP32-P4-NANO-KIT-D for board peripherals and performance.
4. Physical M5Stack TAB5 for portability and its board-specific peripherals.

ESP32-S31 / S31 Korvo-1 is a possible future portability target only. It is not
part of the current bring-up sequence and is not implemented, tested, or
validated.

The automated Hello World check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
activate the separately installed ESP-IDF v5.5.4 environment, build and merge
the firmware, then run the explicit `~/.local/bin/esp-emu` executable. It
checks both ESP-IDF v5.5.4 and esp-emu v0.39.0 before building. It must not
routinely call `idf.py set-target`; a target change is an explicit setup
operation because that command regenerates configuration and clears the build
directory.

The ESP-IDF v5.5.4 activation script is not safe under the test script's
strict Bash options and may run an external `eim select` operation. The test
script temporarily relaxes `-e` and `-u`, presents the expected sourced Bash
context, and suppresses that optional `eim` behavior only while activating
ESP-IDF. This workaround is specific to the ESP-IDF v5.5.4 activation script
and should be reviewed if the ESP-IDF version changes.

The future firmware should remain capable of headless operation so CPU,
memory, timer, UART, and other emulator-core tests can run without display or
audio hardware. `esp-emu` performance is not representative of real ESP32-P4
hardware performance.
