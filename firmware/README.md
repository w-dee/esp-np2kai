# Firmware

This directory contains the first minimal ESP32-P4 firmware application. It
is a headless Hello World target with no board-specific code, peripherals, or
component dependencies. It is verified under esp-emu v0.39.0 with ESP-IDF
v5.5.4.

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

The build-and-emulation check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
build the firmware, create a merged image, boot it under esp-emu, detect
`ESP-NP2KAI HELLO WORLD OK`, and preserve combined emulator/UART output in
`firmware/build/esp-emu-hello-world.log`. It will not silently change the
configured target.

The eventual firmware should keep board-specific code outside the emulator
core and retain a headless mode for emulator-core and integration tests.
