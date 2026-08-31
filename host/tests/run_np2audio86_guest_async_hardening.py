#!/usr/bin/env python3
"""Mechanical fresh-process runner for the host-only 86R.4B hardening cases."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


CASE_MARKERS = {
    "claim-publication": (
        "TEST_FIRST_REPRODUCTION",
        "AUDIO86_GUEST_ASYNC_CUTPOINT_SUCCESS",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "live-event-pressure": (
        "AUDIO86_GUEST_ASYNC_DEFAULT_RING_ABI",
        "AUDIO86_GUEST_ASYNC_EVENT_FULL",
        "AUDIO86_GUEST_ASYNC_EVENT_FULL_GUEST_TIME_PAUSED",
        "AUDIO86_GUEST_ASYNC_EVENT_WRAP",
        "AUDIO86_GUEST_ASYNC_BYTE_WRAP",
    ),
    "failure-cleanup": (
        "AUDIO86_GUEST_ASYNC_CUTPOINT_FAILURE",
        "AUDIO86_GUEST_ASYNC_FAILURE_CLEANUP",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "reset-fatal": (
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_RESET_PRODUCER",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "supplemental": (
        "AUDIO86_GUEST_ASYNC_BYTE_FULL",
        "DATA_RUN_RESERVATION_ORDER",
        "AUDIO86_GUEST_ASYNC_DATA_RUN_SPLIT_PRESSURE",
        "AUDIO86_GUEST_ASYNC_POST_RESET_CONTINUATION",
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_EVENT_PRODUCER",
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_BYTE_PRODUCER",
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_CONSUMERS",
        "AUDIO86_GUEST_ASYNC_FIRST_ERROR_IMMUTABLE",
    ),
}


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in values:
            raise RuntimeError(f"duplicate marker: {key}")
        values[key] = value
    return values


def last_marker(output: str) -> str:
    markers = [line for line in output.splitlines() if "=" in line]
    return markers[-1] if markers else "NONE"


def run_case(binary: Path, case: str, timeout: float, repetition: int) -> dict[str, str]:
    command = [str(binary), "--case", case]
    try:
        completed = subprocess.run(command, capture_output=True, text=True,
                                   timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        print(
            f"AUDIO86_GUEST_ASYNC_HARDENING_FAILURE repetition={repetition} "
            f"scenario={case} exit=TIMEOUT last_marker={last_marker(stdout)}",
            file=sys.stderr,
        )
        raise RuntimeError("hardening timeout") from error
    if completed.returncode != 0:
        print(
            f"AUDIO86_GUEST_ASYNC_HARDENING_FAILURE repetition={repetition} "
            f"scenario={case} exit={completed.returncode} "
            f"last_marker={last_marker(completed.stdout)}",
            file=sys.stderr,
        )
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        raise RuntimeError("hardening child failed")
    values = parse(completed.stdout)
    for marker in CASE_MARKERS[case]:
        if values.get(marker) != "PASS":
            raise RuntimeError(f"{case}: missing {marker}=PASS")
    if values.get("AUDIO86_GUEST_ASYNC_HARDENING_CASE") != case:
        raise RuntimeError(f"{case}: case identity mismatch")
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    args = parser.parse_args()
    if args.repetitions <= 0 or args.timeout_seconds <= 0:
        raise RuntimeError("positive repetitions and timeout required")
    for repetition in range(1, args.repetitions + 1):
        for case in CASE_MARKERS:
            run_case(args.binary, case, args.timeout_seconds, repetition)
    for marker in sorted({item for values in CASE_MARKERS.values() for item in values}):
        print(f"{marker}=PASS")
    print("AUDIO86_GUEST_ASYNC_HARDENING_STRESS=PASS")
    print(f"AUDIO86_GUEST_ASYNC_HARDENING_REPETITIONS=PASS count={args.repetitions}")
    print("AUDIO86_GUEST_ASYNC_HARDENING_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
