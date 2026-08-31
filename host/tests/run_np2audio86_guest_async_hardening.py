#!/usr/bin/env python3
"""Fresh-process, fail-closed runner for 86R.4Bb host hardening evidence."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


FORCED_CASE_MARKERS = {
    "claim-publication": (
        "TEST_FIRST_REPRODUCTION",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "live-event-pressure": (
        "AUDIO86_GUEST_ASYNC_DEFAULT_RING_ABI",
        "AUDIO86_GUEST_ASYNC_EVENT_FULL",
        "EVENT_FULL_SAMPLE_PUBLICATION",
        "AUDIO86_GUEST_ASYNC_EVENT_FULL_GUEST_TIME_PAUSED",
    ),
    "event-full-fatal": (
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_EVENT_PRODUCER",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "reset-fatal": (
        "AUDIO86_GUEST_ASYNC_FATAL_WAKE_RESET_PRODUCER",
        "AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT",
        "AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME",
    ),
    "producer-wake": ("AUDIO86_GUEST_ASYNC_FATAL_WAKE_PRODUCER_TO_WORKER",),
    "coordinator-wake": ("AUDIO86_GUEST_ASYNC_FATAL_WAKE_WORKER_TO_COORDINATOR",),
    "split-pressure-fatal": ("DATA_RUN_SPLIT_PRESSURE_FATAL",),
    "supplemental": (
        "AUDIO86_GUEST_ASYNC_BYTE_FULL",
        "DATA_RUN_RESERVATION_ORDER",
        "AUDIO86_GUEST_ASYNC_DATA_RUN_SPLIT_PRESSURE",
        "AUDIO86_GUEST_ASYNC_POST_RESET_CONTINUATION",
        "POST_RESET_STATE",
    ),
}

CUTPOINT_SUCCESS = tuple(f"cutpoint-success-{point}" for point in range(1, 12))
CUTPOINT_FATAL = tuple(
    f"cutpoint-fatal-{point}" for point in (2, 3, 5, 7, 8, 10, 11)
)
SCHEDULES = ("schedule-a", "schedule-b", "schedule-c", "schedule-d")

PRESSURE_ORACLE = {
    "AUDIO86_GUEST_ASYNC_PRESSURE_WORKER_TRACE_SHA256": "WORKER_APPLY_TRACE_SHA256",
    "AUDIO86_GUEST_ASYNC_PRESSURE_PRE_RESET_PCM_SHA256": "PRE_RESET_PCM_SHA256",
    "AUDIO86_GUEST_ASYNC_PRESSURE_FULL_PCM_SHA256": "FULL_REPLAY_PCM_SHA256",
    "AUDIO86_GUEST_ASYNC_PRESSURE_BYTE_SHA256": "PCM86_BYTES_SHA256",
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


def require_pass(values: dict[str, str], case: str, markers: tuple[str, ...]) -> None:
    for marker in markers:
        if values.get(marker) != "PASS":
            raise RuntimeError(f"{case}: missing {marker}=PASS")


def require_sha(values: dict[str, str], golden: dict[str, str], case: str) -> None:
    for actual_key, golden_key in PRESSURE_ORACLE.items():
        actual = values.get(actual_key, "")
        expected = golden.get(golden_key, "")
        if not re.fullmatch(r"[0-9a-f]{64}", actual):
            raise RuntimeError(f"{case}: {actual_key} is not a 64-character lowercase SHA-256")
        if not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise RuntimeError(f"golden: {golden_key} is not a 64-character lowercase SHA-256")
        if actual != expected:
            differing = next(index for index, pair in enumerate(zip(actual, expected))
                            if pair[0] != pair[1])
            raise RuntimeError(
                f"{case}: {actual_key} != {golden_key} first_nybble={differing}"
            )


def require_case_evidence(values: dict[str, str], case: str,
                          golden: dict[str, str]) -> None:
    if case in FORCED_CASE_MARKERS:
        require_pass(values, case, FORCED_CASE_MARKERS[case])
    if case == "live-event-pressure":
        require_sha(values, golden, case)
        for marker in ("AUDIO86_GUEST_ASYNC_EVENT_WRAP_COUNT",
                       "AUDIO86_GUEST_ASYNC_BYTE_WRAP_COUNT"):
            try:
                if int(values.get(marker, "0"), 10) < 1:
                    raise RuntimeError(f"{case}: {marker} must be >= 1")
            except ValueError as error:
                raise RuntimeError(f"{case}: invalid {marker}") from error
    if case in SCHEDULES:
        require_sha(values, golden, case)
        if values.get("AUDIO86_GUEST_ASYNC_SCHEDULE_MODE") != case[-1]:
            raise RuntimeError(f"{case}: schedule identity mismatch")
        if case == "schedule-d" and int(
                values.get("AUDIO86_GUEST_ASYNC_SCHEDULE_HANDOFFS", "0"), 10) < 1:
            raise RuntimeError(f"{case}: no actual producer/worker handoff")
    if case in CUTPOINT_SUCCESS or case in CUTPOINT_FATAL:
        point = case.rsplit("-", 1)[1]
        expected = f"{point} entered=1 invariant=PASS fatal="
        value = values.get("AUDIO86_GUEST_ASYNC_CUTPOINT", "")
        wanted = "PASS" if case in CUTPOINT_FATAL else "NA"
        if value != expected + wanted:
            raise RuntimeError(f"{case}: cutpoint evidence mismatch: {value!r}")
    if values.get("AUDIO86_GUEST_ASYNC_HARDENING_CASE") != case:
        raise RuntimeError(f"{case}: case identity mismatch")


def run_case(binary: Path, case: str, timeout: float, repetition: int,
             golden: dict[str, str]) -> None:
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
    require_case_evidence(values, case, golden)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    args = parser.parse_args()
    if args.repetitions != 25 or args.timeout_seconds <= 0:
        raise RuntimeError("86R.4Bb requires exactly 25 forced repetitions")
    try:
        golden_json = json.loads(args.golden.read_text(encoding="utf-8"))
        golden = golden_json["values"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError(f"invalid 86R.3 golden: {args.golden}") from error

    # All forced cases, including every success and representative fatal
    # cutpoint, execute in fresh processes 25 times.
    forced = tuple(FORCED_CASE_MARKERS) + CUTPOINT_SUCCESS + CUTPOINT_FATAL
    for repetition in range(1, args.repetitions + 1):
        for case in forced:
            run_case(args.binary, case, args.timeout_seconds, repetition, golden)

    # Schedule pressure has an independently fixed 4 x 10 acceptance matrix.
    for case in SCHEDULES:
        for repetition in range(1, 11):
            run_case(args.binary, case, args.timeout_seconds, repetition, golden)

    for marker in sorted({item for values in FORCED_CASE_MARKERS.values() for item in values}):
        print(f"{marker}=PASS")
    print("AUDIO86_GUEST_ASYNC_EVENT_WRAP=PASS")
    print("AUDIO86_GUEST_ASYNC_BYTE_WRAP=PASS")
    print("AUDIO86_GUEST_ASYNC_PRESSURE_WORKER_TRACE=PASS")
    print("AUDIO86_GUEST_ASYNC_PRESSURE_PRE_RESET_PCM=PASS")
    print("AUDIO86_GUEST_ASYNC_PRESSURE_FULL_PCM=PASS")
    print("AUDIO86_GUEST_ASYNC_FATAL_WAKE_CONSUMERS=PASS")
    print("AUDIO86_GUEST_ASYNC_CUTPOINT_SUCCESS=PASS")
    print("AUDIO86_GUEST_ASYNC_CUTPOINT_FAILURE=PASS")
    print("AUDIO86_GUEST_ASYNC_SCHEDULE_MATRIX=PASS modes=4 repetitions=10")
    print("AUDIO86_GUEST_ASYNC_HARDENING_FORCED_REPETITIONS=PASS count=25")
    print("AUDIO86_GUEST_ASYNC_HARDENING_STRESS=PASS")
    print("AUDIO86_GUEST_ASYNC_HARDENING_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
