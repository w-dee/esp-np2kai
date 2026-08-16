#!/usr/bin/env python3
"""Run the headless runner with an external wall-clock safety boundary."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from typing import Optional, Sequence


OUTCOME_STATUS = {
    "PASS": 0,
    "FAIL": 1,
    "NOT_REACHED": 2,
    "RUNNING_TIMEOUT": 3,
    "INVALID": 4,
    "HARNESS_ERROR": 5,
}
OUTCOME_PATTERN = re.compile(
    rb"^NP2TEST_RESULT=(PASS|FAIL|NOT_REACHED|RUNNING_TIMEOUT|INVALID|HARNESS_ERROR)$"
)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run headless_runner under an external wall-clock timeout."
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="wall-clock safety timeout in seconds (default: 30)",
    )
    parser.add_argument(
        "--expect-result",
        choices=tuple(OUTCOME_STATUS),
        help="require this normalized result token",
    )
    parser.add_argument("runner")
    parser.add_argument("fixture")
    return parser


def _write_stdout(data: bytes) -> None:
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()


def _write_stderr(data: bytes) -> None:
    if data:
        sys.stderr.buffer.write(data)
        sys.stderr.buffer.flush()


def _result_lines(data: bytes) -> tuple[list[str], list[bytes]]:
    valid: list[str] = []
    malformed: list[bytes] = []
    for line in data.splitlines():
        if line.startswith(b"NP2TEST_RESULT="):
            match = OUTCOME_PATTERN.fullmatch(line)
            if match is None:
                malformed.append(line)
            else:
                valid.append(match.group(1).decode("ascii"))
    return valid, malformed


def _strip_result_lines(data: bytes) -> bytes:
    """Avoid duplicating a result if a wedged child violated the runner contract."""
    kept: list[bytes] = []
    for segment in data.splitlines(keepends=True):
        logical = segment.rstrip(b"\r\n")
        if logical.startswith(b"NP2TEST_RESULT="):
            continue
        kept.append(segment)
    return b"".join(kept)


def _terminate_and_reap(process: subprocess.Popen[bytes]) -> tuple[bytes, bytes]:
    process.terminate()
    try:
        return process.communicate(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()


def _run_child(runner: str, fixture: str, timeout: float) -> tuple[str, object]:
    try:
        process = subprocess.Popen(
            [runner, fixture],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        print(f"supervisor: cannot start runner: {error}", file=sys.stderr)
        return "spawn-error", 5

    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        stdout, stderr = _terminate_and_reap(process)
        timeout_results, timeout_malformed = _result_lines(stdout)
        if timeout_results or timeout_malformed:
            print(
                "supervisor: child emitted NP2TEST_RESULT before timeout; "
                "runner contract violation",
                file=sys.stderr,
            )
        _write_stdout(_strip_result_lines(stdout))
        _write_stderr(stderr)
        print("supervisor timeout", file=sys.stderr)
        _write_stdout(b"NP2TEST_RESULT=HARNESS_ERROR\n")
        return "timeout", 5

    _write_stdout(stdout)
    _write_stderr(stderr)
    return "normal", (process.returncode, stdout)


def _validate_normal_child(
    returncode: int,
    stdout: bytes,
    expected_result: Optional[str],
) -> int:
    valid, malformed = _result_lines(stdout)
    if malformed:
        print("supervisor: malformed NP2TEST_RESULT line", file=sys.stderr)
        return 5
    if len(valid) != 1:
        print(
            "supervisor: expected exactly one NP2TEST_RESULT line, "
            f"found {len(valid)}",
            file=sys.stderr,
        )
        return 5

    result = valid[0]
    if expected_result is not None and result != expected_result:
        print(
            f"supervisor: expected NP2TEST_RESULT={expected_result}, "
            f"got NP2TEST_RESULT={result}",
            file=sys.stderr,
        )
        return 5

    expected_status = OUTCOME_STATUS[result]
    if returncode != expected_status:
        print(
            f"supervisor: result/status mismatch: {result} with exit "
            f"{returncode}, expected {expected_status}",
            file=sys.stderr,
        )
        return 5
    return returncode


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    state, result = _run_child(args.runner, args.fixture, args.timeout)
    if state == "timeout" or state == "spawn-error":
        return int(result)

    return _validate_normal_child(result[0], result[1], args.expect_result)


if __name__ == "__main__":
    raise SystemExit(main())
