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

The state byte is the publication commit marker. Before the first accepted
state, the tracker applies the stateless parser but does not independently
validate the raw state domain. `PRE_PROTOCOL`, `UNINITIALIZED`, `TRANSIENT`,
and stateless `INVALID` observations (including erased/random bytes,
out-of-domain raw states, and malformed or incomplete candidates) are
transient/not-yet-committed observations. A fully valid `READY` is accepted;
a fully valid `FAIL` is accepted as a terminal result. A fully valid
`MAKE_OBSERVED` or `BREAK_OBSERVED` before `READY` is invalid.

After a nonterminal state has been accepted, a raw state equal to that state
is accepted only when the complete snapshot is byte-identical. Any changed
body is a publication transient, regardless of whether its CRC is stale or
valid and regardless of its intermediate semantic contents. This covers the
window in which the next state's body and CRC are visible while the old state
byte remains visible. A higher state is parsed strictly only after its state
byte is observed: it must be the next state, except terminal `FAIL` may be
published from any nonterminal state. Lower states and skipped nonterminal
states are invalid. Once `BREAK_OBSERVED` or `FAIL` is accepted, the snapshot
is immutable and any mutation is invalid.
