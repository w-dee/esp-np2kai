#!/usr/bin/env python3
"""Test deterministic golden-header generation without copying golden values."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    descriptor = root / "tests/guest/np2video/golden.json"
    generator = root / "tools/guest/generate_np2video_golden_header.py"
    values = json.loads(descriptor.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="np2video-golden-header-") as name:
        directory = Path(name)
        first = directory / "first.h"
        second = directory / "second.h"
        for output in (first, second):
            subprocess.run(
                ["python3", str(generator), "--descriptor", str(descriptor),
                 "--output", str(output)], check=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first_bytes = first.read_bytes()
        if first_bytes != second.read_bytes():
            raise AssertionError("golden header generation is not deterministic")
        header = first_bytes.decode("ascii")
        for key in ("scene_id", "image_size", "width", "height", "bpp",
                    "pitch", "visible_bytes"):
            token = f"= {values[key]}U;"
            if token not in header:
                raise AssertionError(f"generated header is missing {key}")
        if f"0x{int(values['crc32'], 16):08x}U" not in header:
            raise AssertionError("generated header is missing crc32")
        expected_bytes = [
            f"0x{byte:02x}" for byte in bytes.fromhex(values["fixture_sha256"])
        ]
        if not all(byte in header for byte in expected_bytes):
            raise AssertionError("generated header is missing fixture SHA bytes")
    print("np2video golden header test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
