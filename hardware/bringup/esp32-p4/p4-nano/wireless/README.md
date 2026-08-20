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

The diagnostic uses only public ESP-Hosted APIs. The optional CP init event is
observed through the public event API. The one firmware-version query calls
`eh_host_sys_get_cp_fw_version()` exactly once; the compatibility wrapper is
the same underlying RPC and is therefore not called a second time.

## Expected UART markers

The application emits:

    P4-NANO WIRELESS HOST INIT: PASS/FAIL
    P4-NANO WIRELESS CONNECT: PASS/FAIL
    P4-NANO WIRELESS TRANSPORT: PASS/FAIL
    P4-NANO WIRELESS FW QUERY: PASS/UNSUPPORTED/FAIL
    P4-NANO WIRELESS RESULT: PASS/FAIL

The ESP-Hosted component's official logs are also evidence. Preserve lines
showing the GPIO54 reset, SDIO Slot 1 and pin configuration, card/function
initialization, received slave init, advertised capabilities, base transport,
and coprocessor identity. The application does not invent capability or INIT
markers when the public API does not expose them.

If transport succeeds but the old factory C6 does not support the read-only
firmware-version RPC, the correct result is `TRANSPORT PASS / FW QUERY
UNSUPPORTED`. An unrelated timeout, framing, reset, or transport error is a
transport failure and is reported separately.

## First physical run

Only the normal P4 UART path may be flashed. Do not flash the C6, erase the
full flash, use `--force`, change eFuses, or run a second connection attempt
automatically. A passive C6 UART0 RX capture may remain connected, but it must
not transmit any bytes.

Physical evidence:

    MEASURED ON REAL HARDWARE: FIRST RUN

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
    failure: no response to the single read-only RPC within 5000 ms
    P4-NANO WIRELESS RESULT: FAIL

The transport result is PASS. The firmware-version query is a separate Layer
2 failure, not an `UNSUPPORTED` result: the factory CP did not return a
response. The ESP-Hosted log also reported CP version `0.0.0`, a major-version
mismatch warning, and a missing `SDIO_MODE` TLV while continuing in compatible
streaming mode. No panic, watchdog, reset loop, SDIO CRC error, or card-init
failure was observed during this run. Wi-Fi scanning, AP association, DHCP,
BLE operation, repeated boots, throughput, and production integration remain
untested.
