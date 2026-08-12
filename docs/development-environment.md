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

The repository does not yet contain an ESP-IDF application, CMake files, or
component dependencies. Those belong to a later task.

## Development targets

The intended progression is:

1. Ubuntu native builds and tests for emulator-core and logic work.
2. `esp-emu` for ESP32-P4 firmware, FreeRTOS, RISC-V integration, and
   headless regression.
3. Physical ESP32-P4-NANO-KIT-D for board peripherals and performance.
4. Physical M5Stack TAB5 for portability and its board-specific peripherals.

The future firmware should remain capable of headless operation so CPU,
memory, timer, UART, and other emulator-core tests can run without display or
audio hardware. `esp-emu` performance is not representative of real ESP32-P4
hardware performance.

