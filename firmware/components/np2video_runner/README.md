# ESP np2video runner

`NP2_VIDEO_PROFILE` is a local ESP32-P4/esp-emu profile for the approved
Step 7A.3a `tests/guest/np2video/golden.json` scene. The profile uses
`np2video_runner`, configures effective EXTMEM=8 MiB, keeps the framebuffer in
PSRAM, and validates the final headless RGB565 snapshot against a generated
golden header.

The generated header is build output only. Both it and the flash-preparation
tool read `golden.json` directly; Python tooling never parses the generated C
header. The video image substitutes for the existing read-only `np2test` raw
partition at `0x110000`/`0x134000` only in the dedicated local test path. No
second fixture partition, BMP support, or physical display support is added.

Run the local validation from the repository root:

```sh
bash tools/emu/test-np2video-golden.sh
```

The script preserves its separate build, merged flash image, and esp-emu log
under a temporary `NP2VIDEO_RUN_ROOT` for review.
