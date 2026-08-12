# Firmware

This directory contains the first minimal ESP32-P4 firmware application. It
is a headless Hello World target with no board-specific code, peripherals, or
component dependencies. The firmware is implemented, not yet built or
executed/verified.

The entry point prints the stable UART marker:

```text
ESP-NP2KAI HELLO WORLD OK
```

The project target is `esp32p4`. New firmware C++ is compiled as GNU C++20;
exceptions and RTTI are disabled. The `sdkconfig.defaults` file contains the
small set of project-owned defaults, while generated `sdkconfig` remains a
local file.

For `esp-emu` v0.39.0, the defaults currently select ESP32-P4 ROM revision 0
compatibility with `CONFIG_ESP32P4_REV_MIN_0=y` and
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. These settings must be reviewed for
physical P4-NANO and TAB5 bring-up.

The future build-and-emulation check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
preserve combined emulator/UART output in
`firmware/build/esp-emu-hello-world.log` and will not silently change the
configured target.

The eventual firmware should keep board-specific code outside the emulator
core and retain a headless mode for emulator-core and integration tests.
