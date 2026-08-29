#!/usr/bin/env python3
"""Run one 86H.3 async process under an external liveness timeout."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout-seconds", required=True, type=float)
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error("timeout-seconds must be positive")

    try:
        result = subprocess.run(
            [str(args.binary)],
            capture_output=True,
            text=True,
            timeout=args.timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or "") + (exc.stderr or "")
        args.output.write_text(partial, encoding="utf-8")
        print("AUDIO86_ASYNC_LIVENESS=FAIL TEST_LIVENESS_FAILURE", file=sys.stderr)
        return 1
    output = result.stdout + result.stderr
    args.output.write_text(output, encoding="utf-8")
    if result.returncode != 0:
        print("AUDIO86_ASYNC_LIVENESS=FAIL exit=%d" % result.returncode,
              file=sys.stderr)
        return result.returncode or 1
    print("AUDIO86_ASYNC_LIVENESS=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
