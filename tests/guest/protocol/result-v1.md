# NP2TEST result protocol v1

This document defines the logical contract for reporting a guest test result.
It deliberately does not freeze physical addresses, byte offsets, magic
values, or checksum polynomials.  Those numeric details are selected only
after the IPL assembly layout and memory map have been reviewed together.

## Result states

The result block has a monotonic state field:

- **ABSENT** — no result block was observed.  The guest did not reach the
  reporting phase, or the image could not be attached.
- **RUNNING** — the guest initialized the block and has not finished.
- **PASS** — every required test completed successfully.
- **FAIL** — at least one required test failed, or the block failed its
  integrity checks.

The guest writes all payload fields first, computes and writes the integrity
value second, and writes the final state last.  A host must not interpret a
`PASS` or `FAIL` state until the complete block and its integrity value have
been read consistently.

## Logical fields

The v1 block contains these fields, in an order to be fixed by the reviewed
binary layout:

1. protocol magic and version;
2. block size;
3. test-suite/build identifier;
4. current state;
5. total, completed, passed, and failed test counts;
6. first-failing test identifier (or an explicit “none” value);
7. bounded diagnostic data;
8. integrity checksum covering the defined payload.

All integer encodings, string encodings, maximum diagnostic length, and the
checksum algorithm are part of the reviewed binary layout, not host locale.
Unknown future fields must be ignored when the declared block size permits
them; a known v1 block that is shorter than its required fields is invalid.

## Host interpretation

The host-side verifier reports exactly one of `NOT_REACHED`, `RUNNING`,
`PASS`, `FAIL`, or `INVALID`.  A timeout while the block is `RUNNING` is a
diagnostic timeout, not a `PASS`; an invalid or torn block is `INVALID`.
The verifier must retain the raw block and emulator log alongside the
normalized result so a failure can be reproduced.

The physical placement and exact binary encoding are tracked in
`tests/guest/np2test/layout.json` once the next implementation milestone has
completed its assembly-layout review.
