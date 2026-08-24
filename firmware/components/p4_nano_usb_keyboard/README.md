# P4-NANO USB Boot Keyboard producer

This optional production component owns the ESP-IDF USB Host/HID lifecycle for
one directly connected full-/low-speed Boot Keyboard. HID callbacks only copy
bounded reports into a static queue; the producer task performs D0 parsing,
D1.1 translation, and `KeyboardInputBridge::enqueue()` on the owner-safe
transport boundary.

The component is linked only by the real P4-NANO production runtime. Runtime
validation and synthetic keyboard validation do not start or link this
producer. USB is an optional peripheral: startup failure, malformed reports,
unsupported usages, transfer errors, and normal disconnects degrade to a
disabled keyboard path while NP2 continues.

There is one active keyboard, no hub/TT support, no repeat synthesis, no LED
synchronization, and no automatic B1 rearm. Queue overflow and bridge Full or
Quarantined results fail closed; source-local disconnect is used only when the
bridge remains healthy. Physical D2 validation is pending.
