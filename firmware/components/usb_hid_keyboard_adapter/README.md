# USB HID keyboard adapter

Pure C++20 translation from D0 Boot Keyboard transitions to
`np2_keyboard_input::Event`. The adapter owns report-boundary aggregation for
the two Ctrl aliases (`E0/E4`) and two GRPH aliases (`E2/E6`). It has no USB
Host, ESP-IDF, FreeRTOS, bridge, vendor, or heap dependency.
