<!-- SPDX-License-Identifier: BSD-2-Clause -->

# NP2TEST reference validation (Stage 0 milestone)

The Stage 0 image is intentionally filesystem-less. A DOSBox-X mount can
identify its geometry, but it cannot pass a DOS filesystem sanity check; the
image instead boots its 1,024-byte IPL, writes the result-v1 block, publishes
`PASS`, and halts. DOSBox-X is used here only for raw-media geometry; it does
not expose the guest result block to this host-side check or establish the
lineage-reference runtime result.

For each candidate extension, the image bytes were kept identical and the
following headless command was used with DOSBox-X 2024.03.01:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  dosbox-x -nogui -nomenu -machine pc98 -noautoexec \
  -defaultdir /tmp -exit \
  -c "imgmount a np2test-fd1232.<ext> -t floppy" -c "exit"
```

`.hdm`, `.ima`, `.img`, and `.fdd` were each identified as
`77/2/8 1024 bytes/sector`; each then reported the expected
`Sanity checks failed` for the non-filesystem image. No explicit `0x1fc0`
load-segment override is recorded for this geometry-only check. The NP2kai
source set uses its raw/XDF fallback, so the same raw bytes do not require a
D88 container.

## Pinned SDL NP2kai runtime evidence

The Stage 0 runtime gate was executed on the dedicated local reference
workspace documented in `README.local.md`:

| Item | Recorded value |
| --- | --- |
| Upstream repository | `https://github.com/AZO234/NP2kai.git` |
| Upstream commit | `e2dc9046aa5c786fcfbfb87e883457e421026e31` |
| Reference binary | `bin/sdlnp2kai-ref` |
| Binary SHA-256 | `e20e3323b0ada7c3e24565ecc39688aca533426fea3a6a3899cbd67cfc5d98d4` |
| Build | SDL2, PC-9801/i286, RelWithDebInfo, HAXM off |
| Guest memory symbol | `mem`, `UINT8[0x200000]`, GDB-visible |

The tracked golden was verified before execution:

```text
2b3cdfd9f780ac668ee33da4dcbab3c3250a68c623d8cdf5f9cfd2a8f1794994
```

The image bytes were copied to a temporary path with the selected `.hdm`
extension. SDL video/audio used dummy drivers, and `HOME` plus
`XDG_CONFIG_HOME` pointed to temporary directories. No `bios.rom`, `font.rom`,
MS-DOS, FreeDOS, ELKS, or user HDD image was supplied. The reference source's
internal/simulated BIOS fallback was used; the only startup warning was the
optional host-side `default.ttf` not being present.

GDB set a hardware watchpoint on `mem[0x2907c]`, the result-v1 state byte. The
observed transitions were:

```text
0xff -> 0x00  (reference BIOS/reset initialization and IPL clear)
0x00 -> 0x01  (RUNNING)
0x01 -> 0x02  (PASS)
```

At terminal PASS, GDB dumped `mem[0x29000..0x2907f]`:

| Field | Observed value |
| --- | --- |
| magic | `NP2T` |
| version / header / block | `1 / 32 / 128` |
| total / completed / passed / failed | `1 / 1 / 1 / 0` |
| first failed ID | `0xffff` |
| state | `PASS (2)` |
| stored checksum | `0x049c83b9` (little-endian bytes `b9 83 9c 04`) |
| computed CRC-32/ISO-HDLC over `[0x00,0x78)` | `0x049c83b9` |

Reserved bytes were zero. A second dump after the emulator remained running
in the terminal halt matched the first dump byte-for-byte (128-byte SHA-256:
`b1d1723d2596196600df253a568b8319a63a8188286343b0498cffba45f9b10c`).
The transient RUNNING state was sampled without changing the fixture.

The same bytes under `.ima` and `.img` also reached terminal `PASS` in the
candidate checks. `.hdm` is selected because it is the explicit NP2kai raw
1.25-MB/1024-byte convention for this fixture; the selection is recorded in
`layout.json` as `validated-np2kai-reference-attachment`. The golden bytes
were not modified. The `.fdd` alias was not used for the selection because the
bounded candidate run did not produce terminal-state evidence; this records no
incompatibility claim. This validation does not exercise the ESP32 firmware.
