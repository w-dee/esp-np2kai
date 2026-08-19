# ESP32-P4-NANO hardware bring-up

Status: **BUILT FOR REAL-HARDWARE TEST, NOT YET FLASHED**

This directory contains an independent, retained hardware diagnostic for the
Waveshare ESP32-P4-NANO. It is intentionally separate from the production
firmware under `firmware/` and does not use the production partition table,
PSRAM, networking, or other board peripherals.

## Diagnostic

`gpio-blink/` is a minimal ESP-IDF 5.5.4 application for the measured ESP32-P4
v1.3 board revision. It uses the P4 v1.x compatibility selection, a 16 MiB
flash image setting, the normal ESP-IDF UART console, and GPIO20 as one fixed
external LED output.

At startup it logs:

```text
P4-NANO GPIO BLINK START
```

The LED output is driven high for 500 ms and low for 500 ms. A low-frequency
alive message is printed on the UART console every ten complete blink cycles.

## Wiring for human review

Before any first flash, verify GPIO20 and the physical P1 pin against the
board silkscreen and the board pinout.

```text
GPIO20 (P1 pin 13, verify) -> 1 kOhm resistor -> LED anode (+)
LED cathode (-) -> GND
```

Use an external LED. The onboard USER LED is a power indicator and is not the
GPIO20 diagnostic LED. Do not connect the LED or GPIO20 to 5 V. Do not use
GPIO2 for this test.

The programming console is the board's CH343P USB-UART path at 115200 baud.
The application leaves ESP-IDF's default UART0 console routing unchanged:
GPIO37 is UART0 TX and GPIO38 is UART0 RX on ESP32-P4.

## Build only

From the repository root:

```bash
source tools/emu/activate-idf.sh
cd hardware/bringup/esp32-p4/p4-nano/gpio-blink
idf.py set-target esp32p4
idf.py build
```

The project has no custom partition table and does not enable PSRAM. Build
artifacts are local to this project. The build step does not flash or erase
the connected board.

The first flash command is intentionally withheld for human review. Confirm
the wiring and review the build report before performing any write operation.
