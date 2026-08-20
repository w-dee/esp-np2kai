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

## Hardware configuration

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
hardware-test gate is disabled. The managed component source retains its
constructor-side behavior, but the compiled project-owned adapter is used by
the future physical path and does not execute that block. The gate remains OFF
until the FPC, separate panel power, bracket, and safe backlight sequence have
been reviewed together.

GPIO7/GPIO8 are the same physical I2C bus used by the independent ES8311 audio
diagnostic. The audio diagnostic used controller 0, while the Waveshare display
path uses controller 1. Separate diagnostics may use either controller, but
future production integration must select one controller and one shared bus
handle for ES8311, display/backlight, and touch.

## JD9365 component side effect and safe physical path

The exact locally resolved `waveshare/esp_lcd_jd9365_10_1` version `1.0.4`
has an implicit display-side I2C operation inside
`esp_lcd_new_panel_jd9365()`. Its constructor creates `I2C_NUM_1` on GPIO7
(SDA) and GPIO8 (SCL), addresses `0x45`, writes register `0x95` twice with
the vendor values `0x11` and `0x17`, writes register `0x96` with `0x00`,
delays, and then writes register `0x96` with `0xff`. This constructor combines
panel power/control initialization with backlight policy; the final `0xff`
write is the unsafe automatic full-brightness behavior.

The register roles are supported by a near-authoritative Waveshare panel
regulator implementation: `0x95` is identified as `REG_LCD` and `0x96` as
`REG_PWM`. Its power-state mapping assigns LCD AVDD, panel reset, backlight
enable, and IOVCC control bits to the low byte of `0x95`; consequently,
`0x11` (bits 0 and 4) followed by `0x17` (bits 0, 1, 2, and 4) enables the
additional panel-control lines. The exact controller-MCU bit names are not
published in a Waveshare panel datasheet, so this bit-level interpretation is
source-based rather than a claim of undocumented vendor register definitions.
The same `0x11` -> `0x17` sequence is present in known-working
10.1-DSI-TOUCH-A implementations and is treated as required for cold-start
panel wake/power initialization until the first physical test proves it on
this board. See the [Waveshare panel regulator source](https://www.spinics.net/lists/kernel/msg5921548.html).

The reviewed `2.0.0` source retains the same constructor-side I2C/backlight
behavior and does not provide a suitable disable/override hook. Its API and
dependency changes do not justify switching versions for this bring-up, so
the diagnostic remains pinned to `1.0.4`.

The project therefore owns a minimal adapter in
`main/p4_nano_jd9365_safe.c`. It preserves the JD9365 DSI initialization table
and panel operations while removing all constructor-side I2C traffic.
Project-owned `main.c` separates `display_control_init()`,
`display_control_panel_power_on()`, and `display_backlight_set()`. The future
physical sequence explicitly writes `0x95 = 0x11`, `0x95 = 0x17`, then
`0x96 = 0x00`, waits 100 ms, enables DSI, obtains the DPI framebuffer, primes
and synchronizes a black RGB565 frame, initializes and enables the panel,
waits for stable black scanout, and only then writes a conservative initial
level of `0x40` (approximately 25% only if the device mapping is linear; this
is not a calibrated brightness value). No automatic brightness ramp or
`0x96 = 0xff` write is used.

The compiled physical path includes DSI PHY LDO enable, DSI bus and DBI
creation, safe JD9365 initialization, DPI framebuffer acquisition, black-frame
priming, conservative backlight enable, all seven deterministic patterns with
CRC checks, and final backlight-off cleanup. The hardware gate remains disabled
by default. If the physical path fails after I2C initialization, cleanup only
attempts `0x96 = 0x00` before releasing resources. No undocumented `0x95`
power-off value is invented.

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

The offline run on the physical ESP32-P4 is the approved runtime check for
this boundary. With `CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST=n`, the control
flow returns before `esp_ldo_acquire_channel()`, DSI bus creation, DBI IO
creation, the project-owned JD9365 constructor, or any display-side I2C
transaction. The managed component's side-effecting constructor is compiled
as a dependency but is not called.

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

## Measured on real hardware: offline stage

The LCD remained physically disconnected. Using ESP-IDF v5.5.4 and
`CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST=n`, the diagnostic was flashed to an
ESP32-P4-NANO (ESP32-P4 revision v1.3) through the normal CH343P UART path.
The 2,048,000-byte DMA-capable PSRAM framebuffer allocation succeeded on the
physical board, and the complete offline sequence passed:

```text
P4-NANO DISPLAY OFFLINE START
P4-NANO DISPLAY HW GATE: DISABLED
P4-NANO DISPLAY FB ALLOC: PASS bytes=2048000
P4-NANO DISPLAY PATTERN BLACK: PASS crc32=0xb483d8cc
P4-NANO DISPLAY PATTERN RED: PASS crc32=0x67196861
P4-NANO DISPLAY PATTERN GREEN: PASS crc32=0x43d010a4
P4-NANO DISPLAY PATTERN BLUE: PASS crc32=0x7743f398
P4-NANO DISPLAY PATTERN BARS: PASS crc32=0x22b23526
P4-NANO DISPLAY PATTERN CHECKER: PASS crc32=0xfd8b8a01
P4-NANO DISPLAY PATTERN BORDER: PASS crc32=0x446766bc
P4-NANO DISPLAY HW ACCESS: NOT RUN
P4-NANO DISPLAY OFFLINE RESULT: PASS
P4-NANO DISPLAY HARDWARE TEST: NOT RUN
```

The run showed no panic, watchdog, or reset loop. Source and configuration
control-flow review confirms that the offline run did not execute DSI PHY LDO
acquisition, DSI bus creation, DBI IO creation, the JD9365 constructor, any
transaction to display-side I2C address `0x45`, or any write to registers
`0x95`/`0x96`. This is an offline software/PSRAM result, not a physical
display result; the LCD has not yet been connected or validated.

## Measured on real hardware: physical display stage

The first physical display test was completed once with the LCD FPC and
separate display power connected. The test used the project-owned safe JD9365
adapter rather than the upstream constructor-side backlight behavior. The
hardware gate was enabled only in the temporary generated test configuration;
the tracked default remained disabled.

```text
Board:                 Waveshare ESP32-P4-NANO
SoC:                   ESP32-P4 rev v1.3
Panel:                 Waveshare 10.1-DSI-TOUCH-A
LCD controller:        JD9365, ID 93 65 04
Native resolution:     800 x 1280
Orientation tested:    native portrait, no rotation
Pixel format:          RGB565
DSI:                   2 lanes, 1500 Mbps/lane
DPI clock:             80 MHz
ESP-IDF:               v5.5.4
Component:             waveshare/esp_lcd_jd9365_10_1 1.0.4
```

The successful control and display-side sequence was:

```text
I2C controller init
    -> 0x95 = 0x11
    -> 0x95 = 0x17
    -> 0x96 = 0x00
    -> 100 ms wait
    -> DSI PHY LDO enable
    -> DSI bus
    -> DBI IO
    -> safe JD9365 panel construction
    -> DPI framebuffer
    -> BLACK framebuffer + cache sync
    -> panel reset/init/display-on
    -> 200 ms wait
    -> 0x96 = 0x40
    -> pattern sequence
    -> 0x96 = 0x00
```

The project-owned path does not write `0x96 = 0xff`. The `0x40` value is only
the conservative test level that worked on this board; it is not a calibrated
backlight percentage.

The deterministic digital results were:

```text
BLACK   0xb483d8cc
RED     0x67196861
GREEN   0x43d010a4
BLUE    0x7743f398
BARS    0x22b23526
CHECKER 0xfd8b8a01
BORDER  0x446766bc

P4-NANO DISPLAY HARDWARE RESULT: PASS
panic: none
watchdog: none
reset loop: none
```

These CRCs validate deterministic framebuffer generation and transfer-side
checks; they do not by themselves prove physical display correctness.

Human visual confirmation was recorded separately:

```text
P4-NANO DISPLAY VISIBLE: PASS
P4-NANO DISPLAY COLORS: PASS
P4-NANO DISPLAY GEOMETRY: PASS
P4-NANO DISPLAY PHYSICAL RESULT: PASS
```

The native-size image was visible. Red, green, and blue were displayed
correctly; the color-bar, checkerboard, and border/corner-marker patterns were
visually correct. The border covered the full intended display area exactly.
No quantitative refresh-rate, color-accuracy, luminance, flicker, or signal-
margin claim is made.

After this single physical test, the safe gate-off firmware was flashed
successfully. The restored boot reported:

```text
CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST=n
P4-NANO DISPLAY HW GATE: DISABLED
P4-NANO DISPLAY HW ACCESS: NOT RUN
P4-NANO DISPLAY OFFLINE RESULT: PASS
P4-NANO DISPLAY HARDWARE TEST: NOT RUN
```

Subsequent normal resets and power cycles therefore do not automatically
repeat the physical display test.

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

## Remaining untested scope

The following remain untested:

```text
GT9271 touch input
touch coordinate mapping
touch reset/interrupt behavior
LVGL
UI framework
1280x800 logical landscape rendering
runtime rotation
RGB888
measured refresh rate
long-duration DSI stability
DSI lane-rate margin
color calibration
brightness calibration
concurrent display + SD
concurrent display + audio
concurrent display + wireless
concurrent display + camera
production framebuffer integration
NP2 rendering
scaling/PPA path
```
