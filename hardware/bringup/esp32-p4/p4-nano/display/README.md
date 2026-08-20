# ESP32-P4-NANO 10.1-inch MIPI-DSI display diagnostic

This is an independent, diagnostic-only project for the Waveshare
`10.1-DSI-TOUCH-A` panel. The LCD must remain physically disconnected during
the offline stage. This project does not modify production firmware.

## Correct pin interpretation

The ESP32-P4 datasheet's dedicated-interface table lists package pins 34--41
for MIPI-DSI. Those are package pin numbers, not GPIO numbers. The DSI lane
configuration is selected through the ESP-IDF MIPI-DSI peripheral API; this
diagnostic does not invent `GPIO_NUM_*` assignments for DSI lanes.

Therefore, the earlier claims of UART GPIO37/38, SD GPIO39/40, or Ethernet
GPIO34--36 conflicts with DSI were incorrect and are retracted. UART0 GPIO37/38
and the existing SD GPIO39--44 mapping remain separate board-level signals.

## Hardware configuration encoded for a future test

| Item | Value |
|---|---|
| Panel | JD9365, Waveshare 10.1-DSI-TOUCH-A |
| Native timing | 800 x 1280, no implicit rotation |
| DSI bus | controller 0, 2 data lanes |
| Lane bitrate | 1500 Mbps/lane |
| Pixel format | RGB565 |
| DPI clock | 80 MHz, from pinned Waveshare component configuration |
| H timing | 20 back / 20 pulse / 40 front |
| V timing | 10 back / 4 pulse / 30 front |
| Calculated refresh | approximately 68.66 Hz from the above totals; not a 60 Hz measurement |
| Panel reset | `GPIO_NUM_NC` |
| Backlight GPIO | `GPIO_NUM_NC` |
| DSI PHY LDO | channel 3, 2500 mV |
| Backlight I2C | controller 1, GPIO7 SDA / GPIO8 SCL, address `0x45`, register `0x96` |

The `0x96` choice is specific to `10.1-DSI-TOUCH-A`. The older
`101M-8001280-IPS-CT-K` uses `0x86`; this diagnostic must not use `0x86` for
the A panel.

The physical backlight and display-side I2C are not accessed while the
hardware-test gate is disabled. The pinned JD9365 component's panel creation
path includes display-side I2C/backlight behavior, so the gate remains OFF
until the FPC, separate panel power, bracket, and safe backlight sequence have
been reviewed together.

GPIO7/GPIO8 are the same physical I2C bus used by the independent ES8311 audio
diagnostic. The audio diagnostic used controller 0, while the Waveshare display
path uses controller 1. Separate diagnostics may use either controller, but
future production integration must select one controller and one shared bus
handle for ES8311, display/backlight, and touch.

## Offline software stage

The default configuration is:

```ini
CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST=n
```

With this setting the application only allocates one 2,048,000-byte
RGB565 framebuffer in DMA-capable PSRAM, generates deterministic patterns,
checks representative pixels, and calculates CRC32. It does not:

- create a DSI bus or power the DSI PHY;
- create DBI IO or a JD9365 panel;
- send JD9365 commands;
- initialize or access the display-side I2C device at `0x45`;
- write backlight register `0x96`;
- require the LCD FPC, panel 5 V, or touch controller.

Patterns are `BLACK`, `RED`, `GREEN`, `BLUE`, `BARS`, `CHECKER`, and `BORDER`.
The border includes a one-pixel outer border, an inset border, four distinct
corner blocks, and a center cross so orientation and clipping are visible in
the later physical test. No text or font dependency is used.

The locked RGB565 little-endian framebuffer CRC32 values are:

```text
BLACK   0xb483d8cc
RED     0x67196861
GREEN   0x43d010a4
BLUE    0x7743f398
BARS    0x22b23526
CHECKER 0xfd8b8a01
BORDER  0x446766bc
```

Expected markers are:

```text
P4-NANO DISPLAY OFFLINE START
P4-NANO DISPLAY HW GATE: DISABLED
P4-NANO DISPLAY FB ALLOC: PASS bytes=2048000
P4-NANO DISPLAY PATTERN <name>: PASS crc32=...
P4-NANO DISPLAY HW ACCESS: NOT RUN
P4-NANO DISPLAY OFFLINE RESULT: PASS
P4-NANO DISPLAY HARDWARE TEST: NOT RUN
```

The application must not emit a visible-display or physical-result PASS in
offline mode.

## Native regression

The pure pattern generator has a standalone host test that does not depend on
ESP-IDF or MIPI hardware:

```bash
cc -std=c11 -Wall -Wextra -Werror \
  -I main main/display_patterns.c native/test_display_patterns.c \
  -o /tmp/p4-nano-display-patterns
/tmp/p4-nano-display-patterns
```

It verifies geometry, stride, framebuffer size, representative pixels,
solid-color values, bar regions, checker alternation, border/corner/center
markers, and complete-frame CRC32 values.

## Build

Use ESP-IDF v5.5.4:

```bash
cd hardware/bringup/esp32-p4/p4-nano/display
idf.py set-target esp32p4
idf.py build
```

The physical DSI path remains compiler-checked even though its runtime gate is
off. The dependency is pinned exactly to
`waveshare/esp_lcd_jd9365_10_1` version `1.0.4`; BSP, LVGL, touch, SD, camera,
Wi-Fi, BLE, and image-decoder dependencies are intentionally absent.

## Future physical acceptance boundary

Only after the mechanical FPC-protection bracket is complete and the panel is
connected with board and panel power off may the gate be reviewed for enabling.
The later physical test must separately establish DSI/control initialization,
framebuffer scanout, backlight safety, visible colors, geometry, orientation,
and stable final display. Touch, UI, production integration, simultaneous
heavy-peripheral use, and 60-Hz accuracy remain untested.
