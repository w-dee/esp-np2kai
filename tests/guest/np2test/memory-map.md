<!-- SPDX-License-Identifier: BSD-2-Clause -->

# NP2TEST result memory-map evidence

The result block is owned by NP2TEST at physical address `0x29000` through
`0x2907f` (128 bytes, 16-byte aligned). The surrounding layout is explicit:

| Region | Physical range | Size | Purpose |
| --- | ---: | ---: | --- |
| IPL | `0x1fc00..0x1ffff` | 1,024 bytes | NP2kai FD1232 load/entry sector |
| Payload reserve | `0x20000..0x27fff` | 32 KiB | Future filesystem-less NP2TEST code/data |
| Stack | `0x28000..0x28fff` | 4 KiB | Future NP2TEST stack |
| Result | `0x29000..0x2907f` | 128 bytes | result-v1 block |

The ranges are adjacent but non-overlapping. The payload and stack reservations
are part of the v1 contract even though executable guest code is intentionally
not present in this milestone.

## Evidence

The pinned NP2kai source provides the following address facts:

- [`src/i286c/cpumem.h`](../../../third_party/np2kai/src/i286c/cpumem.h)
  describes `0x000000..0x0fffff` as main memory, maps text/graphics VRAM at
  `0xa0000`/`0xa8000`/`0xb0000`/`0xb8000`/`0xe0000`, and maps the ITF ROM at
  `0x1f8000..0x1fffff` in the extended address space (not within the current
  1 MiB validator address space).
- [`src/bios/biosmem.h`](../../../third_party/np2kai/src/bios/biosmem.h)
  places PC-98 BIOS data and work fields in the low `0x0401..0x05fa` area,
  including the boot-device byte at `0x0584`.
- [`src/bios/bios1b.c`](../../../third_party/np2kai/src/bios/bios1b.c)
  loads a 1,024-byte FD1232 IPL at `0x1fc00` and enters at `1fc0:0000`.
- [`src/bios/bios.h`](../../../third_party/np2kai/src/bios/bios.h) defines
  `BIOS_BASE` as `0xfd800`; [`src/bios/bios.c`](../../../third_party/np2kai/src/bios/bios.c)
  copies the simulated BIOS there and initializes firmware data in the
  `0xe8000..0xfffff` range. This is the conservative first-MiB firmware
  exclusion used by the layout.

The selected result range is therefore outside the conservative low-memory
BIOS reservation (`0x0000..0x0fff`), the IPL/payload/stack reservations, the
PC-98 text and graphics VRAM ranges, and the `0xe8000..0xfffff` firmware range.
The separate `0x1f8000..0x1fffff` ITF-ROM mapping is recorded as an extended
exclusion but is outside the current 1 MiB validator. It is ordinary main RAM
in the pinned NP2kai map and remains owned by NP2TEST for the diagnostic
lifetime.

The [PC-9800 Series Technical Data Book BIOS reference](https://vtda.org/docs/computing/NEC/PC-9800TechnicalDataBookBIOS%2BOCR_1992.pdf)
is retained as supporting historical evidence; the pinned NP2kai source takes
precedence for the emulator-specific map used by this fixture.
