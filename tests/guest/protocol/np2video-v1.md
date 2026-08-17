# NP2V video fixture control block v1

The Step 7A.3a Ubuntu reference fixture publishes a 32-byte little-endian
control block at physical address `0x2a000`. It is a guest-to-host readiness
protocol only; it is not a video checksum or a golden-frame contract.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| `0x00` | 4 | magic | ASCII `NP2V` |
| `0x04` | 2 | version | `1` |
| `0x06` | 2 | header size | `16` |
| `0x08` | 2 | block size | `32` |
| `0x0a` | 2 | scene id | `1` |
| `0x0c` | 2 | diagnostic | `0` for this fixture |
| `0x0e..0x1e` | 17 | reserved | zero |
| `0x1f` | 1 | state | written last |

State values are `BOOTING=1`, `PROGRAMMING_VIDEO=2`, `SCENE_READY=3`, and
`ERROR=4`. The host accepts `SCENE_READY` only after observing the valid
header. It records the framebuffer generation and update sequence at that
point, then runs a bounded number of `pccore_exec(TRUE)` slices and requires a
new update sequence in the same generation.

The fixture image is a reproducible 1,261,568-byte raw 2HD image. Its IPL
clears text/attribute VRAM, writes an 80x25 ASCII scene into `0xa0000` and
`0xa2000`, and then enters `CLI; HLT; JMP`. No expected framebuffer CRC is
defined in this step.
