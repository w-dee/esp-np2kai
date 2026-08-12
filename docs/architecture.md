# Architecture

The project separates the portable PC-9801 emulation model from ESP32-P4
platform and board details. P4-NANO and TAB5 implementations must remain
outside the emulator core so that adding another ESP32-P4 board does not
require changing guest emulation logic.

## Conceptual layers

- **NP2/NP2kai emulator core**: guest CPU, memory, timers, I/O devices, disk
  formats, and emulation state. Imported core code should remain substantially
  C unless a compelling reason requires a change.
- **ESP32-P4 common platform layer**: FreeRTOS-facing scheduling, clocks,
  capability-aware memory, timing, and common services shared by P4 boards.
- **Board abstraction/HAL**: board discovery and hardware-specific operations
  for P4-NANO, TAB5, and future boards.
- **Display backend**: transfers a guest framebuffer to a physical display.
- **Audio backend**: maps emulated audio output to the board codec and output
  path.
- **Storage backend**: SD-card and other host storage access.
- **Input backend**: keyboard and mouse input.
- **Touch backend**: touch coordinates and gesture/input reporting.
- **Networking/C6 backend**: the ESP32-C6 connectivity path.
- **Debug/control plane**: the future UART command interface for test and
  diagnostic control.

The core exposes a guest framebuffer independently of the physical display
backend. It must not assume that the physical display is 1280x800: the
P4-NANO is 1280x800 while the TAB5 is 1280x720. Scaling, rotation, pixel
transfer, and buffering belong to the display/platform side.

## Realtime and language policy

New ESP32-P4-side C++ may use C++20. C++ exceptions and RTTI are disabled, and
`iostream` is not used. RAII, `constexpr`, `enum class`, `std::array`,
`std::span`, `std::string_view`, `std::optional`, and `std::unique_ptr` are
preferred where appropriate; `std::shared_ptr` is avoided.

There must be no heap allocation in realtime or hot paths such as guest CPU
execution, audio generation, video processing/transfer, or ISRs. Those paths
also avoid filesystem access and normal logging. Non-trivial global
constructors and static initialization-order dependencies are avoided.

ESP-IDF capability-aware allocation is used when needed, with internal RAM,
DMA-capable RAM, and PSRAM treated as distinct resources. Virtual dispatch is
acceptable at coarse HAL/backend boundaries, but not in inner CPU, audio, or
video loops or ISRs. C/C++ linkage boundaries are explicit with `extern "C"`
where required. Generated code is inspected and hot paths are benchmarked
instead of assuming an abstraction has zero cost.

## Debug/control plane

The future UART control plane is intended to support keyboard and mouse event
injection, emulator reset/pause/resume, SD-card file transfer, guest memory
read/write/patch, guest I/O port access, CPU register inspection, and
diagnostic tests. These features are documented here only and are not
implemented in the initial repository setup.

