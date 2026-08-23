#!/usr/bin/env python3
"""Validate the deterministic Step 7B.2d-1 guest fixture."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/guest/np2video-live"
BUILDER = ROOT / "tools/guest/build_np2video_stage2.py"
LAYOUT = FIXTURE / "layout.json"
GOLDEN = FIXTURE / "golden.json"
IMAGE_SIZE = 1_261_568


def build(output: pathlib.Path) -> str:
    subprocess.run(
        [sys.executable, str(BUILDER), "--layout", str(LAYOUT), "--output", str(output)],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return hashlib.sha256(output.read_bytes()).hexdigest()


def main() -> int:
    descriptor = json.loads(GOLDEN.read_text(encoding="utf-8"))
    source = (FIXTURE / "src/stage2.asm").read_text(encoding="ascii")
    required = (
        "mov word [es:10], 2",
        "call clear_planes",
        "call draw_bar",
        "call move_bar",
        "mov byte [es:CONTROL_STATE_OFFSET], STATE_SCENE_READY",
        "a800",
        "b000",
        "b800",
        "e000",
    )
    if any(item not in source for item in required):
        raise AssertionError("live fixture is missing a guest VRAM/state contract element")
    if "np2_presentation" in source or "memcpy" in source:
        raise AssertionError("fixture must not synthesize frames on the host")
    with tempfile.TemporaryDirectory(prefix="np2video-live-test-") as temp_name:
        output = pathlib.Path(temp_name) / "live.hdm"
        build(output)
        image = output.read_bytes()
        digest = hashlib.sha256(image).hexdigest()
        if output.stat().st_size != IMAGE_SIZE:
            raise AssertionError("live fixture image has the wrong size")
        if digest != descriptor["fixture_sha256"]:
            raise AssertionError(f"fixture SHA mismatch: {digest}")
        if image[510:512] != b"\x55\xaa" or image[1022:1024] != b"\x55\xaa":
            raise AssertionError("IPL signatures are invalid")
        stage2_size = int.from_bytes(image[1024 + 6:1024 + 8], "little")
        if image[1024:1028] != b"ST2V" or stage2_size < 8:
            raise AssertionError("stage2 header is invalid")
        if "--reproducibility" in sys.argv:
            second = pathlib.Path(temp_name) / "live-second.hdm"
            second_digest = build(second)
            if second_digest != digest or second.read_bytes() != image:
                raise AssertionError("live fixture build is not reproducible")
    print(f"np2video live fixture tests passed sha256={descriptor['fixture_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
