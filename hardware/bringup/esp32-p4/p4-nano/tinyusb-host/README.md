# P4-NANO TinyUSB FS-over-HS-PHY host diagnostic

This is an independent diagnostic for the ESP32-P4 P4-NANO. It uses the
TinyUSB `0.21.0` snapshot under `third_party/tinyusb/` and the P4 HS DWC2
controller (rhport 1) with its HS/UTMI PHY while deliberately running the root
bus at full speed.

The purpose is to validate a real full-speed root bus, a full-speed hub child,
and a full-speed Boot HID keyboard without using the IDF USB Host stack. The
existing stock diagnostic at `../usb-host/` is a control and is intentionally
unchanged.

## Why FS-over-HS-PHY

ESP32-P4 has two USB controllers/PHY paths, but it is not a USB 2.0
transaction-translator host. `OTG_SINGLE_POINT=1` means a P4 root port cannot
perform TT split transactions for a full-speed device behind a high-speed hub.
The supported workaround is therefore:

```text
P4 HS DWC2 + HS/UTMI PHY, root speed forced to FS
    └── FS root bus
          └── FS hub
                └── FS HID keyboard
```

With the root bus speed set to FS, TinyUSB's DWC2 HCD does not enable
HCSPLT/Start-Split/Complete-Split. This is expected and is not a missing TT
feature. A true TT-dependent P4 path remains blocked and is not implemented by
this diagnostic.

## Build

Use ESP-IDF `v5.5.4`, target `esp32p4`, and the measured P4-NANO v1.x/rev 1.3
configuration. From the repository root:

```sh
cd hardware/bringup/esp32-p4/p4-nano/tinyusb-host
. $HOME/.espressif/v5.5.4/esp-idf/export.sh
idf.py -B build-tinyusb set-target esp32p4
idf.py -B build-tinyusb build
```

The project has no dependency on `usb_host_hid`, `usb_host_install`, or the
stock IDF USB Host Hub/TT path. The keyboard parser is the existing parser at
`../usb-host/main/hid_boot_keyboard.c`, compiled directly from that location;
it is not copied or modified here.

## Expected log markers

Startup must identify the fixed experiment:

```text
P4-NANO TINYUSB HOST START
P4-NANO TINYUSB VERSION: 0.21.0
P4-NANO TINYUSB RHPORT: 1
P4-NANO TINYUSB PHY: HS/UTMI
P4-NANO TINYUSB ROOT MODE: FS
P4-NANO TINYUSB SPLIT: DISABLED root-speed=FS
P4-NANO TINYUSB HOST INIT: PASS
```

Device logs include VID/PID, negotiated speed, root-hub port, and hub parent
address/port. A recognized `0853:0103` keyboard must report FS, Boot keyboard,
and produce raw press/release/modifier markers. The diagnostic emits a
software-sequence result and then deinitializes TinyUSB and the PHY in the same
FreeRTOS task/context. It never emits `PHYSICAL RESULT: PASS`; physical
acceptance remains a human decision.

TinyUSB's logical root-port/HPRT power state during teardown must not be
interpreted as removal of the board's Type-A 5 V. The P4-NANO VBUS path is
board-driven through DIO7003HEST5; this project does not invent or control a
VBUS GPIO and does not claim that HPRT teardown switches physical VBUS off.

## Physical test order

Do not change the current PC–CH343P–P4-NANO serial topology while building or
reviewing this project. Before the first physical TinyUSB test, a human must
disconnect the external hub and connect the known `0853:0103` keyboard directly
to the P4-NANO Type-A port. Only after explicit confirmation should this
diagnostic be flashed once and reset once.

Test 1 is direct FS keyboard: it must show root FS, the expected VID/PID and
FS speed, HID readiness, raw events, the short direct sequence, and cleanup.
Stop on any failure and save the serial log.

After Test 1's software result and cleanup are accepted, a human may restore
the existing external HS-capable hub plus keyboard topology for Test 2. Use
the same firmware/configuration. The child must still enumerate FS and record
its hub address/port through `tuh_bus_info_get`; no split transactions are
expected or permitted.

The current stock Stage 2 result remains the historical `BLOCKED_TT` control:
IDF's stock USB Host path reports that an FS child behind the hub cannot be
handled because TT is unsupported. That control result is not overwritten by
this TinyUSB diagnostic.

Tab5, S31, eFuse, forced flashing, USB PHY routing changes, PSRAM, and any
automatic physical topology change are out of scope.

## Measured results

The following results were measured on the Waveshare ESP32-P4-NANO rev v1.3
with ESP-IDF v5.5.4 and the same TinyUSB `0.21.0` firmware/configuration.

### Direct FS keyboard

- Human-confirmed topology: P4-NANO Type-A -> keyboard, no hub
- Root PHY/mode: HS/UTMI PHY, active root bus FS
- Keyboard: VID/PID `0853:0103`, speed FS
- HID: Boot keyboard, interface protocol keyboard, PASS
- Raw input: `A`, Shift+A, Enter, Up, Down, Left, Right make/break and Shift
  modifier transitions, PASS
- Human key correlation: PASS
- Software marker: `P4-NANO TINYUSB DIRECT RESULT: PASS`
- Cleanup: `P4-NANO TINYUSB CLEANUP RESULT: PASS`

### FS hub plus FS HID child

- Human-confirmed topology: P4-NANO Type-A -> external hub -> keyboard
- Root PHY/mode: HS/UTMI PHY, active root bus FS
- Hub: mounted at address `3`
- Keyboard: address `1`, VID/PID `0853:0103`, speed FS
- Topology: parent hub address `3`, parent hub port `4`
- HID: Boot keyboard, interface protocol keyboard, PASS
- Raw input: `A`, `1`, Space, Enter, Shift+A, Left Ctrl, Left Alt, F1, Up,
  Down, Left, Right make/break and modifier transitions, PASS
- Human key correlation: PASS
- Software marker: `P4-NANO TINYUSB HUB FS RESULT: PASS`
- Cleanup: `P4-NANO TINYUSB CLEANUP RESULT: PASS`

No HCSPLT, Start-Split, or Complete-Split path was needed. The measured result
is accurately described as TinyUSB Full-Speed Host over the P4 High-Speed PHY
with an FS hub and FS HID child. It is not P4 TT support, split-transaction
support, or a 480 Mbps host result. No panic, watchdog, or reset loop was
observed during either run. The firmware did not emit a physical PASS marker;
the physical acceptance above includes the human key confirmation.
