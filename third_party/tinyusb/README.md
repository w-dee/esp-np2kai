# Vendored TinyUSB 0.21.0

This is a byte-preserved, minimal TinyUSB host closure for the independent
ESP32-P4 P4-NANO diagnostic at
`hardware/bringup/esp32-p4/p4-nano/tinyusb-host/`. It is not a TinyUSB
working-tree copy, submodule, or modified fork.

## Upstream identity

- URL: https://github.com/hathach/tinyusb.git
- Release tag: `0.21.0`
- Commit: `dae3f9a366bfcddbf9dcf1b48d7500286a849539`
- Commit subject: `docs: convert changelog release notes from RST to Markdown (#3746)`
- Local modifications to upstream files: none

The imported file allowlist is in `import-manifest.json`. `SHA256SUMS` records
the imported bytes, and `LICENSE-MAP.md` records the license/notice evidence.
The upstream `LICENSE` is preserved unchanged. The source files retain their
upstream TinyUSB MIT notices.

The closure deliberately contains only the TinyUSB common, FreeRTOS OSAL,
host, hub, HID host, and ESP32-P4 DWC2 HCD sources needed by this diagnostic.
The board adapter and the keyboard parser are project-owned and are outside
this vendor snapshot.
