# Ubuntu-Native Host Validation

This directory contains the project-owned Ubuntu-native host contracts,
portable NP2kai closure probe, deterministic execution controller, and
headless runner used for the bounded Step 4 validation. The validated scope is
the imported minimum portable core plus the formal NP2TEST Stage-1 golden; it
does not claim complete PC-9801 compatibility or general software support.

## Main commands

Run the focused contracts with:

```sh
make -C host test-result-v1-parser
make -C host test-execution-controller
```

The full native validation is:

```sh
make -C host test-headless-runner-np2test
```

The parser test validates result-v1 decoding and terminal-block integrity. The
controller test validates deterministic returned-slice outcome and budget
semantics. The full command builds the configured native closure, boots the
tracked formal NP2TEST Stage-1 golden, and runs the permanent C
Ubuntu-native headless runner through the Python external supervisor. The
supervisor applies a separate 30-second wall-clock safety timeout; this is a
process-safety boundary, not a guest protocol timeout.

## Runner contract

The normalized outcomes and process statuses are:

| Outcome | Status |
| --- | ---: |
| `PASS` | 0 |
| `FAIL` | 1 |
| `NOT_REACHED` | 2 |
| `RUNNING_TIMEOUT` | 3 |
| `INVALID` | 4 |
| `HARNESS_ERROR` | 5 |
| usage error | 64 |
| input error | 66 |

`NOT_REACHED` means that terminal protocol execution was not reached before
the deterministic pre-running budget. `RUNNING_TIMEOUT` means that the guest
remained in `RUNNING` through the deterministic running budget.
`HARNESS_ERROR` denotes a host, infrastructure, or process-level failure.

The current returned-slice limits are:

- pre-running: 512 slices
- running: 4096 `RUNNING` observations/slices under controller semantics

These are deterministic slice budgets, not wall-clock durations.

## Current validation evidence

The canonical configured closure contains 124 vendor translation units and 12
shared host translation units, for 136 total; the relocatable link has zero
project/non-system unresolved symbols. The formal Stage-1 golden currently
reports:

```text
completed=13 passed=13 failed=0 stored_crc=0x58f5b827
```

In the current measured golden run, first protocol evidence was observed at
returned slice 202 and terminal `PASS` at returned slice 203. These are runtime
measurements, not ABI or architectural guarantees; `RUNNING` may be skipped
observationally.

## Step 7A.2 framebuffer inspection

The headless framebuffer snapshot contract hashes only the visible row-by-row
RGB565 little-endian bytes with CRC-32/ISO-HDLC. Its metadata includes dimensions,
bpp, pixel format, pitch, visible byte count, surface generation, and completed
surface-update sequence. The update sequence is not a guest frame or VSYNC count
and does not imply that pixel values changed.

The host-only `scrnmng_write_bmp()` diagnostic converts the same RGB565 surface
to an explicitly requested, uncompressed 24-bit bottom-up BGR BMP. BMP bytes are
never part of the canonical CRC identity, and the ESP32 build does not include
the BMP writer.

## CI contracts

The guest `np2test` CI job validates fixture syntax, reproducibility, and
golden-image equality. The separate `ubuntu-native-headless` job runs the
parser test, controller test, and full supervised native NP2TEST execution.
Neither job implies ESP32-P4 firmware or physical-board validation.

## Byte-exact Step 4 patches

`host/patches/np2kai/step4/*.patch` are byte-exact patch artifacts. Repository
attributes intentionally preserve their bytes and exclude them from ordinary
whitespace checking with `-text -whitespace`, because some imported upstream
source uses CRLF and patch context must match exact source bytes. Do not run
automatic EOL or whitespace normalization over these files.
