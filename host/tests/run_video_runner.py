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
    parser.add_argument("--fixture-id", default="np2video-7a3a-text")
    parser.add_argument("--scene-id", type=int, default=1)
    parser.add_argument("--multi-frame", type=int, default=0)
    return parser


def _emit(data: bytes, stream) -> None:
    if data:
        stream.buffer.write(data)
        stream.flush()


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if (args.timeout <= 0 or args.expect_result != "REFERENCE_READY" or
            args.scene_id < 0 or args.scene_id > 65535 or
            args.multi_frame < 0 or args.multi_frame > 64 or not args.fixture_id):
        print("supervisor: invalid runner options", file=sys.stderr)
        return 64
    command = [args.runner, args.fixture, "--fixture-id", args.fixture_id,
               "--scene-id", str(args.scene_id)]
    if args.bmp is not None:
        command += ["--dump-framebuffer", args.bmp]
    if args.multi_frame:
        command += ["--multi-frame", str(args.multi_frame)]
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
    if args.multi_frame:
        summaries = [line for line in stdout.splitlines()
                     if line.startswith(b"NP2VIDEO_MULTIFRAME ")]
        if len(summaries) != 1:
            print("supervisor: missing multi-frame summary", file=sys.stderr)
            return 5
        fields = dict(token.split(b"=", 1) for token in summaries[0].split()[1:])
        try:
            updates = int(fields[b"updates"])
            distinct = int(fields[b"distinct"])
            generation = int(fields[b"generation"])
            errors = int(fields[b"framebuffer_errors"])
        except (KeyError, ValueError):
            print("supervisor: malformed multi-frame summary", file=sys.stderr)
            return 5
        if (updates < args.multi_frame or distinct < 16 or generation < 1 or
                errors != 0):
            print("supervisor: multi-frame proof failed", file=sys.stderr)
            return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
