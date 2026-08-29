#!/usr/bin/env python3
"""Run the 86H.4 hardening cases in isolated child processes."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


PV_CASES = ("PV01", "PV02", "PV03", "PV04", "PV05")
POSITIVE_CASES = ("N01", "N02")
TRANSPORT_CASES = ("R01", "R02", "R03")
FAULT_CASES = tuple(f"F{number:02d}" for number in range(1, 25))
CASE_ORDER = PV_CASES + POSITIVE_CASES + TRANSPORT_CASES + FAULT_CASES + ("REC01",)
FULL_CASES = set(POSITIVE_CASES) | {"REC01"}


def run_child(binary: Path, case_id: str, timeout: float) -> tuple[str, int]:
    try:
        completed = subprocess.run(
            [str(binary), "--case", case_id],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or "") + (error.stderr or "")
        return output + f"AUDIO86_HARDENING_TIMEOUT id={case_id}\n", 124
    return completed.stdout + completed.stderr, completed.returncode


def run_suite(binary: Path, output_path: Path, short_timeout: float,
              full_timeout: float) -> int:
    lines: list[str] = []
    for case_id in CASE_ORDER:
        timeout = full_timeout if case_id in FULL_CASES else short_timeout
        child_output, status = run_child(binary, case_id, timeout)
        if case_id.startswith("PV"):
            if status == 0 and not child_output.strip():
                lines.append(f"AUDIO86_PREVALIDATION_CASE id={case_id} pass=1")
            else:
                lines.append(f"AUDIO86_PREVALIDATION_CASE id={case_id} pass=0")
        elif case_id in {"R01", "R02", "R03"}:
            if status == 0 and not child_output.strip():
                lines.append(
                    f"AUDIO86_TRANSPORT_CASE id={case_id} pass=1 threaded=2 full=1 wrap=1"
                )
            else:
                lines.append(f"AUDIO86_TRANSPORT_CASE id={case_id} pass=0")
        else:
            # The executable emits exactly one machine-readable case marker.
            # Keep child output intact so the validator can reject malformed or
            # duplicated markers rather than inferring success from exit status.
            lines.extend(line for line in child_output.splitlines() if line.strip())
        if status != 0:
            output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"AUDIO86_HARDENING_RUN=FAIL case={case_id}", file=sys.stderr)
            return 1
    lines.append("AUDIO86_HARDENING_RESULT=PASS")
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


def run_soak(binary: Path, output_path: Path, timeout: float) -> int:
    child_output, status = run_child(binary, "SOAK25", timeout)
    output_path.write_text(child_output, encoding="utf-8")
    if status != 0:
        print("AUDIO86_HARDENING_SOAK_RUN=FAIL", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--short-timeout-seconds", type=float, default=3.0)
    parser.add_argument("--full-timeout-seconds", type=float, default=15.0)
    parser.add_argument("--soak", action="store_true")
    parser.add_argument("--soak-timeout-seconds", type=float, default=90.0)
    args = parser.parse_args()
    if (args.short_timeout_seconds <= 0 or args.full_timeout_seconds <= 0 or
            args.soak_timeout_seconds <= 0):
        parser.error("timeouts must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.soak:
        return run_soak(args.binary, args.output, args.soak_timeout_seconds)
    return run_suite(args.binary, args.output, args.short_timeout_seconds,
                     args.full_timeout_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
