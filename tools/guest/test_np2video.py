#!/usr/bin/env python3
"""Focused deterministic checks for the Step 7A.3a guest fixture builder."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
LAYOUT = ROOT / "tests/guest/np2video/layout.json"
BUILDER = ROOT / "tools/guest/build_np2video.py"
IMAGE_SIZE = 1_261_568
IPL_SIZE = 1024


def build(output: pathlib.Path) -> bytes:
    subprocess.run(
        ["python3", str(BUILDER), "--layout", str(LAYOUT), "--output", str(output)],
        check=True,
    )
    return output.read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reproducibility", action="store_true")
    args = parser.parse_args()
    layout = json.loads(LAYOUT.read_text(encoding="utf-8"))
    assert layout["scene"]["width"] == 640
    assert layout["scene"]["height"] == 400
    assert len(layout["scene"]["rows_text"]) == 25
    assert all(len(row) == 80 for row in layout["scene"]["rows_text"])
    assert layout["control_block"]["physical_address"] == 0x2A000
    assert layout["control_block"]["state_written_last"] is True

    with tempfile.TemporaryDirectory(prefix="np2video-test-") as temp_name:
        temp = pathlib.Path(temp_name)
        first = temp / "first.image"
        first_bytes = build(first)
        assert len(first_bytes) == IMAGE_SIZE
        assert first_bytes[510:512] == b"\x55\xaa"
        assert first_bytes[1022:1024] == b"\x55\xaa"
        assert first_bytes[IPL_SIZE:] == b"\0" * (IMAGE_SIZE - IPL_SIZE)
        digest = hashlib.sha256(first_bytes).hexdigest()
        sidecar = (first.parent / (first.name + ".sha256")).read_text(encoding="ascii")
        assert sidecar == f"{digest}  {first.name}\n"
        if args.reproducibility:
            second = temp / "second.image"
            second_bytes = build(second)
            assert second_bytes == first_bytes
            assert hashlib.sha256(second_bytes).hexdigest() == digest
    print("np2video fixture tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
