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

## Vendored NP2kai core boundary

Step 3 establishes a fixed, byte-preserved NP2kai snapshot under
`third_party/np2kai/`. It is not a Git submodule or an upstream working-tree
copy. The machine-authoritative import definition is
[`import-manifest.json`](../third_party/np2kai/import-manifest.json); its
upstream SHA, not the observed `wx_alpha` branch name, is the reproducibility
anchor. The vendor-local
[`README.md`](../third_party/np2kai/README.md) is the generated human-readable
summary.

The imported tree is an inspected, byte-preserved source set for the initial
baseline. The allowlist alone is not a general dependency-closure guarantee:
different feature configurations may require different sources. The current
Step 4 Ubuntu-native configuration has nevertheless proven its bounded
closure: 124 vendor translation units plus 10 host translation units compile,
link as a relocatable unit, and leave zero project/non-system unresolved
symbols.

The baseline intent is:

- i286 CPU
- guest video core included; host display backend absent
- guest sound state retained at the source-inspection-selected minimum;
  sound generation and host audio backend absent
- frontend absent, network disabled, and minimum single-thread operation

Desktop frontends, i386/HAXM and other optimized cores, network/VST domains,
optional FM/MAME sound generators, optional Cirrus/TGUI graphics backends, and
ROM or disk-image assets are outside this boundary. The exact machine-readable
exclusion list is maintained in the manifest rather than duplicated here.

Optional implementation domains are excluded where they are not required by
the Step 3 baseline. Exact file membership is authoritative in the manifest;
Step 4 compiler/linker evidence may show that the candidate source set needs to
change. Any such change requires an explicit allowlist update, provenance and
license-evidence review, deterministic regeneration, verification, and human
review. This source-set policy does not establish optional device runtime
support.

Host contracts and adapters remain outside the vendor tree. The Step 4
configuration validates compile/link and bounded startup/CPU execution through
the Ubuntu-native headless path. Display, audio, input integration, broader
software compatibility, target/physical storage integration such as physical
microSD/SDMMC and real media, ESP32-P4 firmware integration, and hardware
validation remain separate later scopes. At the Step 4 milestone, storage was
limited to the read-only tracked FD1232 FDD image path used to boot the formal
Stage-1 golden; Step 6A later adds a separate emulator FATFS/VFS path without
changing that oracle.

## Ubuntu-native execution boundary

The formal NP2TEST Stage-1 guest publishes a 128-byte `result-v1` memory block.
The Ubuntu-native execution pipeline is:

```text
formal NP2TEST guest
        |
        v
result-v1 memory block
        |
        v
result-v1 parser
        |
        v
deterministic execution controller
        |
        v
Ubuntu-native headless runner orchestration
```

The parser validates the guest block and the controller owns returned-slice
budgets and normalized outcomes. A separate Python external supervisor bounds
the host process and wall clock. The deterministic pre-running/running limits
(512 and 4096 returned slices) measure guest/protocol progress; the supervisor's
30-second limit handles a non-returning `pccore_exec(FALSE)` call or another
process-level failure. These timeout domains are intentionally distinct.

The current formal golden run is measured evidence: 13/13 tests pass with
result-v1 CRC `0x58f5b827`. First protocol evidence was observed at returned
slice 202 and terminal `PASS` at returned slice 203; those observations are not
architectural guarantees.

## ESP32-P4 headless core integration

The implemented firmware dependency graph is:

```text
main
  +-- uart_control_transport
  +-- np2memoryprobe -> np2core
  +-- np2fixtureprobe -> np2fixture -> np2host
  +-- np2test_runner -> np2core + np2host + np2fixture
                         + shared Stage-1 configuration/parser/controller
```

The dedicated runner directly owns the one-shot NP2 lifecycle through
`pccore_init()`, `pccore_reset()`, `pccore_exec()`, and `pccore_term()`. The
validated 124-TU NP2core is connected through the ESP32-P4 np2host boundary,
with PSRAM external BSS support. External BSS placement is provided by
ESP-IDF linker facilities rather than vendor-source ESP-specific attributes.

The formal NP2TEST fixture is stored in a dedicated read-only NOR partition.
Its raw access path uses mmap/read-only DOSIO and validates the vendor FDD/XDF
path; it remains an independent oracle and is not replaced by FATFS. The
formal Stage-1 configuration, result-v1 parser, and execution controller are
shared with the native headless oracle, while the ESP32-P4 runner remains a
platform-side adapter around the core lifecycle. Step 6A additionally mounts
FATFS/WL and exercises the same NP2 path through generic VFS/DOSIO.

## Validation-layer boundary

Ubuntu-native execution validates portable core behavior and is the fastest
reference layer. It is not the target ESP32-P4 firmware. `esp-emu` validates
actual ESP32-P4 firmware and integration, while real ESP32-P4 hardware is the
layer for unsupported peripherals, real timing, performance, and board
transport. The firmware integration is implemented and validated under the
non-formal reduced emulator profile. Formal `EXTMEM=13` runtime evidence is
tracked separately. Real-hardware evidence is now scoped rather than wholly
pending: P4-NANO UART/SDMMC and production Host-to-Device File Transfer are
validated at 1.5 Mbps, while display, input, audio, TAB5, removal/durability,
and other unqualified paths remain pending.

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

## Video framebuffer and presentation boundary

Step 7A implements one mutable guest framebuffer owned by `scrnmng`. It uses
RGB565LE data, dynamic guest geometry, and an ESP32-P4 implementation backed by
external PSRAM. Step 7B.1 implements the portable presentation boundary below
the renderer and above any physical display policy:

```text
NP2 / NP2kai renderer
        |
        v
scrnmng single mutable guest framebuffer
  RGB565LE
        |
        | synchronous borrowed view during scrnmng_surfunlock()
        v
portable np2_presentation publisher
  two caller-owned slots
        |
        v
immutable ACQUIRED frame
        |
        v
future platform / SoC presentation consumer
        |
        v
future board display policy
        |
        v
future physical panel
```

The first four stages are implemented and verified by Ubuntu-native tests and
ESP32-P4 / FreeRTOS / `esp-emu` probes. The last three stages are the future
physical-output path; the ESP32-P4 consumer probe validates ownership and
concurrency behavior, not a display driver or panel.

The portable publisher has exactly two caller-owned presentation slots in the
tested contract. It uses the states `FREE`, `WRITING`, `PENDING`, and
`ACQUIRED`, with one producer and one consumer. The producer reclaims or
replaces `PENDING` first, otherwise uses `FREE`, and never waits for or
overwrites `ACQUIRED`. The consumer holds at most one `ACQUIRED` frame, acquires
the latest pending frame, and releases it back to `FREE`. This is the
latest-frame-wins policy: pending frames may coalesce, while an acquired frame
remains immutable. The publisher performs no per-frame allocation, has no
unbounded queue, and never gives the consumer the guest framebuffer pointer.

### Synchronous scrnmng publication contract

The implemented hook ordering is:

```text
renderer completes framebuffer writes
        -> synchronous publish hook
        -> scrnmng unlock state cleared
        -> Step 7A surface_update_sequence increment
        -> pending resize may execute
```

`SCRNMNG_PUBLISH_VIEW` contains a borrowed guest pointer. It is valid only for
the synchronous callback and must not be retained. The callback may perform a
bounded copy, but must not wait for the consumer, allocate, perform normal
frame logging, or call `scrnmng` reentrantly. The publisher copies compact
visible RGB565LE rows into its caller-owned slots; its published frame has a
packed pitch of `width * 2`.

### Framebuffer ownership and lifetime

The guest framebuffer and presentation slots are separate storage domains:

- the guest framebuffer is one mutable renderer-owned `SCRNSURF`, has dynamic
  geometry, may be reallocated on resize, and is stored in ESP32-P4 PSRAM;
- presentation storage consists of two independent caller-owned buffers with
  disjoint backing ranges and an independent lifetime from the guest buffer.

The tested 640x400 RGB565LE contract uses 512000 bytes per presentation slot.
This is a geometry-derived test value, not a universal allocation constant.
An already acquired frame remains valid across a guest resize because it owns
its copied slot. A subsequent publication carries the new source generation.
If a geometry exceeds the fixed slot capacity, publication may fail or drop
without becoming a guest renderer failure.

### Presentation sequences and concurrency

The counters have distinct meanings:

- `surface_update_sequence` is the existing Step 7A scrnmng completion/update
  counter;
- `published_sequence` advances for successful presentation publications;
- `coalesced_count` counts pending presentation frames replaced before acquire;
- `dropped_count` counts submissions that were not published.

None of these is a physical refresh, VSYNC, or displayed-frame count. A future
physical backend may add a separate presented/displayed sequence.

The publisher is a C11 single-producer/single-consumer implementation using
32-bit atomic slot states with acquire/release publication ordering. The
ESP32-P4 / `esp-emu` evidence shows that these 32-bit state atomics are
lock-free in the tested ESP-IDF v5.5.4 toolchain/runtime. That result is not an
automatic guarantee for every future architecture. The accepted bounded
32-bit lease-token wrap domain is an implementation safety limit, not a
physical display guarantee.

## Physical display boundary

Guest framebuffer geometry must not depend on P4-NANO's planned 1280x800
panel, TAB5's 1280x720 panel, PPA, MIPI-DSI, LCD driver type, or board pin
assignments. PPA and MIPI-DSI/LCD APIs are available in ESP-IDF v5.5.4, but this
project has not integrated or validated them. They belong downstream of the
immutable presentation frame.

The current P4-NANO policy is a planned exact nearest-neighbor 2x mapping:

```text
640x400 guest framebuffer -> 1280x800 physical output
```

It is not implemented or validated. TAB5 requires a separate 1280x720
viewport/scaling decision. Neither board has physical display, panel timing,
tearing, bandwidth, PPA, or MIPI-DSI validation. ESP32-S31 / S31 Korvo-1
remains a future portability target; Step 7B runtime evidence is limited to
Ubuntu x86_64 and ESP32-P4 RISC-V / `esp-emu`.

## Audio boundary

Emulated sound generation feeds a generic audio output boundary. Codec type,
I2S/TDM implementation, DMA details, and board wiring belong below that
boundary. No specific P4-NANO or TAB5 codec implementation has been integrated
or validated.

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
ESP32-P4-specific API. The configured-console path is also validated on the
P4-NANO onboard CH343P route at 1.5 Mbps; TAB5 remains unverified.

The skeleton uses a bounded `@ESP-NP2 ` JSON-lines frame, a central command
dispatcher, and a separate configured-console-UART transport. The existing
`ESP-NP2KAI HELLO WORLD OK` marker remains the Hello World milestone marker;
`ESP-NP2KAI UART CONTROL READY` is the separate control-task readiness marker.

## UART Binary Data Plane v1

The Binary Data Plane v1 is verified under ESP-IDF v5.5.4 and esp-emu v0.39.0
for the ESP32-P4 emulator environment. This verification covers the complete
bidirectional byte-path suite over esp-emu UART-TCP. The production
Host-to-Device subset used by File Transfer is additionally hardware-validated
on P4-NANO at 1.5 Mbps; this is not a claim that the complete bidirectional
suite or TAB5 transport has been physically qualified.

The responsibilities remain separate:

```text
UART RX -> control_stream
              +-- text   -> control_plane
              +-- binary -> binary_data_plane

control_plane -- JSON lifecycle/test commands --> binary_data_plane

JSON responses and binary frames -> common raw machine writer
                                  -> uart_control_transport
```

`control_stream` owns the bounded TEXT/START_ZERO/BINARY_COLLECT/
BINARY_DISCARD multiplexer. `binary_data_plane` owns portable framing, CRC,
transfer identity, sequence/offset state, generic endpoint lifecycles, and stop-and-
wait behavior. `uart_control_transport` composes these components with the
configured ESP-IDF console UART. The portable components do not contain board
pin policy or ESP32-P4-specific protocol types.

The binary envelope is exactly:

```text
00 00 COBS(decoded-frame) 00
```

The byte-stream layer also reserves exactly four consecutive NUL bytes as
`TRANSPORT_SYNC` (`00 00 00 00`). This token is detected by `control_stream`,
below the JSON/control framing parser. On completion it discards partial text
and COBS candidates and returns the stream to Text/seeking state. A longer NUL
run remains in that synchronization interval until a non-NUL byte arrives.
`TRANSPORT_SYNC` has no storage, reset, UART-configuration, or
TransferManager-session side effects; recovery of a higher-level binary or
file-transfer session is separate.

The host handshake is `TRANSPORT_SYNC`, then a uniquely identified
`protocol.hello`, then validation of its matching response. The READY line is
diagnostic only, so a host may attach after boot and need not have observed it.
The reserved token is collision-free for the current binary envelope because
COBS emits no NUL in the encoded body: one frame has at most two leading and
one trailing NUL, and directly adjacent frames have at most three at their
boundary. Valid JSON control frames contain no raw NUL.

The decoded Binary Data Plane v1 frame layout is:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | magic = `0x4e 0x42` (`NB`) |
| 2 | 1 | Binary Data Plane version = 1 |
| 3 | 1 | frame type |
| 4 | 2 | flags |
| 6 | 2 | header length = 28 |
| 8 | 4 | transfer ID |
| 12 | 4 | sequence |
| 16 | 8 | absolute offset |
| 24 | 2 | payload length |
| 26 | 2 | status / NACK reason |
| 28 | N | payload |
| 28+N | 4 | CRC-32/ISO-HDLC |

All multibyte fields are explicitly little-endian; native struct layout is not
serialized. The fixed header is 28 bytes, the maximum DATA payload is 1024
bytes, the maximum decoded frame is 1056 bytes, the maximum COBS body is 1061
bytes, and the maximum complete wire frame is 1064 bytes. CRC covers the
decoded header and payload and excludes the CRC field and COBS delimiters.
Future incompatible field widths, offsets, or bounds require a new Binary Data
Plane version rather than a silent v1 change.

The v1 frame types are `DATA = 0x01`, `ACK = 0x02`, and `NACK = 0x03`. JSON
remains responsible for lifecycle/control commands, which currently are:

```text
binary.test.rx.begin
binary.test.tx.begin
binary.transfer.status
binary.transfer.abort
```

The advertised additive capability is `binary.data-plane.v1`. These commands
remain deterministic transport tests; application services attach through the
generic bounded endpoint contract. v1 permits one active transfer total and
uses stop-and-wait.
Transfer IDs identify binary sessions and are independent of JSON request IDs.

ACK advances the receiver's acknowledged sequence and offset. A matching NACK
for the outstanding DATA frame causes immediate retransmission of that same
frame without advancing transfer progress or reapplying its payload CRC.
Timeout and NACK retries use one shared retransmission budget. A mismatched
NACK sequence or offset aborts the active session as protocol desynchronization;
it does not create a NACK loop, rewind, or arbitrary resume. Duplicate
Host-to-ESP32 DATA is applied once and receives an idempotent ACK.
The most recent successfully completed Host-to-Device transfer also retains a
bounded identity record for its exact final DATA frame. An identical replay
receives the same final progress ACK without reopening, consuming, finishing,
or committing the endpoint again; the record is replaced when a new transfer
successfully begins.

The integration test directly verifies deterministic 64 KiB transfers in both
directions, exact bytes, per-frame and whole-transfer CRC, Host-to-ESP32
duplicate DATA, corrupted CRC and `BAD_CRC` recovery, host-generated
Device-to-Host NACK retransmission of an identical DATA frame, and false-NUL
delimiter recovery followed by a successful JSON `system.ping`. Timeout retry,
shared retry-budget exhaustion, and mismatched-NACK abort are implemented v1
semantics established by the source, but are not claimed as separately
injected runtime cases by this test.

## File Transfer Base

The `file-transfer.v1` capability is verified under esp-emu v0.39.0 using this
composition:

```text
control_plane -> file_transfer -> storage <- storage_ram
                      |
                      v
             binary_data_plane -> UART
```

`file_transfer` owns canonical logical paths, metadata commands, ranges,
write replacement policy, and file/transport summaries. `storage` is a neutral
streaming interface with stat/list/read/write-session operations.
`storage_ram` is a deterministic test backend with a 256 KiB arena, 32 entries,
a 192 KiB per-file limit, one read session, and one staged write session.
Staged writes become visible only after commit; abort preserves an existing
target. No filesystem handle or ESP-IDF storage type crosses the interface.

The JSON commands are `file.stat`, `file.list`, `file.read.begin`,
`file.write.begin`, and `file.transfer.status`. Paths are absolute UTF-8 logical
paths rooted at `/`; traversal, repeated separators, controls, backslashes,
oversized paths/components, and malformed UTF-8 are rejected. Listing uses an
unsigned UTF-8 byte-order name cursor that must be one valid component; `/`,
`\`, controls, DEL, `.`, and `..` are not valid inside that cursor. Listing
also has a count limit of 1..16 and a 768-byte response budget. Nonempty
reads/writes use Binary Data Plane bytes; zero-length operations complete
synchronously without a transfer ID.

The RAM backend and esp-emu UART-TCP path are the completed File Transfer Base
foundation. Step 6A also verifies a persistent `StorageFatfs` backend under
the same neutral contract. Production `StorageSdmmc` Host-to-Device writes are
hardware-validated on P4-NANO physical SD at 1.5 Mbps with raw and bounded
`zero-rle-v1`; W=1 remains the default and W=2 bounded Go-Back-N is opt-in.
This scoped result does not cover every real-media lifecycle or TAB5.

## Step 6A persistent storage architecture

Step 6A is the completed, hardware-independent persistent storage integration
using emulator-supported SPI-NOR plus ESP-IDF FATFS/Wear Levelling (WL). It is
distinct from the physical microSD/SDMMC implementation validated separately
under Step 6B; Step 6B remains partial with removal and durability work
outstanding.

The common validation image uses the current approved 8 MiB flash envelope.
This is the common esp-emu validation envelope, not a claim that every future
board has only 8 MiB of flash or that it is the physical maximum of P4-NANO:

```text
nvs           [0x009000, 0x00F000)  size 0x006000
phy_init      [0x00F000, 0x010000)  size 0x001000
factory       [0x010000, 0x210000)  size 0x200000
np2test       [0x210000, 0x344000)  size 0x134000  type/subtype 0x40/0x01  read-only
storage       [0x344000, 0x800000)  size 0x4BC000
total common flash envelope: 0x800000 / 8 MiB
```

The factory application partition is now 2 MiB. It was expanded to provide
headroom for future production growth, especially OSD/UI and display-side
functionality, while preserving the common 8 MiB validation envelope. The
additional 1 MiB was taken from the internal FATFS test/storage partition;
the read-only `np2test` fixture size was deliberately preserved. This FATFS
partition is primarily a development and validation fixture, not the intended
final large user-media store. The production P4-NANO profile now uses
microSD/SDMMC for real user files; other boards and media remain separate
integration scopes.

The offsets and sizes originate from `firmware/partitions.csv`. Operational
tooling validates the generated ESP-IDF partition table through the shared
geometry helper rather than treating duplicated offsets as authoritative.

The measured storage fixture under this geometry is:

```text
storage partition  0x4BC000 / 4,964,352 bytes
WL sectors         1212
WL metadata        0xC000
plain FAT volume   0x4B0000
FAT type           FAT12
sector size        4096
sectors/cluster    1
cluster size       4096
usable data        1193 clusters
clean allocated    320 clusters
clean free         873 clusters
```

These are measured current test-fixture values, not product API guarantees.
Phase 6 freshly measured all relevant profiles below the generated 2 MiB
factory size; the largest was `storage-provider` at `0xFC2D0` (1,032,912
bytes), leaving `0x103D30` (1,064,240 bytes). This is evidence from the
current codebase, not a stable ABI limit or a promise that future firmware
will remain below 1 MiB.

The mounted namespace is:

```text
/persist
├── files          File Transfer visible namespace
├── fixtures       private NP2 fixture namespace
└── .np2-staging   private replacement/staging state
```

File Transfer logical `/` maps to `/persist/files`. The `fixtures` and
`.np2-staging` namespaces are therefore not exposed through File Transfer;
this is namespace isolation, not arbitrary filesystem access.

The storage dependencies are deliberately layered:

```text
File Transfer
    -> storage::Storage
    -> StorageFatfs
    -> ESP-IDF VFS/FATFS/WL
    -> SPI NOR

NP2 / FDD / XDF
    -> DOSIO
    -> generic POSIX/VFS backend
    -> mounted filesystem
```

`np2host`, `np2fixture`, and `np2test_runner` do not depend on
`storage_fatfs`. The Step 6A application composition owns the FATFS mount and
provides the generic VFS path to the runner. This keeps a future SDMMC FATFS
mount, or another mounted filesystem, out of NP2 host/core-specific logic.

The raw and VFS disk sources have distinct roles. The raw source is the
read-only `np2test` partition at logical `./np2test-fd1232.hdm`. The VFS source
is `/persist/fixtures/np2test-fd1232.hdm`, mapped through DOSIO to the same
logical NP2 path. Both use the same FDD/XDF, NP2 CPU, Stage-1 guest, and
result-v1 parser/controller. The raw source remains a permanent deterministic
independent oracle.

Step 6A File Transfer service capacity remains 2 MiB. Routine CI uses a
bounded 262145-byte upload/download workload, while the routine persistence
case uses 4097 bytes to cross the 4 KiB FAT cluster boundary. A clean image
using the approved 2 MiB-app / 8 MiB validation geometry accepts
a new 2 MiB file upload, readback, and ranged reads.
That protocol maximum does not guarantee same-size replacement of a full 2 MiB
file on the internal FATFS fixture: replacement temporarily needs both the
existing target and a staging file. The extended replacement/rollback path is
therefore validated with a 419-cluster (`0x1A3000`) payload and a 35-cluster
safety margin. This capacity distinction is not a firmware bug and does not
lower the 2 MiB protocol limit. These File Transfer writes do not make the NP2
guest disk writable: the NP2 disk path remains read-only through DOSIO.

The NoSpace contract is geometry-derived rather than a fixed 4 MiB prefill:
the 1 MiB request needs 256 clusters, the target is left with 255 free
clusters, and under the current approved geometry, the measured result derives
a 618-cluster
(`0x26A000` / 2,531,328-byte) prefill. Begin/preallocation fails with
`NO_SPACE` before any payload frame (`payload_frames=0`); the pre-existing file
remains intact, staging cleanup succeeds, and the endpoint recovers. The
`0x26A000` value is this measured result, not a permanent source-of-truth
constant.

Validated persistent behavior also includes metadata/stat, listing/pagination,
ranged reads, UTF-8, FAT case-insensitive collision handling, FAT-invalid
names, staged replacement, abort preservation, actual FAT NoSpace mapping,
cross-process persistence, and high-address physical flash persistence. The
high-address invariant is semantic: the marker must be at physical offset
`>= 0x400000` and inside the storage partition. Phase 6 observed `0x68D000` as
evidence, not as a permanent required address.

During Step 6A.1 development, `StorageFatfs` direct underlying POSIX
`pread`/`pwrite` operations of 4096 bytes were observed to hang under the
pinned esp-emu/IDF combination, while 512-byte operations passed. The current
512-byte bound is therefore an emulator-observed local compatibility
workaround. It is not a FATFS requirement, a physical ESP requirement, or a
proven exact failure threshold. Step 6A.3 separately established that generic
DOSIO VFS reads can perform 4096-byte POSIX reads successfully. A separate
formal P4-NANO physical-SD NP2TEST run validates the production DOSIO/VFS/SD
path; it does not turn the esp-emu-specific 512-byte observation into a
physical constraint.

The strongest Step 6A.4 source-independence proof used a temporary 8 MiB image
copy. Only the raw partition's first `0x1000` bytes at offset `0x210000` were
poisoned. Its SHA changed from
`3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3` to
`92ecf3e62e8ea67a2e618b58cf57e6a8db0f7a4ca5891507d53854285fe108f`, while
the FATFS storage region remained unchanged. The VFS-backed NP2 run still
completed 13 tests with 13 passed, 0 failed, and CRC `0x58f5b827`. This is
evidence of source independence, not a claim of arbitrary filesystem or guest
software compatibility.

The current provider split is:

```text
File Transfer -> storage::Storage -> StorageFatfs -> VFS/FATFS/WL -> SPI NOR
NP2 -> FDD/XDF -> DOSIO -> POSIX/VFS -> mounted FATFS -> SPI NOR

File Transfer -> storage::Storage -> StorageSdmmc -> VFS/FATFS -> SDMMC
NP2 -> FDD/XDF -> DOSIO -> POSIX/VFS -> mounted FATFS -> SDMMC
```

No FATFS-specific logic is introduced into NP2 core/host boundaries.

Machine-readable JSON and binary frames use the raw UART driver path so
stdout/VFS newline conversion cannot change arbitrary binary bytes. Normal
human-readable logging remains on stdout/VFS. The common machine writer holds
the stdout FILE lock while flushing pending stdout data and issuing the raw
UART write, preventing interleaving with normal stdout writers that participate
in the same FILE locking discipline:

```text
machine JSON/binary frame
        |
        v
flockfile(stdout)
        |
        v
fflush(stdout)
        |
        v
raw uart_write_bytes()
        |
        v
funlockfile(stdout)
```

This is not an absolute serialization guarantee against direct UART writes
outside the common machine writer, ROM output, panic/constrained-environment
output, or other paths that bypass normal stdout FILE locking. Future
project-owned machine-readable output should use the common machine writer
rather than introducing an independent raw UART path.
