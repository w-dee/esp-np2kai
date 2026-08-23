# NP2 video Step 7B.2d-1 multi-frame fixture

This fixture is derived from the reviewed direct-VRAM path. Guest-side 8086
code enters the same 640x400 analog 16-color mode and moves an eight-byte-wide
high-contrast band through the B/R/E graphics planes. Each state is generated
by real writes to the NP2 graphics VRAM windows; the normal NP2 renderer then
produces the host RGB565 surface. No host-side framebuffer rewrite, text/font,
SDMMC, audio, or input dependency is involved.

The guest publishes the existing NP2V `SCENE_READY` state after mode and first
band initialization, then remains free-running until the runner's bounded
benchmark stop request is implemented. The compact oracle is the deterministic
band trajectory: byte positions 8..68, one byte per state, eight bytes wide,
with reversal at each endpoint.

Build and validate the image with:

```sh
make -C tests/guest/np2video-live test
make -C tests/guest/np2video-live reproducibility-check
make -C host test-video-runner-live-reference
```

The raw image and manifest are build artifacts and are not tracked. The golden
descriptor records the image identity and the first post-ready reference CRC;
animation correctness is proven by the multi-frame host regression.
