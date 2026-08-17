# NP2 video fixture 7A.3a

This directory contains the source and reviewed layout for the Ubuntu-first
deterministic text scene. The raw image is generated under `build/` and is not
tracked in Git.

The scene is intentionally limited to white ASCII text on a black background:
80 columns, 25 rows, 8x16 characters, 640x400 pixels, hidden cursor, and no
graphics, blink, underline, or reverse attributes. The host runner must wait
for the NP2V `SCENE_READY` state before enabling rendering, then require a
post-ready framebuffer update before taking its reference snapshot.

Build and run the reference target from `host/`:

```sh
make test-video-runner-reference
make test-video-runner-reference-bmp
```

The second command retains a diagnostic BMP in the host build directory.
The result token is `NP2VIDEO_RESULT=REFERENCE_READY`; it is reference
generation, not golden validation.

The first scene has received human visual approval. Its approved fixture
SHA-256 is
`f4ae6584339cbdb94e80e6fb48f9a27724fee7a9f350668b618d33b2794c8eca`; the
authoritative fixture identity and canonical RGB565LE CRC (`0x0a280896`) are
recorded in `golden.json`. Run the separate regression with:

```sh
make -C host test-video-runner-golden
make -C host test-video-golden-checker
```

The golden descriptor is the sole tracked source of the approved fixture
identity and framebuffer metadata. The exact update sequence is only a
synchronization diagnostic: the checker requires a post-READY update in a
stable generation but does not freeze its numeric sequence value. BMP output
remains diagnostic-only and is not golden data.
