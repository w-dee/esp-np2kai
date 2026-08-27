#!/usr/bin/env python3
"""Fail-closed validation for structured E1C sustained-workload output."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


DESCRIPTOR_PATH = Path(__file__).with_name("opngen_sustained_workload_golden.json")
PROFILES = {"SYNTHETIC-LIGHT", "SYNTHETIC-HEAVY", "STRESS"}
TOP_LEVEL_KEYS = {
    "golden_version",
    "workload_version",
    "sample_rate_hz",
    "quantum_frames",
    "warmup_frames",
    "cases",
}
CASE_KEYS = {
    "profile",
    "duration_frames",
    "event_count",
    "event_crc32",
    "event_sha256",
    "pcm_frames",
    "pcm_bytes",
    "pcm_crc32",
    "pcm_sha256",
}


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_keys(actual: set[str], expected: set[str], label: str) -> None:
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(f"{label} keys mismatch missing={missing} extra={extra}")


def load_descriptor(path: Path = DESCRIPTOR_PATH) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("descriptor must be an object")
    _require_keys(set(document), TOP_LEVEL_KEYS, "descriptor")
    if document["golden_version"] != 1 or document["workload_version"] != 1:
        raise ValueError("descriptor versions must be 1")
    for key in ("sample_rate_hz", "quantum_frames", "warmup_frames"):
        if not _is_int(document[key]):
            raise ValueError(f"descriptor {key} must be an integer")
    if (document["sample_rate_hz"], document["quantum_frames"],
            document["warmup_frames"]) != (48000, 240, 48000):
        raise ValueError("descriptor global workload contract is invalid")
    cases = document["cases"]
    if not isinstance(cases, dict) or set(cases) != {
            "SYNTHETIC-LIGHT-30", "SYNTHETIC-HEAVY-30", "STRESS-30", "STRESS-60"}:
        raise ValueError("descriptor cases are incomplete or unexpected")
    for name, case in cases.items():
        if not isinstance(case, dict):
            raise ValueError(f"descriptor case {name} must be an object")
        _require_keys(set(case), CASE_KEYS, f"descriptor case {name}")
        if case["profile"] not in PROFILES:
            raise ValueError(f"descriptor case {name} profile is invalid")
        for key in ("duration_frames", "event_count", "pcm_frames", "pcm_bytes"):
            if not _is_int(case[key]) or case[key] <= 0:
                raise ValueError(f"descriptor case {name} {key} is invalid")
        for key in ("event_crc32", "pcm_crc32"):
            if not isinstance(case[key], str) or not re.fullmatch(
                    r"0x[0-9a-f]{8}", case[key]):
                raise ValueError(f"descriptor case {name} {key} is malformed")
        for key in ("event_sha256", "pcm_sha256"):
            if not isinstance(case[key], str) or not re.fullmatch(
                    r"[0-9a-f]{64}", case[key]):
                raise ValueError(f"descriptor case {name} {key} is malformed")
    return document


def _one_line(lines: list[str], prefix: str, errors: list[str]) -> str:
    matches = [line for line in lines if line.startswith(prefix + " ")]
    if len(matches) != 1:
        errors.append(f"expected exactly one {prefix} line, got {len(matches)}")
        return ""
    return matches[0]


def _fields(line: str, label: str, errors: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            errors.append(f"{label}: malformed token {token!r}")
            continue
        key, value = token.split("=", 1)
        if key in values:
            errors.append(f"{label}: duplicate field {key}")
        values[key] = value
    return values


def _decimal(values: dict[str, str], key: str, label: str,
             errors: list[str]) -> None:
    value = values.get(key)
    if value is None or not re.fullmatch(r"[0-9]+", value):
        errors.append(f"{label}: malformed decimal {key}={value!r}")


def _require(values: dict[str, str], keys: set[str], label: str,
             errors: list[str]) -> None:
    for key in sorted(keys):
        if key not in values or values[key] == "":
            errors.append(f"{label}: missing {key}")


def _exact(values: dict[str, str], keys: set[str], label: str,
           errors: list[str]) -> None:
    extra = sorted(set(values) - keys)
    if extra:
        errors.append(f"{label}: unexpected fields {extra}")


def _hex(values: dict[str, str], key: str, pattern: str, label: str,
         errors: list[str]) -> None:
    value = values.get(key)
    if value is None or not re.fullmatch(pattern, value):
        errors.append(f"{label}: malformed {key}={value!r}")


def _case_for(meta: dict[str, str], descriptor: dict[str, Any],
              requested: str | None, errors: list[str]) -> tuple[str, dict[str, Any]] | None:
    cases = descriptor["cases"]
    if requested is not None:
        if requested not in cases:
            errors.append(f"unknown requested descriptor case {requested}")
            return None
        return requested, cases[requested]
    matches = [
        (name, case) for name, case in cases.items()
        if meta.get("profile") == case["profile"] and
        meta.get("duration_frames") == str(case["duration_frames"])
    ]
    if len(matches) != 1:
        errors.append(f"workload does not identify exactly one case: {matches}")
        return None
    return matches[0]


def parse_logical_identity(text: str) -> dict[str, str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    errors: list[str] = []
    meta = _fields(_one_line(lines, "E1C_WORKLOAD_META", errors), "META", errors)
    events = _fields(_one_line(lines, "E1C_EVENTS", errors), "EVENTS", errors)
    pcm = _fields(_one_line(lines, "E1C_PCM", errors), "PCM", errors)
    if errors:
        raise ValueError("; ".join(errors))
    return {
        "workload_version": meta["version"],
        "profile": meta["profile"],
        "sample_rate": meta["sample_rate"],
        "duration_frames": meta["duration_frames"],
        "warmup_frames": meta["warmup_frames"],
        "quantum": meta["quantum"],
        "producer_event_count": events["produced"],
        "producer_event_crc32": events["producer_crc32"],
        "producer_event_sha256": events["producer_sha256"],
        "consumer_event_count": events["consumed"],
        "consumer_event_crc32": events["consumer_crc32"],
        "consumer_event_sha256": events["consumer_sha256"],
        "sequence_errors": events["sequence_errors"],
        "pcm_frames": pcm["frames"],
        "pcm_bytes": pcm["bytes"],
        "pcm_crc32": pcm["crc32"],
        "pcm_sha256": pcm["sha256"],
    }


def validate_text(text: str, descriptor: dict[str, Any] | None = None,
                 requested_case: str | None = None) -> list[str]:
    errors: list[str] = []
    if descriptor is None:
        descriptor = load_descriptor()
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    result_lines = [line for line in lines if line.startswith("E1C_RESULT=")]
    if result_lines != ["E1C_RESULT=PASS"]:
        errors.append("expected exactly one terminal E1C_RESULT=PASS")
    if not lines or lines[-1] != "E1C_RESULT=PASS":
        errors.append("E1C_RESULT=PASS must be the final non-empty line")
    if any("E1C_RESULT=FAIL" in line for line in lines):
        errors.append("failure terminal is present")

    meta_line = _one_line(lines, "E1C_WORKLOAD_META", errors)
    events_line = _one_line(lines, "E1C_EVENTS", errors)
    pcm_line = _one_line(lines, "E1C_PCM", errors)
    _one_line(lines, "E1C_QUEUE", errors)
    _one_line(lines, "E1C_TIMING", errors)
    meta = _fields(meta_line, "META", errors) if meta_line else {}
    events = _fields(events_line, "EVENTS", errors) if events_line else {}
    pcm = _fields(pcm_line, "PCM", errors) if pcm_line else {}
    _require(meta, {"version", "profile", "sample_rate", "duration_frames",
                    "warmup_frames", "quantum"}, "META", errors)
    _require(events, {"produced", "consumed", "producer_crc32",
                      "producer_sha256", "consumer_crc32", "consumer_sha256",
                      "sequence_errors"}, "EVENTS", errors)
    _require(pcm, {"frames", "bytes", "crc32", "sha256"}, "PCM", errors)
    _exact(meta, {"version", "profile", "sample_rate", "duration_frames",
                  "warmup_frames", "quantum"}, "META", errors)
    _exact(events, {"produced", "consumed", "producer_crc32",
                    "producer_sha256", "consumer_crc32", "consumer_sha256",
                    "sequence_errors"}, "EVENTS", errors)
    _exact(pcm, {"frames", "bytes", "crc32", "sha256"}, "PCM", errors)
    for key in ("version", "sample_rate", "duration_frames", "warmup_frames",
                "quantum"):
        _decimal(meta, key, "META", errors)
    for key in ("produced", "consumed", "sequence_errors"):
        _decimal(events, key, "EVENTS", errors)
    for key in ("frames", "bytes"):
        _decimal(pcm, key, "PCM", errors)
    for key in ("producer_crc32", "consumer_crc32"):
        _hex(events, key, r"0x[0-9a-f]{8}", "EVENTS", errors)
    for key in ("producer_sha256", "consumer_sha256"):
        _hex(events, key, r"[0-9a-f]{64}", "EVENTS", errors)
    _hex(pcm, "crc32", r"0x[0-9a-f]{8}", "PCM", errors)
    _hex(pcm, "sha256", r"[0-9a-f]{64}", "PCM", errors)
    selected = _case_for(meta, descriptor, requested_case, errors)
    if selected is None:
        return errors
    _, case = selected
    if meta.get("version") != str(descriptor["workload_version"]):
        errors.append("META version does not match descriptor")
    if meta.get("profile") != case["profile"]:
        errors.append("META profile does not match descriptor")
    for key, expected in (("sample_rate", descriptor["sample_rate_hz"]),
                          ("duration_frames", case["duration_frames"]),
                          ("warmup_frames", descriptor["warmup_frames"]),
                          ("quantum", descriptor["quantum_frames"])):
        if meta.get(key) != str(expected):
            errors.append(f"META {key} does not match descriptor")
    if events.get("produced") != events.get("consumed"):
        errors.append("producer/consumer event count mismatch")
    if events.get("producer_crc32") != events.get("consumer_crc32"):
        errors.append("producer/consumer event CRC mismatch")
    if events.get("producer_sha256") != events.get("consumer_sha256"):
        errors.append("producer/consumer event SHA mismatch")
    if events.get("sequence_errors") != "0":
        errors.append("sequence_errors is not zero")
    expected_events = str(case["event_count"])
    if events.get("produced") != expected_events:
        errors.append("producer event count does not match descriptor")
    if events.get("producer_crc32") != case["event_crc32"]:
        errors.append("event CRC does not match descriptor")
    if events.get("producer_sha256") != case["event_sha256"]:
        errors.append("event SHA does not match descriptor")
    if pcm.get("frames") != str(case["pcm_frames"]):
        errors.append("PCM frames do not match descriptor")
    if pcm.get("bytes") != str(case["pcm_bytes"]):
        errors.append("PCM bytes do not match descriptor")
    if pcm.get("crc32") != case["pcm_crc32"]:
        errors.append("PCM CRC does not match descriptor")
    if pcm.get("sha256") != case["pcm_sha256"]:
        errors.append("PCM SHA does not match descriptor")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--descriptor", type=Path, default=DESCRIPTOR_PATH)
    parser.add_argument("--case")
    args = parser.parse_args()
    try:
        descriptor = load_descriptor(args.descriptor)
        errors = validate_text(args.log.read_text(encoding="utf-8",
                                                  errors="replace"),
                               descriptor, args.case)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors = [str(error)]
    if errors:
        for error in errors:
            print(f"E1C_VALIDATION_ERROR {error}")
        print("E1C_VALIDATION=FAIL")
        return 1
    print("E1C_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
