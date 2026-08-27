#!/usr/bin/env python3
"""Run the accepted E1C cases in independent fresh processes."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools" / "emu"))

from validate_opngen_sustained_log import (  # noqa: E402
    DESCRIPTOR_PATH,
    load_descriptor,
    parse_logical_identity,
    validate_text,
)


CASES = (
    ("SYNTHETIC-LIGHT-30", "SYNTHETIC-LIGHT", 30, 5),
    ("SYNTHETIC-HEAVY-30", "SYNTHETIC-HEAVY", 30, 5),
    ("STRESS-30", "STRESS", 30, 5),
    ("STRESS-60", "STRESS", 60, 3),
)


def run_once(binary: str, descriptor: dict[str, object], case_name: str,
             profile: str, duration: int, timeout_seconds: float):
    try:
        result = subprocess.run(
            [binary, "--profile", profile, "--duration-seconds", str(duration)],
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or "") + (error.stderr or "")
        return None, f"timeout after {timeout_seconds}s", output
    output = result.stdout + result.stderr
    if result.returncode != 0:
        return None, f"exit={result.returncode}", output
    errors = validate_text(output, descriptor, case_name)
    if errors:
        return None, "; ".join(errors), output
    try:
        identity = parse_logical_identity(output)
    except ValueError as error:
        return None, str(error), output
    return identity, None, output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--descriptor", type=Path, default=DESCRIPTOR_PATH)
    parser.add_argument("--timeout-seconds", type=float, default=90.0)
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error("timeout-seconds must be positive")
    try:
        descriptor = load_descriptor(args.descriptor)
    except (OSError, ValueError) as error:
        print(f"E1C_FORMAL_RESULT=FAIL\ndescriptor: {error}", file=sys.stderr)
        return 1

    for case_name, profile, duration, repetitions in CASES:
        expected_identity = None
        passed = 0
        for iteration in range(1, repetitions + 1):
            identity, error, output = run_once(
                args.binary, descriptor, case_name, profile, duration,
                args.timeout_seconds)
            if error is not None:
                print("E1C_FORMAL_CASE profile=%s duration=%d pass=%d total=%d" %
                      (profile, duration, passed, repetitions))
                print("E1C_FORMAL_RESULT=FAIL")
                print("E1C formal failure case=%s iteration=%d: %s" %
                      (case_name, iteration, error), file=sys.stderr)
                print(output, file=sys.stderr)
                return 1
            if expected_identity is None:
                expected_identity = identity
            elif identity != expected_identity:
                print("E1C_FORMAL_CASE profile=%s duration=%d pass=%d total=%d" %
                      (profile, duration, passed, repetitions))
                print("E1C_FORMAL_RESULT=FAIL")
                print("E1C formal logical identity changed case=%s iteration=%d" %
                      (case_name, iteration), file=sys.stderr)
                return 1
            passed += 1
        assert expected_identity is not None
        print("E1C_FORMAL_CASE profile=%s duration=%d pass=%d total=%d" %
              (profile, duration, passed, repetitions))
        print("E1C_FORMAL_IDENTITY profile=%s duration=%d event_count=%s "
              "event_crc32=%s event_sha256=%s pcm_frames=%s pcm_bytes=%s "
              "pcm_crc32=%s pcm_sha256=%s" %
              (profile, duration,
               expected_identity["producer_event_count"],
               expected_identity["producer_event_crc32"],
               expected_identity["producer_event_sha256"],
               expected_identity["pcm_frames"], expected_identity["pcm_bytes"],
               expected_identity["pcm_crc32"], expected_identity["pcm_sha256"]))
    print("E1C_FORMAL_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
