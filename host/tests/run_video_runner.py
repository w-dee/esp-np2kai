#!/usr/bin/env python3
"""Run video_runner under a wall-clock safety boundary."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from typing import Optional, Sequence


RESULT_PATTERN = re.compile(rb"^NP2VIDEO_RESULT=REFERENCE_READY$")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--expect-result", default="REFERENCE_READY")
    parser.add_argument("runner")
    parser.add_argument("fixture")
    parser.add_argument("--dump-framebuffer", dest="bmp")
    return parser


def _emit(data: bytes, stream) -> None:
    if data:
        stream.buffer.write(data)
        stream.flush()


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if args.timeout <= 0 or args.expect_result != "REFERENCE_READY":
        print("supervisor: invalid runner options", file=sys.stderr)
        return 64
    command = [args.runner, args.fixture]
    if args.bmp is not None:
        command += ["--dump-framebuffer", args.bmp]
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        print(f"supervisor: cannot start runner: {error}", file=sys.stderr)
        return 5
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        _emit(stdout, sys.stdout)
        _emit(stderr, sys.stderr)
        print("supervisor: video runner timed out", file=sys.stderr)
        return 5
    _emit(stdout, sys.stdout)
    _emit(stderr, sys.stderr)
    results = [line for line in stdout.splitlines() if line.startswith(b"NP2VIDEO_RESULT=")]
    if results != [b"NP2VIDEO_RESULT=REFERENCE_READY"]:
        print(f"supervisor: unexpected result lines: {results!r}", file=sys.stderr)
        return 5
    if process.returncode != 0:
        print(f"supervisor: result/status mismatch: exit {process.returncode}", file=sys.stderr)
        return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
