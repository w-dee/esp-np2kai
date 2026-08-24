# NP2K control-v1

The keyboard fixture publishes a 64-byte little-endian block at physical
address `0x27fc0`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `NP2K` |
| 4 | 2 | version = 1 |
| 6 | 2 | header size = 26 |
| 8 | 2 | block size = 64 |
| 10 | 2 | flags = 0 |
| 12 | 4 | suite ID = `0x4e504b31` |
| 16 | 4 | build ID = `0x00010001` |
| 20 | 1 | expected make = `0x1d` |
| 21 | 1 | expected break = `0x9d` |
| 22 | 1 | observed make |
| 23 | 1 | observed break |
| 24 | 2 | failure reason |
| 26 | 30 | reserved, zero |
| 56 | 4 | CRC-32/ISO-HDLC over bytes `[0,56)` |
| 60 | 1 | state, written last |
| 61 | 3 | reserved, zero |

States are monotonic: `UNINITIALIZED=0`, `READY=1`, `MAKE_OBSERVED=2`,
`BREAK_OBSERVED=3`, and `FAIL=4`. `BREAK_OBSERVED` and `FAIL` are terminal and
immutable. Failure reasons are `NONE=0`, `PRECONDITION_DATA_READY=1`,
`STATUS_OVERFLOW=2`, `MAKE_MISMATCH=3`, and `BREAK_MISMATCH=4`.

A tracker treats a bad CRC while the raw state equals the last accepted state
as a publication transient. A higher state must have a complete valid block,
must not skip a state (except terminal FAIL), and a lower state is invalid.
