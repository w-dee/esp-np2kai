# HID Boot Keyboard parser

This pure-C component is the single source of truth for the D0 eight-byte Boot
Protocol keyboard parser. It is consumed by production components in later
slices and by the P4-NANO standalone USB Host Stage 1/Stage 2 application.

The parser has no ESP-IDF, FreeRTOS, USB Host, or NP2 dependency. Its native
transactionality tests remain under
`hardware/bringup/esp32-p4/p4-nano/usb-host/native`.
