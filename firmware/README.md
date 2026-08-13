# Firmware

This directory contains the current minimal ESP32-P4 firmware application. It
is a headless Hello World and UART Control Plane Base target with no
board-specific code or peripherals. The firmware contains the
`control_plane` and `uart_control_transport` components and uses only
ESP-IDF-provided JSON, UART, VFS, and FreeRTOS functionality; no external
third-party component dependency has been added. The Hello World and UART
Control Plane paths are verified under esp-emu v0.39.0 with ESP-IDF v5.5.4.

The Binary Data Plane v1 and its bidirectional UART-TCP integration test are
implemented but not yet verified under esp-emu. Its transfer test remains
separate from the verified JSON Control Plane path.

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

The Binary Data Plane integration check is
[`tools/emu/test-uart-binary-data-plane.sh`](../tools/emu/test-uart-binary-data-plane.sh).
It is implemented for the COBS/CRC transfer protocol and deterministic 64 KiB
test endpoints, but its emulator execution has not yet been verified.

The eventual firmware should keep board-specific code outside the emulator
core and retain a headless mode for emulator-core and integration tests.
