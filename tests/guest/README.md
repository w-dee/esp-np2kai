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

The neutral raw artifact is tracked as a golden image, with its SHA-256 pinned
in `np2test/layout.json` and `np2test/golden/SHA256SUMS`.  The golden is a
reproducibility check for this Stage 0 implementation, not evidence that the
final NP2kai filename extension has been selected.

The raw filename extension remains unresolved. The builder uses
`np2test-fd1232.image` as a neutral development artifact while `.hdm`, `.ima`,
`.img`, and `.fdd` remain candidates. DOSBox-X geometry recognition is recorded
in `np2test/reference-validation.md`, but final selection still requires a
pinned NP2kai reference attachment check; geometry recognition alone does not
prove full boot compatibility.

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
