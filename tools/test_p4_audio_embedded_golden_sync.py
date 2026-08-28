#!/usr/bin/env python3
"""Verify firmware STRESS-60 identity constants against the formal golden."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORMAL_DEFAULT = ROOT / "tools" / "emu" / "opngen_sustained_workload_golden.json"
SOURCE_DEFAULT = (
    ROOT
    / "firmware"
    / "components"
    / "p4_nano_audio_benchmark"
    / "p4_nano_audio_benchmark.cpp"
)
SHA_FIELDS = ("event_sha256", "pcm_sha256")


def fail(field: str, message: str) -> None:
    raise AssertionError(f"{field}: {message}")


def parse_sha(field: str, value: str) -> bytes:
    if not isinstance(value, str) or len(value) != 64:
        fail(field, f"expected 64 hex characters, got {value!r}")
    if re.fullmatch(r"[0-9a-fA-F]{64}", value) is None:
        fail(field, "invalid hexadecimal syntax")
    try:
        parsed = bytes.fromhex(value)
    except ValueError as exc:
        fail(field, f"bytes.fromhex failed: {exc}")
    if len(parsed) != 32:
        fail(field, f"expected 32 bytes, got {len(parsed)}")
    return parsed


def parse_crc(field: str, value: str) -> int:
    if not isinstance(value, str) or re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", value) is None:
        fail(field, f"invalid numeric form: {value!r}")
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        fail(field, f"invalid numeric value: {exc}")
    if not 0 <= parsed <= 0xFFFFFFFF:
        fail(field, f"outside uint32 range: {parsed}")
    return parsed


def first_difference(left: bytes, right: bytes) -> tuple[int | None, int]:
    differences = [(i, a, b) for i, (a, b) in enumerate(zip(left, right)) if a != b]
    if not differences and len(left) == len(right):
        return None, 0
    index = differences[0][0] if differences else min(len(left), len(right))
    return index, len(differences) + abs(len(left) - len(right))


def require_bytes_equal(field: str, embedded: bytes, formal: bytes) -> None:
    if embedded == formal:
        return
    index, count = first_difference(embedded, formal)
    embedded_byte = embedded[index] if index is not None and index < len(embedded) else None
    formal_byte = formal[index] if index is not None and index < len(formal) else None
    fail(
        field,
        f"first differing byte index={index} embedded_byte={embedded_byte} "
        f"formal_byte={formal_byte} differing_byte_count={count}",
    )


def extract_array(source: str, name: str) -> bytes:
    match = re.search(
        rf"static const uint8_t {name}\[\] = \{{(.*?)\}};", source, re.DOTALL
    )
    if match is None:
        fail(name, "initializer not found")
    values = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{1,2})", match.group(1))]
    if len(values) != 32:
        fail(name, f"expected exactly 32 initializer bytes, got {len(values)}")
    return bytes(values)


def extract_stress_scalars(source: str) -> tuple[int, int, int, int]:
    match = re.search(
        r'static const Workload kStress = \{\s*"STRESS-60",\s*false,.*?\{\s*'
        r'(\d+)U,\s*(\d+)U,\s*(0[xX][0-9a-fA-F]+)U,\s*\{\},\s*'
        r'(0[xX][0-9a-fA-F]+)U,\s*\{\}\}\};',
        source,
        re.DOTALL,
    )
    if match is None:
        fail("kStress", "scalar initializer not found")
    event_count, end_frame, event_crc, pcm_crc = match.groups()
    return int(event_count), int(end_frame), int(event_crc, 0), int(pcm_crc, 0)


def run(formal_path: Path, source_path: Path) -> None:
    formal = json.loads(formal_path.read_text())
    case = formal.get("cases", {}).get("STRESS-60")
    if not isinstance(case, dict):
        fail("STRESS-60", "case missing")
    if case.get("profile") != "STRESS":
        fail("profile", f"expected STRESS, got {case.get('profile')!r}")
    for field, expected in (
        ("duration_frames", 2880000),
        ("event_count", 41127),
        ("pcm_frames", 2880000),
        ("pcm_bytes", 11520000),
    ):
        if case.get(field) != expected:
            fail(field, f"expected {expected}, got {case.get(field)!r}")
    event_crc = parse_crc("event_crc32", case.get("event_crc32"))
    pcm_crc = parse_crc("pcm_crc32", case.get("pcm_crc32"))
    formal_sha = {field: parse_sha(field, case.get(field)) for field in SHA_FIELDS}
    if case["pcm_bytes"] != case["pcm_frames"] * 4:
        fail("pcm_bytes", "does not equal pcm_frames * 4")

    source = source_path.read_text()
    embedded_sha = {
        "event_sha256": extract_array(source, "kStressEventSha"),
        "pcm_sha256": extract_array(source, "kStressPcmSha"),
    }
    event_count, end_frame, embedded_event_crc, embedded_pcm_crc = extract_stress_scalars(source)
    if event_count != case["event_count"]:
        fail("event_count", f"embedded={event_count} formal={case['event_count']}")
    if end_frame != case["pcm_frames"] or end_frame != case["duration_frames"]:
        fail("end_frame", f"embedded={end_frame} formal_frames={case['pcm_frames']}")
    if embedded_event_crc != event_crc:
        fail("event_crc32", f"embedded=0x{embedded_event_crc:08x} formal=0x{event_crc:08x}")
    if embedded_pcm_crc != pcm_crc:
        fail("pcm_crc32", f"embedded=0x{embedded_pcm_crc:08x} formal=0x{pcm_crc:08x}")
    for field in SHA_FIELDS:
        require_bytes_equal(field, embedded_sha[field], formal_sha[field])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--formal", type=Path, default=FORMAL_DEFAULT)
    parser.add_argument("--source", type=Path, default=SOURCE_DEFAULT)
    args = parser.parse_args()
    try:
        run(args.formal, args.source)
    except (AssertionError, OSError, json.JSONDecodeError) as exc:
        print(f"P4_AUDIO_EMBEDDED_GOLDEN_SYNC=FAIL {exc}", file=sys.stderr)
        return 1
    print("P4_AUDIO_EMBEDDED_GOLDEN_SYNC=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
