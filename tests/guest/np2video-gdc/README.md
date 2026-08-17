# NP2 video fixture 7A.3d

This directory contains the Ubuntu-native GDC drawing-command reference
fixture. The guest enters the reviewed 640x400 analog 16-color mode, clears
all four page-0 graphics planes, and draws only the specified frame, lines,
and yellow rectangle through the vendor-backed GDC `CSRW`, `VECTW`, and
`VECTE` commands. It does not create a golden descriptor and it is not part
of ESP graphics CI.

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
```

The BMP is diagnostic-only and must be reviewed visually. The observed GDC
framebuffer CRC is provisional reference evidence; it is intentionally not
stored here, in another golden descriptor, or in CI.
