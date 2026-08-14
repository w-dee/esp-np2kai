<!-- SPDX-License-Identifier: BSD-2-Clause -->

# NP2TEST reference validation (foundation milestone)

The foundation image is intentionally filesystem-less, so a DOSBox-X mount
can identify its geometry but cannot pass a DOS filesystem sanity check. This
is expected until the executable IPL and result-bearing test payload are
added.

For each candidate extension, the image bytes were kept identical and the
following headless command was used with DOSBox-X 2024.03.01:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  dosbox-x -nogui -nomenu -machine pc98 -noautoexec \
  -defaultdir /tmp -exit \
  -c "imgmount a np2test-fd1232.<ext> -t floppy" -c "exit"
```

`.hdm`, `.ima`, `.img`, and `.fdd` were each identified as
`77/2/8 1024 bytes/sector`; each then reported the expected
`Sanity checks failed` for the empty, non-filesystem image. The NP2kai source
set uses its raw/XDF fallback for unrecognized extensions, so the same raw
bytes do not require a D88 container. A runtime NP2kai desktop attachment
check remains pending until a pinned desktop build is available; this note is
not a claim that the ESP32 firmware has been exercised. The unresolved
condition is recorded as `pending-np2kai-reference-attachment-validation` in
`layout.json`.
