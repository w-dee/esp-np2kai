#!/usr/bin/env python3
"""Focused command-line validation checks for video_runner."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    runner = pathlib.Path(sys.argv[1])
    no_args = subprocess.run([str(runner)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if no_args.returncode != 64:
        raise SystemExit(f"expected usage status 64, got {no_args.returncode}")
    with tempfile.NamedTemporaryFile(prefix="np2video-cli-", delete=False) as file:
        file.write(b"not a 1261568-byte fixture")
        invalid_path = pathlib.Path(file.name)
    try:
        invalid = subprocess.run(
            [str(runner), str(invalid_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    finally:
        invalid_path.unlink()
    if invalid.returncode != 66:
        raise SystemExit(f"expected invalid-fixture status 66, got {invalid.returncode}")
    print("video_runner CLI validation tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
