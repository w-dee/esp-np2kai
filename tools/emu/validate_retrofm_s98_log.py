#!/usr/bin/env python3
"""Fail-closed validator for the accepted RetroFM S98 playback log."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


DESCRIPTOR_PATH = Path(__file__).with_name("opngen_retrofm_s98_golden.json")
TOP_KEYS = {"golden_version", "fixture_id", "provenance_manifest", "input",
            "parser", "event", "pcm"}
PARSER_KEYS = {
    "s98_version", "device_count", "device_type", "declared_device_clock_hz",
    "effective_opngen_clock_hz", "clock_policy", "timer_numerator",
    "timer_denominator", "compression", "data_offset", "tag_offset",
    "loop_offset", "source_write_count", "ignored_write_count",
    "final_sync_count", "end_frame",
}
META_KEYS = {
    "fixture", "s98_version", "device_count", "device_type", "declared_clock",
    "effective_clock", "clock_policy", "raw_timer", "effective_timer",
    "data_offset", "tag_offset", "loop_offset", "source_writes",
    "ignored_writes", "final_sync", "end_frame",
}
EVENT_KEYS = {
    "fixture", "preflight_count", "preflight_crc32", "preflight_sha256",
    "producer_count", "producer_crc32", "producer_sha256", "consumer_count",
    "consumer_crc32", "consumer_sha256", "sequence_errors", "same_timestamp_pairs",
}
PCM_KEYS = {"fixture", "frames", "bytes", "crc32", "sha256"}
SOURCE_KEYS = {"fixture", "source_bytes", "source_sha256"}
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
CRC_RE = re.compile(r"0x[0-9a-f]{8}\Z")


def _keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        actual = set(value) if isinstance(value, dict) else set()
        raise ValueError(f"{label} keys mismatch missing={sorted(expected - actual)} "
                         f"extra={sorted(actual - expected)}")


def _int(value: Any, label: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"{label} must be a non-negative integer")


def _sha(value: Any, label: str) -> None:
    if not isinstance(value, str) or not SHA_RE.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase SHA-256")


def _crc(value: Any, label: str) -> None:
    if not isinstance(value, str) or not CRC_RE.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase CRC-32")


def load_descriptor(path: Path = DESCRIPTOR_PATH) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    _keys(document, TOP_KEYS, "descriptor")
    if document["golden_version"] != 1:
        raise ValueError("descriptor golden_version is invalid")
    if document["fixture_id"] != "retrofm_pocket_demo":
        raise ValueError("descriptor fixture_id is invalid")
    provenance = document["provenance_manifest"]
    _keys(provenance, {"path", "manifest_version"}, "provenance_manifest")
    if provenance != {
        "path": "tools/emu/retrofm_pocket_fixture_provenance.json",
        "manifest_version": 1,
    }:
        raise ValueError("descriptor provenance reference is invalid")
    input_record = document["input"]
    _keys(input_record, {"path", "bytes", "sha256"}, "input")
    expected_input = {
        "path": "testdata/s98/retrofm-pocket-demo-strict.s98",
        "bytes": 3753,
        "sha256": "702d8b3003d2d81449fd1003aa2231afdacaae9d680f73fdf11d8195edb046c2",
    }
    if input_record != expected_input:
        raise ValueError("descriptor input identity is invalid")
    _sha(input_record["sha256"], "input.sha256")
    _int(input_record["bytes"], "input.bytes")

    parser = document["parser"]
    _keys(parser, PARSER_KEYS, "parser")
    expected_parser = {
        "s98_version": 3, "device_count": 1, "device_type": 2,
        "declared_device_clock_hz": 4000000, "effective_opngen_clock_hz": 3993600,
        "clock_policy": "WORKLOAD_CLOCK_MISMATCH", "timer_numerator": 1,
        "timer_denominator": 44100, "compression": 0, "data_offset": 48,
        "tag_offset": 3578, "loop_offset": 0, "source_write_count": 1047,
        "ignored_write_count": 0, "final_sync_count": 530082, "end_frame": 576960,
    }
    if parser != expected_parser:
        raise ValueError("descriptor parser contract is invalid")
    for key in PARSER_KEYS - {"clock_policy"}:
        _int(parser[key], f"parser.{key}")

    event = document["event"]
    _keys(event, {"count", "crc32", "sha256"}, "event")
    if event != {
        "count": 1047,
        "crc32": "0x3416c2b6",
        "sha256": "898b049d1c37c8cc6503759849244048e0e7f778087e1eb5706bedf116e9dacf",
    }:
        raise ValueError("descriptor event contract is invalid")
    _int(event["count"], "event.count")
    _crc(event["crc32"], "event.crc32")
    _sha(event["sha256"], "event.sha256")

    pcm = document["pcm"]
    _keys(pcm, {"sample_rate_hz", "channels", "format", "frames", "bytes",
                "crc32", "sha256"}, "pcm")
    if pcm != {
        "sample_rate_hz": 48000, "channels": 2, "format": "s16le",
        "frames": 576960, "bytes": 2307840, "crc32": "0x79b0dfad",
        "sha256": "1d4d24ad9c966dea085607afee6a9ecb049c2c476863c534dbfe0e50ace1016b",
    }:
        raise ValueError("descriptor PCM contract is invalid")
    for key in ("sample_rate_hz", "channels", "frames", "bytes"):
        _int(pcm[key], f"pcm.{key}")
    _crc(pcm["crc32"], "pcm.crc32")
    _sha(pcm["sha256"], "pcm.sha256")
    return document


def _fields(line: str, expected: set[str], label: str, errors: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in line.split()[1:]:
        key, separator, value = token.partition("=")
        if not separator or key in values:
            errors.append(f"{label}: malformed or duplicate token {token!r}")
            continue
        values[key] = value
    if set(values) != expected:
        errors.append(f"{label}: keys mismatch missing={sorted(expected - set(values))} "
                      f"extra={sorted(set(values) - expected)}")
    return values


def _same(values: dict[str, str], key: str, expected: str, label: str,
          errors: list[str]) -> None:
    if values.get(key) != expected:
        errors.append(f"{label}.{key}: {values.get(key)!r} != {expected!r}")


def validate_text(text: str, descriptor: dict[str, Any] | None = None,
                  fixture_id: str | None = None) -> list[str]:
    errors: list[str] = []
    if descriptor is None:
        try:
            descriptor = load_descriptor()
        except (OSError, ValueError, json.JSONDecodeError) as error:
            return [f"descriptor: {error}"]
    expected_fixture = fixture_id or descriptor["fixture_id"]
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    result_lines = [line for line in lines if line.startswith("S98_S3_RESULT ")]
    expected_result = f"S98_S3_RESULT fixture={expected_fixture} PASS"
    if result_lines != [expected_result]:
        errors.append("terminal result must be exactly one PASS marker")
    elif not lines or lines[-1] != expected_result:
        errors.append("PASS result marker must be the terminal output line")
    records: dict[str, dict[str, str]] = {}
    for prefix, keys in (("S98_S3_SOURCE", SOURCE_KEYS), ("S98_S3_META", META_KEYS),
                         ("S98_S3_EVENTS", EVENT_KEYS), ("S98_S3_PCM", PCM_KEYS)):
        matches = [line for line in lines if line.startswith(prefix + " ")]
        if len(matches) != 1:
            errors.append(f"expected exactly one {prefix} line, got {len(matches)}")
            continue
        records[prefix] = _fields(matches[0], keys, prefix, errors)
    if len(records) != 4:
        return errors

    source = records["S98_S3_SOURCE"]
    _same(source, "fixture", expected_fixture, "source", errors)
    _same(source, "source_bytes", str(descriptor["input"]["bytes"]), "source", errors)
    _same(source, "source_sha256", descriptor["input"]["sha256"], "source", errors)

    parser = descriptor["parser"]
    meta = records["S98_S3_META"]
    for key, expected in {
        "fixture": expected_fixture, "s98_version": str(parser["s98_version"]),
        "device_count": str(parser["device_count"]), "device_type": str(parser["device_type"]),
        "declared_clock": str(parser["declared_device_clock_hz"]),
        "effective_clock": str(parser["effective_opngen_clock_hz"]),
        "clock_policy": parser["clock_policy"],
        "raw_timer": f"{parser['timer_numerator']}/{parser['timer_denominator']}",
        "effective_timer": f"{parser['timer_numerator']}/{parser['timer_denominator']}",
        "data_offset": str(parser["data_offset"]), "tag_offset": str(parser["tag_offset"]),
        "loop_offset": str(parser["loop_offset"]), "source_writes": str(parser["source_write_count"]),
        "ignored_writes": str(parser["ignored_write_count"]),
        "final_sync": str(parser["final_sync_count"]), "end_frame": str(parser["end_frame"]),
    }.items():
        _same(meta, key, expected, "meta", errors)

    event = descriptor["event"]
    events = records["S98_S3_EVENTS"]
    _same(events, "fixture", expected_fixture, "events", errors)
    for view in ("preflight", "producer", "consumer"):
        _same(events, f"{view}_count", str(event["count"]), "events", errors)
        _same(events, f"{view}_crc32", event["crc32"][2:], "events", errors)
        _same(events, f"{view}_sha256", event["sha256"], "events", errors)
    _same(events, "sequence_errors", "0", "events", errors)

    pcm = descriptor["pcm"]
    pcm_values = records["S98_S3_PCM"]
    _same(pcm_values, "fixture", expected_fixture, "pcm", errors)
    for key, expected in {
        "frames": str(pcm["frames"]), "bytes": str(pcm["bytes"]),
        "crc32": pcm["crc32"][2:], "sha256": pcm["sha256"],
    }.items():
        _same(pcm_values, key, expected, "pcm", errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, default=DESCRIPTOR_PATH)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    try:
        descriptor = load_descriptor(args.descriptor)
        errors = validate_text(args.log.read_text(encoding="utf-8"), descriptor)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"RETROFM_PLAYBACK=FAIL reason={error}")
        return 1
    if errors:
        print("RETROFM_PLAYBACK=FAIL")
        for error in errors:
            print(f"  {error}")
        return 1
    print("RETROFM_PLAYBACK=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
