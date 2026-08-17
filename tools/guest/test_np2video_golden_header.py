#!/usr/bin/env python3
"""Test deterministic golden-header generation without copying golden values."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    descriptors = (
        root / "tests/guest/np2video/golden.json",
        root / "tests/guest/np2video-gfx-vram/golden.json",
    )
    generator = root / "tools/guest/generate_np2video_golden_header.py"
    for descriptor in descriptors:
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
            expected_fixture = (
                f'static const char np2video_golden_fixture_id[] = '
                f'"{values["fixture_id"]}";'
            )
            if expected_fixture not in header:
                raise AssertionError("generated header is missing fixture_id")
            if "Generated from golden.json" not in header:
                raise AssertionError("generated header comment is not descriptor-neutral")
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
            boundary_cases = (
                ("A" * 63, True),
                ("A" * 64, False),
                ("", False),
                ("A" * 62 + "!", False),
            )
            for index, (fixture_id, accepted) in enumerate(boundary_cases):
                synthetic_descriptor = directory / f"boundary-{index}.json"
                synthetic_descriptor.write_text(
                    json.dumps({**values, "fixture_id": fixture_id}),
                    encoding="utf-8")
                result = subprocess.run(
                    ["python3", str(generator), "--descriptor",
                     str(synthetic_descriptor), "--output",
                     str(directory / f"boundary-{index}.h")],
                    check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True)
                if (result.returncode == 0) != accepted:
                    raise AssertionError(
                        f"fixture_id boundary acceptance mismatch: {len(fixture_id)}")
                if not accepted and "1-63" not in result.stderr:
                    raise AssertionError("fixture_id boundary error omits 1-63 contract")
    print("np2video golden header test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
