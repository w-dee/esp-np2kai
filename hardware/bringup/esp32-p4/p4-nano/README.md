# ESP32-P4-NANO hardware bring-up

Status: **MEASURED ON REAL HARDWARE: SOFTWARE/UART PASS; LED BLINK PASS; FLASH/PSRAM PASS; SDMMC PASS; WIRELESS COMPANION TRANSPORT PASS; AUDIO SPEAKER OUTPUT PASS**

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

## Production headless bring-up

The first production headless bring-up was also **MEASURED ON REAL HARDWARE**.
The production `p4-v1x` image booted on the physical board with the
production PSRAM and external-BSS configuration enabled. Its configured
8 MiB Flash envelope was used on the board's physical 16 MiB NOR Flash; this
does not change the independently measured physical Flash capacity above.

```text
Board:                              Waveshare ESP32-P4-NANO
ESP32-P4 silicon revision:          v1.3
ESP-IDF:                            v5.5.4
Production image:                   p4-v1x booted successfully
Configured production Flash envelope: 8 MiB
Physical NOR Flash:                 16 MiB
Production PSRAM/external-BSS:      booted successfully
PSRAM initialized:                  32 MiB
NP2MEM_RESULT=PASS
UART Control Plane TX/RX:           PASS through CH343P USB-UART
TRANSPORT_SYNC:                     four consecutive NUL bytes (00 00 00 00)
Startup sync + first protocol.hello: 20/20 PASS
system.ping / system.info:           PASS
Late attach after approximately 20 seconds: PASS
Bounded garbage recovery:            PASS
uart_flush_input() correctness dependency: none
Runtime faults:                     no panic/watchdog/reset loop during validated runs
```

The production headless validation covered boot, PSRAM initialization and
external-BSS placement, the memory probe, and the UART Control Plane. It did
not provision the raw NP2 fixture or run the formal NP2TEST:

```text
RAW NP2 FIXTURE: NOT PROVISIONED
FORMAL NP2TEST: NOT RUN
```

These results must not be read as a formal NP2 emulation pass on real hardware.
The three PSRAM/memory evidence sources remain distinct:

1. The original factory firmware independently reported 32 MiB PSRAM during
   the initial read-only hardware identification.
2. The independent `flash-psram` diagnostic verified Flash, PSRAM startup,
   capability-allocator behavior, an explicitly tested 32,768,000-byte region,
   data integrity, and heap integrity using our ESP-IDF v5.5.4 diagnostic
   firmware.
3. The production `p4-v1x` firmware independently booted with its production
   PSRAM/external-BSS configuration and reported `NP2MEM_RESULT=PASS`.

## Speaker audio diagnostic

The independent audio diagnostic validated the P4-NANO speaker-output path;
it does not claim microphone, recording, or full audio-subsystem validation.
The following was **MEASURED ON REAL HARDWARE**:

```text
Board:                 Waveshare ESP32-P4-NANO
Codec:                 ES8311 at I2C address 0x18
Amplifier:             NS4150B
Speaker:               small speaker supplied with ESP32-P4-NANO-KIT-D
                       exact impedance/power rating not independently measured
I2C:                   SDA GPIO7, SCL GPIO8
I2S:                   DOUT GPIO9, WS GPIO10, BCLK GPIO12, MCLK GPIO13
                       48 kHz, signed 16-bit stereo
PA:                    GPIO53, active-high, final state LOW
Generated stimulus:    1 kHz tone, three audible bursts
Digital result:        PASS
Physical speaker output: PASS
```

The diagnostic uses ESP-IDF v5.5.4, `espressif/es8311` 1.0.0~1, codec volume
parameter 55, and PCM peak 4096. Human observation confirmed three audible
tone bursts separated by silence, final silence, and the same sequence after a
P4 reset. An external spectrum analyzer observed the generated tone at
nominally 1.000 kHz. This does not claim calibrated frequency accuracy, THD,
SPL, or output power.

The following audio areas remain explicitly untested:

```text
microphone input
ADC path
recording
echo
stereo acoustic separation
output power
SPL
THD
frequency sweep
WAV/music playback
production audio integration
simultaneous audio with other heavy peripherals
```

## Wireless companion transport

The wireless companion bring-up validated the P4 host reset/control path,
ESP-Hosted connection, and Layer-1 SDIO transport to the factory C6. It did
not validate Wi-Fi network operation, BLE operation, or production wireless
integration. Advertised WLAN/BLE capabilities below are coprocessor
capability observations, not data-plane feature tests.

The following was **MEASURED ON REAL HARDWARE**:

```text
Board:                         Waveshare ESP32-P4-NANO
P4:                            ESP32-P4 v1.3
Companion:                     ESP32-C6-MINI-1-N4
Factory C6 firmware:           network_adapter
                               release/ng-v1.0.2-330-g83efce6
                               ESP-Hosted-MCU Slave FW 0.0.6
Host:                          ESP-IDF v5.5.4, espressif/esp_hosted 3.0.6
Transport:                     SDIO Slot 1, 4-bit, 20 MHz
Pins:                          CLK GPIO18, CMD GPIO19, D0-D3 GPIO14-17
                               reset GPIO54 active-low
Observed C6 chip ID:           0x0d
Observed capabilities:          0x0d
Observed advertisements:        WLAN over SDIO, HCI over SDIO, BLE only
CP INIT:                        received
Connect:                        PASS
Transport:                      PASS
Formal cold-boot repeatability: 5 / 5 PASS
```

All five counted cycles contained one ESP32-P4 POWERON ROM boot, one
`rst:0x1 (POWERON)`, a complete UART capture, CP INIT, C6 identity and
capabilities, and transport PASS. The normal diagnostic keeps the modern
firmware-version query disabled:

```ini
CONFIG_P4_NANO_WIRELESS_RUN_FW_VERSION_QUERY=n
```

The historical protocol-version finding is:

```text
factory protocol:  Req_Max = 350
modern host:      Req_GetCoprocessorFwVersion = 350
```

One historical version-query attempt timed out after five seconds because the
factory CP protocol has no request ID 350. This was not an SDIO transport
failure; the normal acceptance path does not issue that request.

Formal cold-boot capture used a power-only and receive-only configuration:

```text
P4 power:          USB power-only cable
UART capture:      external CH341 RX-only
                   P4 GPIO37 / UART0_TX -> CH341 RX
                   P4 GND -> CH341 GND
CH341 TX/VCC:      not connected
Onboard CH343P:    USB data path disconnected from host
```

An earlier duplicate-ROM observation was strongly associated with the onboard
CH343P USB data/control path or host enumeration/control-line behavior. The
power-only configuration eliminated that observation in the repeated formal
runs. The exact DTR/RTS mechanism is not proven.

The following remain explicitly untested:

```text
Wi-Fi scan
AP association
DHCP / Internet connectivity
Wi-Fi throughput
BLE advertising
BLE pairing
Bluetooth HCI operation
concurrent SD card + C6 SDIO operation
production wireless integration
NP2 networking
RAW NP2 FIXTURE: NOT PROVISIONED
FORMAL NP2TEST: NOT RUN
```

## SDMMC diagnostic

An independent `sdmmc/` diagnostic has now been added for the next physical
bring-up stage. It is intentionally separate from the production firmware and
does not implement or exercise the production File Transfer/storage
abstraction.

The initial board configuration is 4-bit SDMMC at 20 MHz using the Waveshare
SD1 wiring: CLK GPIO43, CMD GPIO44, D0 GPIO39, D1 GPIO40, D2 GPIO41, and D3
GPIO42. The diagnostic uses ESP-IDF's on-chip SD power-control driver with
LDO channel 4, which is routed to the board's `ESP_LDO_VO4` SD power network.
It leaves GPIO45 untouched rather than assuming an undocumented active
polarity. Card detect and write-protect inputs are unused.

At boot it performs only host/card initialization, card information, FAT mount,
root listing, and exact read-only verification of `/README.TXT` containing
`ESP32-P4 SD TEST CARD\n`. A host-triggered `WRITE_TEST` is accepted only after
the read-only result passes and performs one bounded exclusive-create,
write/read/verify/delete transaction on `/sdcard/P4SDTEST.BIN`. It never
formats, repartitions, overwrites an existing scratch file, or modifies
`README.TXT`.

The SDMMC diagnostic is **MEASURED ON REAL HARDWARE** and remains separate from
production storage and File Transfer. It uses native SDMMC slot 1 in 4-bit
mode at 20 MHz: CLK GPIO43, CMD GPIO44, D0-D3 GPIO39-GPIO42, with D4-D7 set to
`GPIO_NUM_NC`. It uses on-chip LDO channel 4 / `ESP_LDO_VO4` and does not
explicitly drive GPIO45.

```text
Board:                         Waveshare ESP32-P4-NANO
ESP32-P4 silicon revision:     v1.3
ESP-IDF:                       v5.5.4
Card:                          SDHC, 8,068,792,320 bytes, 7695 MB reported
Sector size:                   512 bytes
Initial read-only validation:  PASS
Ubuntu post-write inspection:  PASS
Fully captured WRITE_TEST:     3 / 3 PASS
Formal cold-boot read-only:    5 / 5 PASS
GPIO20 safe-off blink:         PASS
SAFE-OFF write transition:     PASS
```

`README.TXT` was verified byte-for-byte as `ESP32-P4 SD TEST CARD\n` with an
exact size of 22 bytes. The focused LED synchronization validation performed
one additional bounded WRITE_TEST and confirmed LOW/NO during the filesystem
transaction and blinking/YES afterward; it does not change the established
3/3 tally.

The known LDO voltage-0 warning was non-functional, and startup `0xff` UART
garbage was safely ignored. The diagnostic does not claim 40 MHz, hot-plug,
endurance, LFN, arbitrary filesystem operations, production storage
integration, or production UART File Transfer-to-SD support.

See [sdmmc/README.md](sdmmc/README.md) for the complete procedure and safety
constraints.

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

The GPIO20, Flash/PSRAM, production headless, SD/MMC, wireless companion
transport, and speaker-output real-hardware bring-up milestones are complete.
MIPI-DSI and USB host remain outside the current bring-up scope; broader audio
functions remain unvalidated as listed above. Formal NP2 emulation on real
hardware remains unvalidated because the raw fixture was not provisioned and
formal NP2TEST was not run.
