# ESP32-P4-NANO hardware bring-up

Status: **MEASURED ON REAL HARDWARE: SOFTWARE/UART PASS; LED BLINK PASS; FLASH/PSRAM PASS**

This directory contains an independent, retained hardware diagnostic for the
Waveshare ESP32-P4-NANO. It is intentionally separate from the production
firmware under `firmware/` and does not use the production partition table,
PSRAM, networking, or other board peripherals.

## Diagnostic

`gpio-blink/` is a minimal ESP-IDF 5.5.4 application for the measured ESP32-P4
v1.3 board revision. It supports the ESP32-P4 v1.x silicon range, uses a
16 MiB flash image setting, the normal ESP-IDF UART console, and GPIO20 as one
fixed external LED output.

At startup it logs:

```text
P4-NANO GPIO BLINK START
```

The LED output is driven high for 500 ms and low for 500 ms. A low-frequency
alive message is printed on the UART console every ten complete blink cycles.

## Measured on real hardware

The first real-hardware bring-up was completed on a Waveshare ESP32-P4-NANO.

```text
Board:                         Waveshare ESP32-P4-NANO
ESP32-P4 silicon revision:     v1.3
ESP-IDF:                       v5.5.4
Programming interface:         CH343P USB-UART
Stable serial path:            /dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61041224-if00
NOR Flash detected:            16 MiB
PSRAM detected by factory FW:  32 MiB
gpio-blink silicon support:    ESP32-P4 v1.x
External LED GPIO:             GPIO20
Application marker:            P4-NANO GPIO BLINK START
Periodic alive logs:           observed
Runtime stability:             no panic/watchdog/reboot loop for approximately 20 seconds
Human visual confirmation:     500 ms ON / 500 ms OFF, approximately 1 Hz
```

The 16 MiB NOR Flash and 32 MiB PSRAM observations above came from the
original factory firmware and its read-only hardware identification. The
`gpio-blink` diagnostic intentionally does not enable or use PSRAM.

The observed result was:

```text
REAL HARDWARE SOFTWARE/UART: PASS
REAL HARDWARE LED BLINK: PASS
```

## Flash/PSRAM diagnostic

The independent `flash-psram/` project verifies Flash and PSRAM using our own
ESP-IDF v5.5.4 firmware. It retains the ESP32-P4 v1.x configuration, uses the
normal single-app partition table, initializes PSRAM during ESP-IDF startup,
enables the ESP-IDF startup memory test, and allocates test memory explicitly
with `MALLOC_CAP_SPIRAM`. It does not modify or depend on production firmware.

The following results were **MEASURED ON REAL HARDWARE**:

```text
Board:                         Waveshare ESP32-P4-NANO
ESP32-P4 silicon revision:     v1.3
ESP-IDF:                       v5.5.4
Programming interface:         CH343P USB-UART
Stable serial path:            /dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61041224-if00
NOR Flash:                     16 MiB
PSRAM:                         33,554,432 bytes
PSRAM mode:                    HEX
PSRAM clock:                   200 MHz
ESP-IDF startup memory test:   SPI SRAM memory test OK
MALLOC_CAP_SPIRAM total:       33,554,432 bytes
MALLOC_CAP_SPIRAM free:        33,551,756 bytes
MALLOC_CAP_SPIRAM largest:     33,030,144 bytes
Application allocation/test:   32,768,000 bytes
Data mismatches:               0
Heap integrity after free:     PASS
Runtime heartbeat:              stable for more than 20 seconds
Runtime faults:                 no panic/watchdog/reset loop
```

The application tested 32,768,000 bytes, leaving a safety margin from the
largest reported free block; it does not claim that the application-level
sweep covered every byte of the 32 MiB PSRAM device. Two distinct deterministic
address/index-dependent 32-bit patterns were written and read back across the
entire tested allocation.

The factory firmware had independently reported 32 MiB PSRAM during the earlier
read-only hardware identification. The result below is independent evidence
from the new ESP-IDF v5.5.4 `flash-psram` firmware, including its startup memory
test, capability-allocator statistics, allocation, data verification, and
post-free heap-integrity check.

The observed result was:

```text
P4-NANO FLASH CHECK: PASS
P4-NANO PSRAM INIT: PASS
P4-NANO PSRAM DATA TEST: PASS
P4-NANO FLASH-PSRAM RESULT: PASS

REAL HARDWARE FLASH: PASS
REAL HARDWARE PSRAM INIT: PASS
REAL HARDWARE PSRAM DATA TEST: PASS
```

## Confirmed wiring

GPIO20 and the physical P1 pin were checked against the board silkscreen and
pinout before the first flash.

```text
GPIO20 (P1 pin 13) -> 1 kOhm resistor -> LED anode (+)
LED cathode (-) -> GND
```

Use an external LED. The onboard USER LED is a power indicator and is not the
GPIO20 diagnostic LED. Do not connect the LED or GPIO20 to 5 V. Do not use
GPIO2 for this test.

The programming console is the board's CH343P USB-UART path at 115200 baud.
The application leaves ESP-IDF's default UART0 console routing unchanged:
GPIO37 is UART0 TX and GPIO38 is UART0 RX on ESP32-P4.

## Build and validation

From the repository root:

```bash
source tools/emu/activate-idf.sh
cd hardware/bringup/esp32-p4/p4-nano/gpio-blink
idf.py set-target esp32p4
idf.py build
```

The project has no custom partition table and does not enable PSRAM. Build
artifacts are local to this project. The diagnostic was flashed only after
the wiring review, using the stable serial path recorded above. No production
firmware files were modified.

The GPIO20 and Flash/PSRAM real-hardware bring-up diagnostics are complete.
Further diagnostics, such as SD/MMC, MIPI-DSI, USB host, audio, and production
firmware tests, remain out of scope for this milestone.
