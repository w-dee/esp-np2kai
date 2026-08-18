# NP2 video fixture 7A.3c

This directory contains the Ubuntu-first direct-VRAM graphics reference
fixture. The guest enters the reviewed 640x400 analog 16-color mode, clears
the four page-0 graphics planes, writes a deterministic scene directly through
the B/R/G/E VRAM windows, and publishes NP2V `SCENE_READY` only after all
writes. It does not issue GDC drawing commands. The IPL is a 1024-byte
stage-2 loader; the flat 16-bit stage2 payload is loaded at physical `0x20000`
from disk offset `0x400`, checked with its `ST2V` header, and entered at
`2000:0008`.

The generated raw image and its manifest are build artifacts and are not
tracked. Build and test the fixture with:

```sh
make -C tests/guest/np2video-gfx-vram test
make -C tests/guest/np2video-gfx-vram reproducibility-check
make -C host test-video-gfx-vram-golden
```

The builder assembles stage2 first, derives its exact byte and sector counts,
passes those values to the IPL, and verifies zero padding and deterministic
rebuilds. A temporary loader-only proof is run from the host test suite before
the full graphics reference.

Run the Ubuntu host reference and retain a diagnostic BMP with:

```sh
make -C host test-video-runner-gfx-vram-reference
make -C host test-video-runner-gfx-vram-reference-bmp
```

The human-approved direct CPU-VRAM scene is authoritative through
`golden.json`, which records the deterministic raw HDM SHA-256 and the
RGB565LE framebuffer metadata and CRC32. The BMP remains diagnostic-only and
is never part of the golden identity. The fixture covers no GDC drawing
commands; the separate Step 7A.3d fixture documents the actual slave-GDC
drawing-command path.
