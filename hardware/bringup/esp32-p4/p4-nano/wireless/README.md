# ESP32-P4-NANO wireless companion diagnostic

This is the first active P4-side diagnostic for the factory ESP32-C6
wireless companion. It validates only the host reset/control path, the SDIO
transport, ESP-Hosted connection, and one optional read-only coprocessor
firmware-version RPC.

It does not scan for Wi-Fi, associate with an access point, obtain DHCP, use
BLE, flash the C6, or integrate wireless support into production firmware.
It also does not initialize an SD card. The SDIO peripheral is used only by
ESP-Hosted as the P4-to-C6 transport.

## Dependency and build

The project is pinned to:

    ESP-IDF v5.5.4
    espressif/esp_hosted 3.0.6

The generated image is explicitly configured for the ESP32-P4 v1.x revision
envelope (`CONFIG_ESP32P4_REV_MIN_100=y`), matching the P4-NANO's v1.3
silicon.

Build with the ESP-IDF environment selected explicitly:

    idf.py set-target esp32p4
    idf.py build

`esp_wifi_remote` is intentionally absent. Wi-Fi host support, BT host
support, OTA, heartbeat, and power-save features are disabled. The
constructor-based top-level ESP-Hosted initialization is disabled so that the
diagnostic owns the explicit `eh_host_init()` and
`eh_host_connect_to_slave()` sequence. ESP-Hosted's compile-time auto-init
machinery remains enabled because 3.0.6 requires its public header during the
build; individual feature initializers remain disabled.

ESP-Hosted 3.0.6 declares its OTA option as a hidden, default-enabled symbol.
`main/Kconfig.projbuild` exposes that option as a project-level policy knob so
`sdkconfig.defaults` can keep OTA explicitly disabled. This is configuration
only; no ESP-Hosted component source is modified.

## P4-NANO host configuration

The factory C6 remains unchanged. The P4 host is configured for the
ESP-Hosted MCU backend with SDIO Slot 1, 4-bit bus, and a 20 MHz ceiling:

    CLK  GPIO18
    CMD  GPIO19
    D0   GPIO14
    D1   GPIO15
    D2   GPIO16
    D3   GPIO17
    C6 reset/enable  GPIO54, active-low

GPIO54 is not configured or driven by `main.c`; reset ownership remains in
ESP-Hosted's SDIO backend. GPIO6 is not configured, and the P4 GPIO6 to C6
GPIO2 wake-up wire is not exercised by this minimal diagnostic.

The diagnostic uses only public ESP-Hosted APIs. The CP init event is observed
through the public event API and is part of the Layer-1 acceptance boundary.
The optional firmware-version query calls
`eh_host_sys_get_cp_fw_version()` exactly once when explicitly enabled; it is
disabled by default because the factory CP protocol predates request ID 350.
The compatibility wrapper is the same underlying RPC and is not called.

## Expected UART markers

The application emits:

    P4-NANO WIRELESS HOST INIT: PASS/FAIL
    P4-NANO WIRELESS CONNECT: PASS/FAIL
    P4-NANO WIRELESS TRANSPORT: PASS/FAIL
    P4-NANO WIRELESS CP INIT EVENT: PASS/FAIL
    P4-NANO WIRELESS TRANSPORT RESULT: PASS/FAIL
    P4-NANO WIRELESS FW QUERY: NOT RUN/PASS/UNSUPPORTED/FAIL

The ESP-Hosted component's official logs are also evidence. Preserve lines
showing the GPIO54 reset, SDIO Slot 1 and pin configuration, card/function
initialization, received slave init, advertised capabilities, base transport,
and coprocessor identity. The application does not invent capability or INIT
markers when the public API does not expose them.

The normal acceptance run does not issue the firmware-version RPC. It can be
enabled only with `CONFIG_P4_NANO_WIRELESS_RUN_FW_VERSION_QUERY=y`; this is
expected to fail against the factory CP because its protocol has no request ID
350. A firmware-query limitation does not change the Layer-1 transport result.

## Protocol-generation audit

The factory CP source at upstream commit
`83efce638beb0d59b641399df862b20e3aa12f36` defines:

    Req_WifiSetBandMode = 348
    Req_WifiGetBandMode = 349
    Req_Max = 350

There is no `Req_GetCoprocessorFwVersion` in that protocol. ESP-Hosted 3.0.6
assigns modern `Req_GetCoprocessorFwVersion = 350`.

The factory dispatcher logs an invalid request lookup for IDs outside its
request table and returns an error before the response is packed or
transmitted. This explains the observed host timeout without indicating an
SDIO transport failure.

## First physical run

Only the normal P4 UART path may be flashed. Do not flash the C6, erase the
full flash, use `--force`, change eFuses, or run a second connection attempt
automatically. A passive C6 UART0 RX capture may remain connected, but it must
not transmit any bytes.

Physical evidence:

    MEASURED ON REAL HARDWARE: FIRST RUN (pre-change query enabled)

    Board: Waveshare ESP32-P4-NANO
    ESP32-P4 revision: v1.3
    ESP-IDF: v5.5.4
    programming path: CH343P USB-UART
    transport: SDIO Slot 1, 4-bit, 20 MHz
    SDIO pins: CLK18 CMD19 D0=14 D1=15 D2=16 D3=17
    reset: GPIO54 active-low, owned by ESP-Hosted

    P4-NANO WIRELESS HOST INIT: PASS
    P4-NANO WIRELESS CP INIT EVENT: PASS reset_reason=0
    P4-NANO WIRELESS CONNECT: PASS
    P4-NANO WIRELESS TRANSPORT: PASS

    slave chip: ESP32-C6 (0x0d)
    capabilities: 0x0d
    advertised features: WLAN over SDIO, HCI over SDIO, BLE only

    P4-NANO WIRELESS FW QUERY: FAIL rc=-1 (ESP_FAIL)
    request: modern ID 350, no response after 5000 ms

The first run attempted one modern firmware-version RPC (request ID 350) and
received no response after 5000 ms. The source audit established that the
factory protocol has no such request, so this is classified as an unsupported
Layer-2 protocol generation, not a generic transport failure. The ESP-Hosted
log also reported CP version `0.0.0`, a major-version mismatch warning, and a
missing `SDIO_MODE` TLV while continuing in compatible streaming mode. No
panic, watchdog, reset loop, SDIO CRC error, or card-init failure was observed
during that run.

The corrected Layer-1-only image was used for the formal cold-boot acceptance
below. Each counted boot emitted `P4-NANO WIRELESS TRANSPORT RESULT: PASS`
without issuing ID 350.

Factory C6 Stage-0 evidence, captured separately, identified:

    project: network_adapter
    version: release/ng-v1.0.2-330-g83efce6
    ESP-Hosted-MCU Slave FW: 0.0.6
    transport: SDIO only

## Formal power-only cold-boot acceptance

    MEASURED ON REAL HARDWARE: FORMAL COLD-BOOT ACCEPTANCE

    Board: Waveshare ESP32-P4-NANO
    ESP32-P4 revision: v1.3
    ESP-IDF: v5.5.4
    P4 power: USB power-only cable
    P4 UART capture: external CH341 RX-only
    capture path: /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
    capture settings: 115200 8N1, no flow control
    UART wiring: P4 GPIO37 / UART0_TX -> CH341 RX; P4 GND -> CH341 GND
    CH341 TX/VCC/3V3/5V: not connected
    onboard CH343P USB data path: disconnected from host

    Formal cold-boot wireless transport: 5 / 5 PASS

    COLD BOOT #1: PASS
    COLD BOOT #2: PASS
    COLD BOOT #3: PASS
    COLD BOOT #4: PASS
    COLD BOOT #5: PASS

Every counted cycle contained one complete `ESP-ROM:esp32p4...` boot stream
with `rst:0x1 (POWERON)`, SDIO card/function initialization success, and:

    SDIO: 4-bit, 20 MHz
    slave chip: ESP32-C6 (0x0d)
    capabilities: 0x0d
    advertised features: WLAN over SDIO, HCI over SDIO, BLE only
    compatible streaming mode: established

Every counted cycle also emitted:

    P4-NANO WIRELESS HOST INIT: PASS
    P4-NANO WIRELESS CP INIT EVENT: PASS
    P4-NANO WIRELESS CONNECT: PASS
    P4-NANO WIRELESS TRANSPORT: PASS
    P4-NANO WIRELESS TRANSPORT RESULT: PASS
    P4-NANO WIRELESS FW QUERY: NOT RUN

No `msg_id=350` firmware-version request or 5000-ms firmware-query timeout
was observed. The accepted compatibility warnings were coprocessor version
`0.0.0`, a major-version mismatch, a missing `SDIO_MODE` TLV, and the old CP
lacking SDIO SW_AGGR while compatible streaming mode was selected.

The earlier occasional duplicate P4 ROM boot was observed with the onboard
CH343P USB data/control path connected to the host. Repeated power-only USB
boots with the external CH341 RX-only capture path produced clean single-ROM
boots. This establishes the isolation method used for the formal result, but
does not prove the exact CH343P DTR/RTS or host-enumeration mechanism.

Wi-Fi scanning, AP association, DHCP, BLE operation, throughput testing,
concurrent SD-card use with the C6 SDIO transport, and production integration
remain untested.
