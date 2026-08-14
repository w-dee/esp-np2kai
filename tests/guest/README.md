<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Reproducible PC-9801 guest fixtures

This directory contains the guest-side fixtures for Step 3.5.  The reviewed
NP2TEST Stage 1 milestone provides a deterministic, filesystem-less image
artifact and a small executable CPU/memory test core.

## NP2TEST Stage 1 fixture

`np2test/layout.json` defines an FD1232-compatible raw disk:

- 77 cylinders
- 2 heads
- 8 sectors per track
- 1024 bytes per sector
- 1,261,568 bytes total

The Stage 1 image contains a deterministic 1,024-byte 8086 IPL at the first
sector and thirteen reviewed CPU/memory tests.  It initializes the reviewed
stack and result-v1 block, publishes `RUNNING`, records the Stage 1 results,
publishes terminal `PASS`, and then halts.  The remaining disk bytes are
reserved and zero-filled; later device and firmware tests are intentionally
outside this milestone.

The raw attachment extension is selected as `.hdm` after the pinned SDL
NP2kai reference run.  `.ima` and `.img` also accepted the same bytes in the
candidate check, but `.hdm` is the explicit 1.25 MB/1024-byte raw convention
used for this fixture.  The neutral build output remains
`np2test-fd1232.image`; the selected runtime extension does not change bytes.

The golden image's SHA-256 is pinned in `np2test/layout.json` and
`np2test/golden/SHA256SUMS`:

`3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3`

The golden is a reproducibility contract for this Stage 1 implementation;
runtime lineage evidence is recorded in `np2test/reference-validation.md`.

The result-v1 wire contract is complete in
`protocol/result-v1.md`; this IPL is its first executable producer.

The build contract uses the Ubuntu 24.04 `nasm=2.16.01-1build1` package and
CPython 3.12 standard-library execution. The semantic versions are checked at
build time and the actual Python patch version is recorded in the generated
manifest. The Ubuntu package `.deb` is not separately SHA-pinned here; the
canonical CI image/container is the intended artifact pin.

Build and verify the reviewed fixture with:

```sh
make -C tests/guest/np2test check
python3 tools/guest/test_np2test.py
```

The complete runner-independent validation entrypoint is:

```sh
make -C tests/guest/np2test ci-check
```

It requires NASM 2.16.01 and CPython 3.12, and uses `LC_ALL=C` and `TZ=UTC`.
It performs syntax checks, host-side tests, independent double-build
reproducibility checks, and golden equality.  Golden mismatches fail; CI never
promotes or overwrites the golden automatically.  This Step 3.5a-4 check
validates source, toolchain, and artifact structure only; it does not execute
an emulator.  Guest runtime execution belongs to Step 4.

Generated files are written below `build/guest/`, which is ignored by Git.
