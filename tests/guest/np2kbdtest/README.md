# NP2 keyboard hardware fixture

This directory contains the project-generated, BSD-2-Clause licensed
`np2kbdtest` fixture. It is a raw FD1232 image with a standalone 8086 IPL;
there is no DOS, filesystem, proprietary boot sector, or BIOS keyboard service.

The guest keeps interrupts disabled and polls the emulated PC-98 keyboard
controller directly:

1. `IN 0x43`, reject pre-existing data-ready (`0x02`) or overflow (`0x10`).
2. Publish control-v1 `READY` at physical `0x27fc0`.
3. Poll `0x43`, then `IN 0x41` and require make byte `0x1d`.
4. Publish `MAKE_OBSERVED`, wait for the next byte, and require break `0x9d`.
5. Publish `BREAK_OBSERVED`, then result-v1 PASS at physical `0x29000`.

Both control-v1 and result-v1 use CRC-32/ISO-HDLC with the state byte written
last. For control-v1, state zero is an uncommitted precommit window, and a
changed body (with either stale or newly valid CRC) while an old nonterminal
state remains visible is a publication transient; only the committed state
advances the tracker. For result-v1, `RUNNING` is likewise a live publication
state: dynamic counters, diagnostics, reserved bytes, and CRC are authoritative
only after terminal `PASS`/`FAIL` is committed. The fixture never flushes a
stale keyboard byte and never enables IRQs.

Build and verify it with:

```sh
make -C tests/guest/np2kbdtest check
```

The existing `tests/guest/np2test` 13/13 fixture is independent and unchanged.
