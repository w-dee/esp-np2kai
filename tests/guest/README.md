<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Reproducible PC-9801 guest fixtures

This directory contains the guest-side fixtures planned for Step 3.5.  The
first implementation milestone is intentionally small: it fixes the NP2TEST
disk contract and provides a deterministic, filesystem-less image artifact.

## NP2TEST foundation

`np2test/layout.json` defines an FD1232-compatible raw disk:

- 77 cylinders
- 2 heads
- 8 sectors per track
- 1024 bytes per sector
- 1,261,568 bytes total

The Stage 0 image contains a deterministic 1,024-byte 8086 IPL at the first
sector.  It initializes the reviewed stack and result-v1 block, publishes
`RUNNING`, records one passing smoke test, publishes terminal `PASS`, and then
halts.  The remaining disk bytes are reserved and zero-filled; payload tests
are intentionally not part of this milestone.

The raw attachment extension is selected as `.hdm` after the pinned SDL
NP2kai reference run.  `.ima` and `.img` also accepted the same bytes in the
candidate check, but `.hdm` is the explicit 1.25 MB/1024-byte raw convention
used for this fixture.  The neutral build output remains
`np2test-fd1232.image`; the selected runtime extension does not change bytes.

The golden image's SHA-256 is pinned in `np2test/layout.json` and
`np2test/golden/SHA256SUMS`.  The golden is a reproducibility check for this
Stage 0 implementation; runtime evidence is recorded in
`np2test/reference-validation.md`.

The result-v1 wire contract is complete in
`protocol/result-v1.md`; this IPL is its first executable producer.

The build contract uses the Ubuntu 24.04 `nasm=2.16.01-1build1` package and
CPython 3.12 standard-library execution. The semantic versions are checked at
build time and the actual Python patch version is recorded in the generated
manifest. The Ubuntu package `.deb` is not separately SHA-pinned here; the
canonical CI image/container is the intended artifact pin.

Build and verify the foundation with:

```sh
make -C tests/guest/np2test check
python3 tools/guest/test_np2test.py
```

Generated files are written below `build/guest/`, which is ignored by Git.
