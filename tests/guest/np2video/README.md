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
make test-video-runner-reference VIDEO_BMP=1
```

The second command retains a diagnostic BMP in the host build directory.
The result token is `NP2VIDEO_RESULT=REFERENCE_READY`; it is not a golden
frame approval.
