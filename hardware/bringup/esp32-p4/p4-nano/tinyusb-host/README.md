# P4-NANO TinyUSB Host diagnostic

This is an independent diagnostic for the ESP32-P4 P4-NANO. It uses the
TinyUSB `0.21.0` snapshot under `third_party/tinyusb/`, the P4 HS DWC2
controller (rhport 1), and its HS/UTMI PHY. It validates both supported
diagnostic root modes:

```text
normal HS-capable root + direct FS keyboard
FS root over HS/UTMI PHY + FS Hub + FS keyboard
```

The purpose is to validate a real full-speed Boot HID keyboard, Hub and device
lifecycle behavior, and normal P4 HS-capable root configuration without using
the stock IDF USB Host stack. The existing stock diagnostic at `../usb-host/`
is a control and is intentionally unchanged.

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
HCSPLT/Start-Split/Complete-Split. This is the measured workaround path and is
not P4 TT support.

The separate normal HS-capable root profile requests `TUSB_SPEED_HIGH` and
leaves `HCFG_FSLS_ONLY` clear. A directly attached FS keyboard then negotiates
FS on that HS-capable root. There is no external Hub, so no TT or split
transaction is involved. The keyboard must not be described as operating at
480 Mbps.

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

Diagnostic profiles are selected with `-D TINYUSB_DIAG_PROFILE=N`:

```text
0  LEGACY
1  COLD_BOOT
2  KEYBOARD_HOTPLUG
3  HUB_HOTPLUG
4  REINIT
5  HS_ROOT_DIRECT_FS
6  DIRECT_FS_HOTPLUG
```

## Expected log markers

Startup identifies the selected root mode. The FS-root profiles emit:

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
and, where the selected profile supports it, produce raw press/release/modifier
markers. The diagnostic emits bounded software results and deinitializes
TinyUSB and the PHY in the same FreeRTOS task/context. It never emits
`PHYSICAL RESULT: PASS`; physical acceptance remains a human decision.

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

## Accepted measured results

The following results were measured on the Waveshare ESP32-P4-NANO rev v1.3
with ESP-IDF v5.5.4 and the same TinyUSB `0.21.0` firmware/configuration.

### Initial functional bring-up

The initial functional bring-up established direct FS HID operation and the
FS-root-over-HS-PHY Hub topology before lifecycle testing. Both configurations
used the known `0853:0103` Boot HID keyboard.

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

### Cold-boot robustness

With the Hub and keyboard preconnected:

```text
10/10 cold boots: PASS
Hub enumeration: PASS on every run
keyboard 0853:0103 enumeration: PASS on every run
HID ready: PASS on every run
clean shutdown: PASS on every run
```

### Keyboard hotplug

The keyboard hot-unplug/replug lifecycle was measured both behind the FS Hub
and directly on the root port. Each accepted lifecycle included clean device
unmount, HID unmount, application state clearing, re-enumeration, HID ready,
and raw-A press/release after reconnect where that profile provided the input
stage. Both topologies passed:

```text
Hub + keyboard hotplug lifecycle: PASS
direct-root FS keyboard hotplug lifecycle: PASS
```

### Hub hotplug

With the keyboard attached to the external Hub, Hub unplug/replug was accepted
with clean Hub and child unmount, no stale device or instance state, Hub and
child re-enumeration, HID ready, and resumed input:

```text
Hub hotplug lifecycle: PASS
```

### TinyUSB Host/PHY teardown and re-init

Profile 4 performed three bounded Host/PHY teardown and re-init rounds in one
firmware lifetime. Every round passed Host deinit, PHY deinit, inactive-state
verification, Host/PHY re-init, keyboard re-enumeration, HID ready, and stale
state cleanup:

```text
TINYUSB HOST REINIT ROUND 1: PASS
TINYUSB HOST REINIT ROUND 2: PASS
TINYUSB HOST REINIT ROUND 3: PASS
TINYUSB HOST REINIT LIFECYCLE: PASS
HID generations: 1 -> 2 -> 3 -> 4
```

### Normal HS-capable root plus direct FS device

Profile 5 requested a normal HS-capable root with `TUSB_SPEED_HIGH`,
`use_hs_phy=true`, and `HCFG_FSLS_ONLY` clear. The directly connected known
keyboard enumerated as FS at root `0:0`:

```text
TINYUSB NORMAL HS ROOT: PASS
DIRECT FS DEVICE NEGOTIATION: PASS
HS-ROOT DIRECT FS HID: PASS
root requested speed: HIGH
keyboard VID/PID: 0853:0103
keyboard negotiated speed: FS
parent: root 0:0
HID ready: PASS
```

This proves HS-capable root configuration plus direct FS-device negotiation;
it does not claim that the keyboard operated at HS or 480 Mbps.

### Cleanup and stability

Across the accepted robustness and root-mode runs:

```text
host=PASS
phy=PASS
inactive=PASS
stale=PASS
```

No panic, watchdog, reset loop, stale callback, use-after-unmount symptom, or
device/HID state leakage was observed.

Profile 4 final post-reinit raw-A and Profile 5 HS-root direct-FS raw-A were
not run because those diagnostic state machines auto-clean after their stable
stage and do not provide a bounded post-final-reinit input stage:

```text
Profile 4 raw-A: NOT RUN (diagnostic unsupported)
Profile 5 raw-A: NOT RUN (diagnostic unsupported)
```

These are not failures. Raw input was independently validated in the direct
FS and Hub functional/hotplug profiles.

### Historical timeout result

An earlier Hub replug failure with `HOTPLUG_TIMEOUT_MS=30000U` was invalidated
as a USB failure. It was not reproduced after changing the diagnostic operator
timeout to `HOTPLUG_TIMEOUT_MS=180000U`. The 180-second value is an
operator/human interaction timeout, not USB enumeration latency. Physical
cable-operation timestamps were not independently recorded, so UART intervals
containing human interaction must not be presented as pure USB latency.

### Known limitations

The stock ESP-IDF v5.5.4 result for:

```text
P4 HS root -> external HS Hub -> FS keyboard
```

remains `PARTIAL_BLOCKED_TT`. TinyUSB's accepted workaround is:

```text
P4 HS DWC2 + HS/UTMI PHY, root forced FS
    -> FS Hub -> FS keyboard
```

This diagnostic does not implement or prove P4 Transaction Translator support.
True P4 TT / HCSPLT split behavior remains `BLOCKED_BY_HARDWARE`. Mouse,
storage, concurrent production peripherals, PC-98 keyboard mapping, and
production USB input-path integration remain out of scope.
