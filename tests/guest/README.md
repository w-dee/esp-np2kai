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

The image is currently an empty, zero-filled artifact with the two PC-98 boot
signatures described by the layout.  It does not contain executable IPL bytes,
test code, or a golden image yet.  Those require a separate review of the
assembly layout, memory map, and the result block placement.

The raw filename extension remains unresolved. The builder uses
`np2test-fd1232.image` as a neutral development artifact while `.hdm`, `.ima`,
`.img`, and `.fdd` remain candidates. DOSBox-X geometry recognition is recorded
in `np2test/reference-validation.md`, but final selection still requires a
pinned NP2kai reference attachment check; geometry recognition alone does not
prove full boot compatibility.

The result-v1 wire contract is complete in
`protocol/result-v1.md`; executable guest code will consume it in a later
milestone.

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
