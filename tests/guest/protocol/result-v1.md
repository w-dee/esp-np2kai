<!-- SPDX-License-Identifier: BSD-2-Clause -->

# NP2TEST result protocol v1

This is the complete v1 logical and binary contract. The result block is a
fixed 128-byte, 16-byte-aligned object at physical address `0x29000`. Its
memory-map rationale is recorded in
[`../np2test/memory-map.md`](../np2test/memory-map.md).

All multi-byte integers are little-endian. The diagnostic field contains
UTF-8 bytes followed by zero padding; its length is recorded separately and
must not exceed 64 bytes.

## Wire layout

| Offset | Width | Field | Encoding / rule |
| ---: | ---: | --- | --- |
| `0x00` | 4 | magic | ASCII bytes `NP2T` (`4e 50 32 54`) |
| `0x04` | 2 | version | `u16le`, exactly `1` |
| `0x06` | 2 | header size | `u16le`, exactly `32` |
| `0x08` | 2 | block size | `u16le`, exactly `128` |
| `0x0a` | 2 | flags | `u16le`, currently `0` |
| `0x0c` | 4 | suite ID | `u32le`, stable test-suite identifier |
| `0x10` | 4 | build ID | `u32le`, fixture build identifier |
| `0x14` | 2 | total count | `u16le` |
| `0x16` | 2 | completed count | `u16le` |
| `0x18` | 2 | passed count | `u16le` |
| `0x1a` | 2 | failed count | `u16le` |
| `0x1c` | 2 | first failed ID | `u16le`, `0xffff` means none |
| `0x1e` | 2 | diagnostic length | `u16le`, maximum `64` |
| `0x20` | 64 | diagnostic data | UTF-8 bytes, zero-padded |
| `0x60` | 24 | reserved body | must be zero |
| `0x78` | 4 | checksum | CRC-32/ISO-HDLC, `u32le` |
| `0x7c` | 1 | state | `u8`, excluded from checksum and written last |
| `0x7d` | 3 | reserved tail | must be zero |

The checksum covers exactly byte range `[0x00, 0x78)`. The checksum field,
the state byte, and the reserved tail are not covered. CRC-32/ISO-HDLC uses
polynomial `0x04c11db7`, initial value `0xffffffff`, and final XOR
`0xffffffff`.

## Commit protocol

The state byte is deliberately outside checksum coverage. This makes the
commit write atomic with respect to the checksum: changing `RUNNING` to
`PASS` or `FAIL` does not invalidate a checksum over the fixed header/body.

1. The guest writes every checksum-covered field, including zero padding and
   reserved bytes.
2. The guest computes and writes the checksum at `0x78`.
3. The guest writes `RUNNING` (`1`) to `0x7c` last, committing a valid running
   block.
4. When tests finish, the guest updates the checksum-covered counts and
   diagnostic fields, recomputes the checksum, and writes `PASS` (`2`) or
   `FAIL` (`3`) to `0x7c` last.

The host reads the state, reads the complete 128-byte block, then reads the
state again. A state change during the read, a nonzero reserved byte, an
unsupported encoding, or a checksum mismatch is `INVALID`.

## State and host outcomes

The guest state values are:

- `UNINITIALIZED` (`0`) — no committed result block exists yet.
- `RUNNING` (`1`) — the guest has committed a valid block and is still
  executing tests.
- `PASS` (`2`) — all required guest tests completed successfully.
- `FAIL` (`3`) — one or more guest tests completed with a failing result.

`FAIL` is never used for a host-side integrity or harness problem. Host
normalization distinguishes these outcomes:

| Host outcome | Meaning |
| --- | --- |
| `HARNESS_ERROR` | Image attachment or emulator startup failed; outside the guest protocol. |
| `NOT_REACHED` | Attachment and guest start succeeded, but no valid committed result block was reached before the timeout/terminal condition. An all-zero `UNINITIALIZED` area is the normal evidence. |
| `RUNNING_TIMEOUT` | A valid `RUNNING` block was observed, but the guest did not finish before the timeout. |
| `PASS` | A consistent, valid `PASS` block was observed with completed required tests and zero failed tests. |
| `FAIL` | A consistent, valid guest `FAIL` block was observed. |
| `INVALID` | Bad magic/version, malformed layout, unsupported state, torn read, nonzero reserved bytes, or checksum failure. |

The raw result block, emulator log, attachment command, and normalized host
outcome must be retained together for diagnosis. An unsupported future
version is `INVALID`; v1 does not guess at a newer wire format.

## Future extensions

Version 1 requires exactly 128 bytes and zero reserved bytes. Future protocol
versions may append fields only under a separately reviewed versioned contract;
they must not change the v1 offsets, state commit rule, or checksum meaning.
