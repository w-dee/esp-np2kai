# NP2 video fixture 7A.3d

This directory contains the Ubuntu-native GDC drawing-command reference and
golden fixture. The guest enters the reviewed 640x400 analog 16-color mode,
clears all four page-0 graphics planes with CPU writes, and draws the visible
frame, lines, and yellow rectangle through the vendor-backed GDC `CSRW`,
`VECTW`, and `VECTE` commands. All visible scene geometry is GDC-generated.

Human visual review and pixel-exact mechanical review are complete. The
tracked `golden.json` is the authoritative source for the fixture SHA-256 and
framebuffer CRC32. The BMP remains diagnostic-only and is not golden data.

The IPL is the shared 1024-byte stage-2 loader. The flat 16-bit stage2 payload
is loaded at physical `0x20000` from disk offset `0x400`, checked with its
`ST2V` header, and entered at `2000:0008`. The stage2 publishes NP2V scene 3
only after the final bounded GDC completion check.

Build and structurally test the fixture with:

```sh
make -C tests/guest/np2video-gdc test
make -C tests/guest/np2video-gdc reproducibility-check
```

Run the Ubuntu reference and retain a diagnostic BMP with:

```sh
make -C host test-video-gdc-reference
make -C host test-video-gdc-reference-bmp
make -C host test-video-gdc-golden
```

The scene covers horizontal and vertical lines, reverse direction,
non-unit-slope diagonals, an explicit rectangle, and multiple bitplanes. It
does not cover VECTR, circles, fills, GRCG, EGC, or GDC text drawing. This is
not a claim of complete uPD7220/GDC compliance.
