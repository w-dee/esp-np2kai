# Architecture

The project separates the portable PC-9801 emulation model from Espressif
platform and board details. ESP32-P4 is the initial implementation platform;
the architecture should also leave room for a future ESP32-S31 implementation
without making S31 current work.

## Dependency direction

The conceptual dependency direction is:

```text
NP2 / NP2kai emulator core
        |
        v
portable host services and interfaces
        |
        v
Espressif-common platform
        |
        +-- SoC-specific implementation: ESP32-P4
        |       |
        |       +-- board-specific implementation: P4-NANO
        |       +-- board-specific implementation: TAB5
        |
        +-- SoC-specific implementation: ESP32-S31
                |
                +-- board-specific implementation: S31 Korvo-1
                    future, not implemented
```

The emulator core depends only on portable emulator and host-facing interfaces.
The Espressif-common layer may depend on those interfaces, SoC implementations
may depend on the common layer, and board implementations may depend on their
SoC and common platform layers. ESP32-P4-specific or ESP32-S31-specific types,
APIs, peripheral assumptions, pin assignments, memory mechanisms, and driver
objects must not leak above their respective SoC implementation layers.

P4-NANO and TAB5 use the same ESP32-P4 SoC family, so their differences belong
primarily in board-specific implementations. S31 Korvo-1 represents a
different SoC family, so a board-only abstraction would not be sufficient for
future support.

## Future source organization

The following is a non-binding future organization sketch, not a current
implementation requirement:

```text
firmware/
└── components/
    ├── np2core/
    ├── np2host/
    └── platform/
        └── espressif/
            ├── common/
            ├── soc/
            │   ├── esp32p4/
            │   └── esp32s31/       # future
            └── boards/
                ├── p4_nano/
                ├── tab5/
                └── s31_korvo_1/    # future
```

These empty directories must not be created at this stage. Introduce the
organization incrementally as the implementation grows and concrete
dependency boundaries require it.

## Conceptual layers

- **NP2/NP2kai emulator core**: guest CPU, memory, timers, I/O devices, disk
  formats, and emulation state. Imported core code should remain substantially
  C unless a compelling reason requires a change.
- **Portable host services and interfaces**: coarse boundaries for display,
  audio, storage, keyboard/mouse input, touch input, connectivity, timing,
  useful memory allocation services, and debug/control. These interfaces
  describe emulator and guest operations rather than a specific SoC.
- **Espressif-common platform**: common scheduling, clocks, timing, logging,
  capability-aware resource handling, and other services shared across
  Espressif SoC implementations where appropriate.
- **SoC-specific implementation**: ESP32-P4 or a future ESP32-S31 implementation
  containing the target's CPU integration, memory mechanisms, peripheral
  access, interrupt details, and SoC-specific drivers.
- **Board-specific implementation**: board discovery, pin assignments,
  physical display and codec wiring, storage connections, input devices, and
  other operations specific to P4-NANO, TAB5, S31 Korvo-1, or another board.

## Portable host interfaces

The emulator should expose coarse platform-independent boundaries for:

- display output
- audio output
- storage and file access
- keyboard and mouse input
- touch input
- network or other connectivity
- timing and scheduling services
- memory allocation services where an abstraction is useful
- debug and control operations

The documentation defines these boundaries and dependency direction only. It
does not require a particular C++ class hierarchy.

The eventual emulator should not care whether a keyboard or mouse event comes
from physical USB HID, UART debug/control injection, or another board-specific
input path. The transport and device details remain below the portable input
boundary.

## Display and audio boundaries

The emulator side exposes a guest framebuffer independently of the physical
output backend. Upper layers must not assume MIPI-DSI, a particular LCD
controller, 1280x800, 1280x720, ESP32-P4 PPA, or any P4-specific display handle
type.

P4-NANO is currently planned with a 1280x800 physical display, suitable for a
2x display of a 640x400 guest framebuffer. TAB5 has different physical display
geometry. These are board-specific facts, not global emulator assumptions.
Scaling, rotation, pixel transfer, buffering, and display-driver selection
belong below the portable display boundary.

Similarly, emulated sound generation feeds a generic audio output boundary.
Codec type, I2S/TDM implementation, DMA details, and board wiring belong below
that boundary. No specific P4-NANO or TAB5 codec implementation should leak
into the emulator architecture.

## Storage, input, and connectivity

SD-card implementation details and USB-host implementation details remain below
portable storage and input interfaces. The core should not depend on whether
storage is provided by an SD card or another future backend.

Connectivity is described as a generic platform service. A particular ESP32-C6
path, radio, transport, or board wiring belongs in the relevant SoC or board
implementation rather than in the portable emulator layer.

## Realtime and language policy

New platform-side C++ may use C++20. C++ exceptions and RTTI are disabled, and
`iostream` is not used. RAII, `constexpr`, `enum class`, `std::array`,
`std::span`, `std::string_view`, `std::optional`, and `std::unique_ptr` are
preferred where appropriate; `std::shared_ptr` is avoided.

Imported NP2/NP2kai core code should remain substantially C. C/C++ linkage
boundaries are explicit with `extern "C"` where required.

There must be no heap allocation in realtime or hot paths such as guest CPU
execution, audio generation, video processing/transfer, or ISRs. Those paths
also avoid filesystem access and normal logging. Non-trivial global
constructors and static initialization-order dependencies are avoided.

Virtual dispatch is acceptable at coarse host or platform abstraction
boundaries, but not in inner CPU, audio, or video loops or ISRs. SoC-specific
virtual calls must not enter those hot paths.

## Memory policy

ESP-IDF capability-aware allocation is used when needed, with internal RAM,
DMA-capable RAM, and PSRAM treated as distinct resources. The emulator core
must not depend directly on ESP32-P4 memory APIs. SoC-specific allocation
capabilities are hidden below platform interfaces where necessary, and any
future ESP32-S31 memory model follows the same boundary.

## Debug/control plane

The debug/control plane remains SoC-independent at the command and protocol
level. Future commands such as the following describe guest or emulator
operations, not ESP32-P4-specific operations:

```text
input.key
input.mouse
emulator.reset
emulator.pause
emulator.resume
file.read
file.write
debug.memory.read
debug.memory.write
debug.io.read
debug.io.write
debug.cpu.get_registers
```

Conceptually, the protocol is separate from its transport:

```text
debug/control protocol
        |
        v
transport abstraction
   +-- UART
   +-- future USB transport
   +-- future network transport
```

The future guest/emulator commands listed above are documented here only and
are not implemented in the initial repository setup. The UART Control Plane
Base provides the first neutral transport/protocol/dispatcher path and is
verified under `esp-emu` v0.39.0 for the ESP32-P4 emulator environment. Its
protocol is independent of the configured console UART and of any
ESP32-P4-specific API. This emulator verification does not validate physical
P4-NANO or TAB5 UART paths.

The skeleton uses a bounded `@ESP-NP2 ` JSON-lines frame, a central command
dispatcher, and a separate configured-console-UART transport. The existing
`ESP-NP2KAI HELLO WORLD OK` marker remains the Hello World milestone marker;
`ESP-NP2KAI UART CONTROL READY` is the separate control-task readiness marker.
