#!/usr/bin/env python3
"""Fail-closed validator for the isolated 86H.4 hardening suite."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


CRC32 = re.compile(r"^[0-9a-f]{8}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
FROZEN_CRC32 = "58929f1f"
FROZEN_SHA256 = (
    "7f1bc0cdcab519690c0d3580746827199f86dd270868f33ceb01d230e096310e"
)
PV_CASES = {f"PV{number:02d}" for number in range(1, 6)}
R_CASES = {"R01", "R02", "R03"}
FULL_CASES = {"N01", "N02", "REC01"}
FAULT_ERRORS = {
    "F01": "SEQUENCE", "F02": "SEQUENCE",
    "F03": "TIMESTAMP", "F04": "TIMESTAMP",
    "F05": "OPCODE", "F06": "OPCODE",
    "F07": "DATA_LENGTH", "F08": "DATA_LENGTH", "F09": "DATA_LENGTH",
    "F10": "DATA_AVAILABILITY",
    "F11": "PRODUCER", "F12": "PRODUCER", "F13": "PRODUCER",
    "F14": "PRODUCER",
    "F15": "WATERMARK", "F16": "WATERMARK",
    "F17": "PREMATURE_COMPLETION", "F18": "PREMATURE_COMPLETION",
    "F19": "WATERMARK", "F20": "WATERMARK",
    "F21": "WORKER", "F22": "PRODUCER", "F23": "PRODUCER",
    "F24": "WORKER",
}


def fail(message: str) -> None:
    raise SystemExit(f"AUDIO86_HARDENING_VALIDATION=FAIL {message}")


def parse_fields(line: str, prefix: str) -> dict[str, str]:
    if not line.startswith(prefix + " "):
        fail(f"malformed {prefix} marker")
    values: dict[str, str] = {}
    for token in line[len(prefix):].split():
        if "=" not in token:
            fail(f"{prefix}: malformed token {token!r}")
        key, value = token.split("=", 1)
        if not key or key in values:
            fail(f"{prefix}: duplicate or empty field {key!r}")
        values[key] = value
    return values


def exact_fields(values: dict[str, str], expected: set[str], label: str) -> None:
    if set(values) != expected:
        fail(f"{label}: fields mismatch expected={sorted(expected)} got={sorted(values)}")


def require_uint(values: dict[str, str], key: str, label: str) -> None:
    value = values.get(key, "")
    if not re.fullmatch(r"[0-9]+", value):
        fail(f"{label}.{key}: malformed unsigned integer")


def validate_positive(values: dict[str, str]) -> None:
    exact_fields(values, {"id", "pass", "pcm_crc32", "pcm_sha256",
                          "producer_reaped", "worker_reaped"}, "positive")
    if values["id"] not in FULL_CASES or values["pass"] != "1":
        fail("positive case/pass mismatch")
    if values["pcm_crc32"] != FROZEN_CRC32 or not SHA256.fullmatch(
            values["pcm_sha256"]) or values["pcm_sha256"] != FROZEN_SHA256:
        fail("positive frozen PCM identity mismatch")
    if values["producer_reaped"] != "1" or values["worker_reaped"] != "1":
        fail("positive case has unreaped thread")


def validate_fault(values: dict[str, str]) -> None:
    expected_fields = {
        "id", "injected", "detected", "first_error", "producer_created",
        "worker_created", "producer_terminal", "worker_terminal",
        "producer_reaped", "worker_reaped", "workload_success",
        "peer_unblocked", "later_error_attempted", "event_residual",
        "byte_residual", "pass",
    }
    exact_fields(values, expected_fields, "fault")
    case_id = values["id"]
    if case_id not in FAULT_ERRORS:
        fail(f"unknown fault case {case_id!r}")
    if (values["injected"], values["detected"], values["pass"],
            values["workload_success"], values["peer_unblocked"]) != (
                "1", "1", "1", "0", "1"):
        fail(f"{case_id}: injection/detection/pass policy mismatch")
    if values["first_error"] != FAULT_ERRORS[case_id]:
        fail(f"{case_id}: unexpected first_error {values['first_error']!r}")
    for key in ("producer_created", "worker_created", "producer_reaped",
                "worker_reaped", "later_error_attempted", "event_residual",
                "byte_residual"):
        require_uint(values, key, case_id)
    if (int(values["producer_created"]) and
            values["producer_reaped"] != "1"):
        fail(f"{case_id}: producer was created but not reaped")
    if (int(values["worker_created"]) and values["worker_reaped"] != "1"):
        fail(f"{case_id}: worker was created but not reaped")
    if values["producer_terminal"] not in {"NOT_STARTED", "COMPLETED", "ABORTED"}:
        fail(f"{case_id}: invalid producer terminal state")
    if values["worker_terminal"] not in {"NOT_STARTED", "COMPLETED", "ABORTED"}:
        fail(f"{case_id}: invalid worker terminal state")
    if values["later_error_attempted"] != ("1" if case_id == "F22" else "0"):
        fail(f"{case_id}: unexpected later-error attempt marker")


def validate_normal(path: Path) -> None:
    try:
        lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
    except OSError as error:
        fail(str(error))
    if any("=FAIL" in line or "TIMEOUT" in line for line in lines):
        fail("explicit failure or timeout marker")
    if any(line == "AUDIO86_ASYNC_RESULT=PASS" for line in lines):
        fail("positive 86H.3 success marker is not valid in hardening log")
    marker_counts: dict[str, int] = {}
    for line in lines:
        if line.startswith("AUDIO86_PREVALIDATION_CASE "):
            prefix = "AUDIO86_PREVALIDATION_CASE"
            values = parse_fields(line, prefix)
            exact_fields(values, {"id", "pass"}, prefix)
            if values["id"] not in PV_CASES or values["pass"] != "1":
                fail("prevalidation case mismatch")
            case_id = values["id"]
        elif line.startswith("AUDIO86_TRANSPORT_CASE "):
            prefix = "AUDIO86_TRANSPORT_CASE"
            values = parse_fields(line, prefix)
            exact_fields(values, {"id", "pass", "threaded", "full", "wrap"}, prefix)
            if values["id"] not in R_CASES or values["pass"] != "1" or \
                    values["threaded"] != "2" or values["full"] != "1" or \
                    values["wrap"] != "1":
                fail("transport case mismatch")
            case_id = values["id"]
        elif line.startswith("AUDIO86_HARDENING_CASE "):
            prefix = "AUDIO86_HARDENING_CASE"
            values = parse_fields(line, prefix)
            validate_positive(values)
            case_id = values["id"]
        elif line.startswith("AUDIO86_FAULT_CASE "):
            prefix = "AUDIO86_FAULT_CASE"
            values = parse_fields(line, prefix)
            validate_fault(values)
            case_id = values["id"]
        elif line == "AUDIO86_HARDENING_RESULT=PASS":
            case_id = "__result__"
        else:
            fail(f"unexpected line {line!r}")
        marker_counts[case_id] = marker_counts.get(case_id, 0) + 1
    expected = PV_CASES | R_CASES | FULL_CASES | set(FAULT_ERRORS)
    if set(marker_counts) - expected != {"__result__"}:
        fail("required case set is incomplete or contains unknown IDs")
    for case_id in expected:
        if marker_counts.get(case_id) != 1:
            fail(f"case {case_id} marker count={marker_counts.get(case_id, 0)}")
    if marker_counts.get("__result__") != 1 or not lines or \
            lines[-1] != "AUDIO86_HARDENING_RESULT=PASS":
        fail("exactly one final hardening PASS marker is required")
    print("AUDIO86_HARDENING_VALIDATION=PASS")


def validate_soak(path: Path) -> None:
    try:
        lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
    except OSError as error:
        fail(str(error))
    if len(lines) != 1 or not lines[0].startswith("AUDIO86_HARDENING_SOAK "):
        fail("SOAK25 requires exactly one soak marker")
    values = parse_fields(lines[0], "AUDIO86_HARDENING_SOAK")
    exact_fields(values, {"repetitions", "pass", "pcm_crc32", "pcm_sha256"}, "soak")
    if values != {"repetitions": "25", "pass": "1", "pcm_crc32": FROZEN_CRC32,
                  "pcm_sha256": FROZEN_SHA256}:
        fail("SOAK25 frozen identity or repetition mismatch")
    print("AUDIO86_HARDENING_SOAK_VALIDATION=PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--soak", action="store_true")
    # Keep the CLI compatible with the other fixture validators while making
    # the frozen identity explicit and mechanically checked in this validator.
    parser.add_argument("--golden", type=Path)
    args = parser.parse_args()
    if args.soak:
        validate_soak(args.log)
    else:
        validate_normal(args.log)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
