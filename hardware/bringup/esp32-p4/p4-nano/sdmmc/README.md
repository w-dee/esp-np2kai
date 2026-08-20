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
`/sdcard/P4NANO_SCRATCH.BIN`: exclusive create, deterministic 4096-byte write,
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

## Build and first validation

From the repository root:

```bash
source tools/emu/activate-idf.sh
cd hardware/bringup/esp32-p4/p4-nano/sdmmc
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61041224-if00 flash monitor
```

The first physical run is read-only. Stop if any required read-only marker is
`FAIL`. If all markers pass, send `WRITE_TEST` once, then perform only the
approved cold-boot and bounded-repeat checks. Do not run endurance, hot-plug,
mount/unmount loops, raw-sector tests, or large writes.

## Validation record

Real-hardware results are intentionally not pre-filled by this source-only
implementation. Record the measured board, power configuration, card type and
capacity, negotiated bus width and frequency, every read-only marker,
`WRITE_TEST` result, repeat counts, and any error names/codes in the parent
bring-up README after the physical run. Do not describe this diagnostic as
production File Transfer or storage integration.
