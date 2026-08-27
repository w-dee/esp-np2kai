#!/usr/bin/env python3
"""Fail-closed validation for an accepted S3 S98 integration log."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


DESCRIPTOR_PATH = Path(__file__).with_name("opngen_s98_fixture_golden.json")
FIXTURES = {
    "fm_single_tone",
    "fm_frequency_change",
    "fm_three_channel",
    "fm_same_timestamp_burst",
    "fm_envelope",
    "fm_algorithm_feedback",
}
TOP_LEVEL_KEYS = {
    "golden_version",
    "s98_version",
    "sample_rate_hz",
    "effective_opngen_clock_hz",
    "fixtures",
}
CASE_KEYS = {
    "fixture_name",
    "source_bytes",
    "source_sha256",
    "device_count",
    "device_type",
    "declared_device_clock_hz",
    "effective_opngen_clock_hz",
    "clock_policy",
    "raw_timer_numerator",
    "raw_timer_denominator",
    "effective_timer_numerator",
    "effective_timer_denominator",
    "data_offset",
    "tag_offset",
    "loop_offset",
    "source_write_count",
    "ignored_write_count",
    "final_sync_count",
    "event_count",
    "event_crc32",
    "event_sha256",
    "end_frame",
    "same_timestamp_pairs",
    "pcm_frames",
    "pcm_bytes",
    "pcm_crc32",
    "pcm_sha256",
}
SOURCE_KEYS = {"fixture", "source_bytes", "source_sha256"}
META_KEYS = {
    "fixture", "s98_version", "device_count", "device_type",
    "declared_clock", "effective_clock", "clock_policy", "raw_timer",
    "effective_timer", "data_offset", "tag_offset", "loop_offset",
    "source_writes", "ignored_writes", "final_sync", "end_frame",
}
EVENT_KEYS = {
    "fixture", "preflight_count", "preflight_crc32", "preflight_sha256",
    "producer_count", "producer_crc32", "producer_sha256",
    "consumer_count", "consumer_crc32", "consumer_sha256",
    "sequence_errors", "same_timestamp_pairs",
}
PCM_KEYS = {"fixture", "frames", "bytes", "crc32", "sha256"}
INVARIANT_KEYS = {"fixture", "parser_repeat", "transport_identity", "source_immutable"}


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
    if document["golden_version"] != 1 or document["s98_version"] != 3:
        raise ValueError("descriptor versions are invalid")
    if document["sample_rate_hz"] != 48000 or document["effective_opngen_clock_hz"] != 3993600:
        raise ValueError("descriptor global contract is invalid")
    fixtures = document["fixtures"]
    if not isinstance(fixtures, dict) or set(fixtures) != FIXTURES:
        raise ValueError("descriptor fixture set is incomplete or unexpected")
    for name, case in fixtures.items():
        if not isinstance(case, dict):
            raise ValueError(f"descriptor fixture {name} must be an object")
        _require_keys(set(case), CASE_KEYS, f"descriptor fixture {name}")
        if case["fixture_name"] != name:
            raise ValueError(f"descriptor fixture_name mismatch for {name}")
        for key in CASE_KEYS - {"fixture_name", "source_sha256", "event_crc32", "event_sha256", "pcm_crc32", "pcm_sha256", "clock_policy"}:
            if not _is_int(case[key]) or case[key] < 0:
                raise ValueError(f"descriptor {name} {key} is invalid")
        for key in ("source_sha256", "event_sha256", "pcm_sha256"):
            if not isinstance(case[key], str) or not re.fullmatch(r"[0-9a-f]{64}", case[key]):
                raise ValueError(f"descriptor {name} {key} is malformed")
        for key in ("event_crc32", "pcm_crc32"):
            if not isinstance(case[key], str) or not re.fullmatch(r"0x[0-9a-f]{8}", case[key]):
                raise ValueError(f"descriptor {name} {key} is malformed")
        if case["clock_policy"] != "EXACT_NP2":
            raise ValueError(f"descriptor {name} clock policy is invalid")
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


def _exact(values: dict[str, str], expected: set[str], label: str, errors: list[str]) -> None:
    if set(values) != expected:
        errors.append(f"{label} keys mismatch missing={sorted(expected - set(values))} extra={sorted(set(values) - expected)}")


def _digits(values: dict[str, str], key: str, label: str, errors: list[str]) -> None:
    if not re.fullmatch(r"[0-9]+", values.get(key, "")):
        errors.append(f"{label} {key} is not a decimal integer")


def _sha(values: dict[str, str], key: str, label: str, errors: list[str]) -> None:
    if not re.fullmatch(r"[0-9a-f]{64}", values.get(key, "")):
        errors.append(f"{label} {key} is not a lowercase SHA-256")


def _crc(values: dict[str, str], key: str, label: str, errors: list[str]) -> None:
    if not re.fullmatch(r"[0-9a-f]{8}", values.get(key, "")):
        errors.append(f"{label} {key} is not an eight-digit lowercase CRC-32")


def _same(values: dict[str, str], key: str, expected: str, label: str, errors: list[str]) -> None:
    if values.get(key) != expected:
        errors.append(f"{label} {key} does not match descriptor")


def validate_text(text: str, descriptor: dict[str, Any] | None = None,
                 requested_fixture: str | None = None) -> list[str]:
    errors: list[str] = []
    if descriptor is None:
        descriptor = load_descriptor()
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    terminal = [line for line in lines if line.startswith("S98_S3_RESULT")]
    if len(terminal) != 1 or not re.fullmatch(
            r"S98_S3_RESULT fixture=[a-z0-9_]+ PASS", terminal[0]):
        errors.append("expected exactly one terminal S98_S3_RESULT fixture=... PASS")
    if terminal and lines[-1] != terminal[0]:
        errors.append("terminal result must be the final non-empty line")
    if any("S98_S3_RESULT=FAIL" in line or line.endswith(" FAIL") for line in lines):
        errors.append("failure result is present")

    source_line = _one_line(lines, "S98_S3_SOURCE", errors)
    meta_line = _one_line(lines, "S98_S3_META", errors)
    event_line = _one_line(lines, "S98_S3_EVENTS", errors)
    pcm_line = _one_line(lines, "S98_S3_PCM", errors)
    invariant_line = _one_line(lines, "S98_S3_INVARIANTS", errors)
    source = _fields(source_line, "SOURCE", errors) if source_line else {}
    meta = _fields(meta_line, "META", errors) if meta_line else {}
    events = _fields(event_line, "EVENTS", errors) if event_line else {}
    pcm = _fields(pcm_line, "PCM", errors) if pcm_line else {}
    invariant = _fields(invariant_line, "INVARIANTS", errors) if invariant_line else {}
    _exact(source, SOURCE_KEYS, "SOURCE", errors)
    _exact(meta, META_KEYS, "META", errors)
    _exact(events, EVENT_KEYS, "EVENTS", errors)
    _exact(pcm, PCM_KEYS, "PCM", errors)
    _exact(invariant, INVARIANT_KEYS, "INVARIANTS", errors)
    for values, keys, label in ((source, SOURCE_KEYS, "SOURCE"),
                                (meta, META_KEYS, "META"),
                                (events, EVENT_KEYS, "EVENTS"),
                                (pcm, PCM_KEYS, "PCM")):
        for key in keys:
            if key in values and key not in {"fixture", "source_sha256", "preflight_sha256", "producer_sha256", "consumer_sha256", "sha256", "preflight_crc32", "producer_crc32", "consumer_crc32", "crc32", "clock_policy", "raw_timer", "effective_timer"}:
                _digits(values, key, label, errors)
    _sha(source, "source_sha256", "SOURCE", errors)
    for key in ("preflight_sha256", "producer_sha256", "consumer_sha256"):
        _sha(events, key, "EVENTS", errors)
    _sha(pcm, "sha256", "PCM", errors)
    for key in ("preflight_crc32", "producer_crc32", "consumer_crc32"):
        _crc(events, key, "EVENTS", errors)
    _crc(pcm, "crc32", "PCM", errors)
    for key in ("raw_timer", "effective_timer"):
        if not re.fullmatch(r"[0-9]+/[0-9]+", meta.get(key, "")):
            errors.append(f"META {key} is not numerator/denominator")

    fixture = source.get("fixture")
    for values, label in ((meta, "META"), (events, "EVENTS"), (pcm, "PCM"), (invariant, "INVARIANTS")):
        if values.get("fixture") != fixture:
            errors.append(f"{label} fixture does not match SOURCE")
    if requested_fixture is not None and fixture != requested_fixture:
        errors.append("fixture does not match requested fixture")
    if fixture not in FIXTURES:
        errors.append(f"unknown fixture {fixture!r}")
        return errors
    case = descriptor["fixtures"].get(fixture)
    if case is None:
        errors.append("fixture missing from descriptor")
        return errors
    if terminal and terminal[0] != f"S98_S3_RESULT fixture={fixture} PASS":
        errors.append("terminal result fixture does not match SOURCE")
    for key in ("parser_repeat", "transport_identity", "source_immutable"):
        if invariant.get(key) != "PASS":
            errors.append(f"INVARIANTS {key} is not PASS")

    source_expected = {"source_bytes": case["source_bytes"], "source_sha256": case["source_sha256"]}
    for key, expected in source_expected.items():
        _same(source, key, str(expected), "SOURCE", errors)
    meta_expected = {
        "s98_version": descriptor["s98_version"], "device_count": case["device_count"],
        "device_type": case["device_type"], "declared_clock": case["declared_device_clock_hz"],
        "effective_clock": case["effective_opngen_clock_hz"], "clock_policy": case["clock_policy"],
        "raw_timer": f"{case['raw_timer_numerator']}/{case['raw_timer_denominator']}",
        "effective_timer": f"{case['effective_timer_numerator']}/{case['effective_timer_denominator']}",
        "data_offset": case["data_offset"], "tag_offset": case["tag_offset"],
        "loop_offset": case["loop_offset"], "source_writes": case["source_write_count"],
        "ignored_writes": case["ignored_write_count"], "final_sync": case["final_sync_count"],
        "end_frame": case["end_frame"],
    }
    for key, expected in meta_expected.items():
        _same(meta, key, str(expected), "META", errors)
    event_expected = {
        "preflight_count": case["event_count"], "producer_count": case["event_count"],
        "consumer_count": case["event_count"], "preflight_crc32": case["event_crc32"][2:],
        "producer_crc32": case["event_crc32"][2:], "consumer_crc32": case["event_crc32"][2:],
        "preflight_sha256": case["event_sha256"], "producer_sha256": case["event_sha256"],
        "consumer_sha256": case["event_sha256"], "sequence_errors": 0,
        "same_timestamp_pairs": case["same_timestamp_pairs"],
    }
    for key, expected in event_expected.items():
        _same(events, key, str(expected), "EVENTS", errors)
    if not (events.get("preflight_count") == events.get("producer_count") == events.get("consumer_count")):
        errors.append("event counts differ between preflight/producer/consumer")
    if not (events.get("preflight_crc32") == events.get("producer_crc32") == events.get("consumer_crc32")):
        errors.append("event CRCs differ between preflight/producer/consumer")
    if not (events.get("preflight_sha256") == events.get("producer_sha256") == events.get("consumer_sha256")):
        errors.append("event SHAs differ between preflight/producer/consumer")
    for key, expected in (("frames", case["pcm_frames"]), ("bytes", case["pcm_bytes"]),
                          ("crc32", case["pcm_crc32"][2:]), ("sha256", case["pcm_sha256"])):
        _same(pcm, key, str(expected), "PCM", errors)
    if pcm.get("frames") != meta.get("end_frame"):
        errors.append("PCM frames do not equal end_frame")
    if (not pcm.get("frames", "").isdigit() or
            pcm.get("bytes") != str(int(pcm["frames"]) * 4)):
        errors.append("PCM bytes do not equal frames*4")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--descriptor", type=Path, default=DESCRIPTOR_PATH)
    parser.add_argument("--fixture")
    args = parser.parse_args()
    try:
        descriptor = load_descriptor(args.descriptor)
        errors = validate_text(args.log.read_text(encoding="utf-8", errors="replace"), descriptor, args.fixture)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors = [str(error)]
    if errors:
        for error in errors:
            print(f"S98_S3_VALIDATION_ERROR {error}")
        print("S98_S3_VALIDATION=FAIL")
        return 1
    print("S98_S3_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
