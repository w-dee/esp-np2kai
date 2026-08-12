# Development Environment

## Baseline

The initial project baseline is:

| Item | Version or target |
| --- | --- |
| Host OS | Ubuntu 24.04 |
| ESP-IDF | v5.5.4 |
| esp-emu | v0.39.0 |
| ESP target | `esp32p4` |
| New ESP32-P4-side C++ | C++20 |

The versions are recorded in [`tools/versions.env`](../tools/versions.env).
That file is informational and must not modify the user's shell automatically.

## Environment boundaries

The project must not use or depend on a PlatformIO-installed ESP-IDF
environment. The ESP-IDF and `esp-emu` setup should be reproducible and
explicitly selected by the developer or CI environment. This initial setup
does not install system packages, ESP-IDF, `esp-emu`, or other external
software.

The minimal headless Hello World application now exists under `firmware/`, but
it has not yet been built or executed. No component dependencies have been
added.

The firmware targets `esp32p4` through `firmware/sdkconfig.defaults`. New
firmware C++ is explicitly compiled as GNU C++20 in the `main` component.
C++ exceptions and RTTI are disabled through ESP-IDF configuration, and no
`iostream` is used.

For compatibility with `esp-emu` v0.39.0, the current defaults also select
the ESP32-P4 ROM revision 0 compatibility settings
`CONFIG_ESP32P4_REV_MIN_0=y` and
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. These settings are currently for
emulator compatibility and must be reviewed again during physical-board
bring-up.

## Development targets

The intended progression is:

1. Ubuntu native builds and tests for emulator-core and logic work.
2. `esp-emu` for ESP32-P4 firmware, FreeRTOS, RISC-V integration, and
   headless regression.
3. Physical ESP32-P4-NANO-KIT-D for board peripherals and performance.
4. Physical M5Stack TAB5 for portability and its board-specific peripherals.

The future automated Hello World check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
activate the separately installed ESP-IDF v5.5.4 environment, build and merge
the firmware, then run the explicit `~/.local/bin/esp-emu` executable. It must
not routinely call `idf.py set-target`; a target change is an explicit setup
operation because that command regenerates configuration and clears the build
directory.

The script and firmware are implemented, not yet executed or verified.

The future firmware should remain capable of headless operation so CPU,
memory, timer, UART, and other emulator-core tests can run without display or
audio hardware. `esp-emu` performance is not representative of real ESP32-P4
hardware performance.
