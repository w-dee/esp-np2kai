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

The final raw filename extension remains deliberately unselected.  The
builder uses `np2test-fd1232.image` as a neutral development artifact while
`.hdm`, `.ima`, `.img`, and `.fdd` remain candidates for the later NP2kai and
independent-emulator attachment check.

Build and verify the foundation with:

```sh
make -C tests/guest/np2test check
python3 tools/guest/test_np2test.py
```

Generated files are written below `build/guest/`, which is ignored by Git.
