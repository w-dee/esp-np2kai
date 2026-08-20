# ESP32-P4-NANO SDMMC bring-up diagnostic

This is an independent ESP-IDF 5.5.4 diagnostic for the Waveshare
ESP32-P4-NANO. It is not part of the production storage or File Transfer
implementation.

The diagnostic starts with a read-only card and FAT check. It does not format,
repartition, erase raw sectors, overwrite `README.TXT`, overwrite an existing
scratch file, or accept arbitrary host-selected paths or data.

## Board configuration

The initial configuration is deliberately conservative:

```text
SDMMC host slot:       slot 1 (GPIO-matrix, non-UHS)
Bus width:             4-bit
Card clock:            20 MHz (SDMMC_FREQ_DEFAULT)
CLK:                   GPIO43
CMD:                   GPIO44
D0:                    GPIO39
D1:                    GPIO40
D2:                    GPIO41
D3:                    GPIO42
D4-D7:                 GPIO_NUM_NC (not connected; never configured)
Card detect:           unused
Write protect:         unused
SD power:              ESP32-P4 on-chip LDO channel 4 / ESP_LDO_VO4
GPIO45:                not driven; board-default power state is retained
```

The Waveshare schematic routes `ESP_LDO_VO4` into the SD power network, and
the board's SD power-control path includes GPIO45. The diagnostic therefore
uses the ESP-IDF SD power-control driver for LDO channel 4 before card
initialization, but does not guess a GPIO45 active level or drive that GPIO.
The actual board power state must be confirmed during the first physical run;
a card-init failure is a reason to stop and investigate, not to guess a new
polarity.

Prepare a FAT card with this exact root file before testing:

```text
/README.TXT
ESP32-P4 SD TEST CARD
```

The file is verified byte-for-byte, including its final newline. The
read-only stage never writes to the card.

## Automatic read-only stage

At boot the diagnostic reports separate markers for power configuration, host
initialization, card initialization, card information, FAT mounting, root
listing, and the exact `README.TXT` read. Mount failure never triggers a
format. The required final marker is:

```text
P4-NANO SD READ-ONLY RESULT: PASS
```

If any read-only stage fails, the application stops and will not accept the
write test.

## Explicit bounded write test

Only after the read-only result passes, send the exact line below through the
normal ESP-IDF console:

```text
WRITE_TEST
```

One command performs one fixed transaction against
`/sdcard/P4SDTEST.BIN`: exclusive create, deterministic 4096-byte write,
flush and close, read-only reopen, byte-for-byte verification, unlink, and an
absence check. If the scratch file already exists, the diagnostic reports
`SCRATCH_EXISTS` and does not overwrite or delete it. `README.TXT` is never
modified.

The write-stage markers are:

```text
P4-NANO SD FILE CREATE: PASS/FAIL
P4-NANO SD FILE WRITE: PASS/FAIL
P4-NANO SD FILE READ: PASS/FAIL
P4-NANO SD FILE VERIFY: PASS/FAIL
P4-NANO SD FILE DELETE: PASS/FAIL
P4-NANO SD WRITE TEST RESULT: PASS/FAIL
```

## Safe-to-power-off indicator

The existing active-high external LED is dedicated to the safe-to-power-off
state:

```text
GPIO20 -> 1 kOhm -> LED anode
LED cathode -> GND
```

GPIO20 starts LOW and remains OFF during startup, read-only validation, UART
initialization, filesystem activity, and failure states. After
`P4-NANO SD READ-ONLY RESULT: PASS` and
`P4-NANO SD WRITE COMMAND READY`, the diagnostic emits:

```text
P4-NANO SAFE-OFF LED ENABLED
P4-NANO SAFE TO POWER OFF: YES
```

and blinks GPIO20 at 250 ms ON / 250 ms OFF. When `WRITE_TEST` is accepted,
the LED is forced LOW and the diagnostic emits `SAFE-OFF LED DISABLED` and
`SAFE TO POWER OFF: NO`. The LED is re-enabled only after the complete
write/read/verify/delete transaction passes.

The host helpers require an explicit `--port`. The cold-boot monitor treats
CH343P disappearance during human power-off as expected and requires the
safe-off LED marker on every counted boot. The write helper validates the
ordered LED-disabled, filesystem, and LED-enabled markers for one transaction.

## Build and first validation

From the repository root:

```bash
source tools/emu/activate-idf.sh
cd hardware/bringup/esp32-p4/p4-nano/sdmmc
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61041224-if00 flash
```

Use `tools/cold_boot_monitor.py` for the human-controlled cold-boot procedure
and `tools/acceptance_write.py` for one deterministic bounded write command.
Do not run endurance, hot-plug/card-detect, mount/unmount loops, raw-sector
tests, or large writes.

## Measured on real hardware

The following evidence was measured on a Waveshare ESP32-P4-NANO:

```text
Board:                         Waveshare ESP32-P4-NANO
ESP32-P4 silicon revision:     v1.3
ESP-IDF:                       v5.5.4
Card:                          SDHC
Capacity:                      8,068,792,320 bytes
Reported size:                 7695 MB
Sector size:                   512 bytes
Interface:                     native SDMMC
Host slot:                     1
Bus width:                     4-bit
Clock:                         20 MHz (real frequency 20,000 kHz)
CLK/CMD:                       GPIO43 / GPIO44
D0-D3:                         GPIO39-GPIO42
D4-D7:                         GPIO_NUM_NC
LDO:                           channel 4 / ESP_LDO_VO4
GPIO45:                        not explicitly driven
README.TXT exact bytes:        ESP32-P4 SD TEST CARD\n
README.TXT size:               22 bytes
Initial read-only validation:  PASS
Ubuntu post-write inspection:  PASS
Fully captured WRITE_TEST:     3 / 3 PASS
Formal cold-boot read-only:    5 / 5 PASS
GPIO20 safe-off blink:         PASS
SAFE-OFF write transition:     PASS
```

The Ubuntu inspection confirmed that `README.TXT` remained unchanged and that
the scratch file was absent after the transaction. A separate focused
synchronization validation performed exactly one additional WRITE_TEST and
passed the LED-disabled/NO, 4096-byte write/read/verify/delete, and
LED-enabled/YES sequence; this transaction is not added to the established
3/3 acceptance tally.

The known non-functional ESP-IDF v5.5.4 warning was observed:

```text
The voltage value 0 is out of the recommended range [500, 2700]
```

Startup UART garbage `0xff` was observed and safely ignored. No SDMMC
timeout, CRC, FAT, VFS, short-count, verification, panic, watchdog, or reset
loop was observed.

Diagnostic issues resolved during bring-up:

1. `fgets(stdin)` could terminate because default UART VFS RX was
   non-blocking/`EWOULDBLOCK`; explicit UART-driver RX made reception
   persistent.
2. `P4NANO_SCRATCH.BIN` was incompatible with LFN-disabled FatFs;
   `P4SDTEST.BIN` is a valid 8.3 replacement.
3. An early host sender could match a stale RESULT marker; ordered
   per-transaction validation fixed this.
4. CH343P disappearance during power-off is expected; the cold-boot monitor
   now reconnects for each boot.
5. The safe-off blink task had a stale-enabled race; synchronized HIGH checks
   and unconditional LOW after the ON interval fixed it.
6. GPIO20 provides the human-visible safe-power-off indication.
7. The earlier 23-byte README expectation was an instruction error; the exact
   file size is 22 bytes.

The following remain untested or out of scope: 40 MHz operation, hot-plug/card
detect, endurance testing, LFN support, arbitrary filesystem operations,
production storage integration, and production UART File Transfer-to-SD
integration. This diagnostic is not production firmware.
