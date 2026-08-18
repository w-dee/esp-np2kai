#!/usr/bin/env python3
"""Validate the deterministic ESP presentation contract log."""

from __future__ import annotations

import argparse
from pathlib import Path


def _one(lines: list[str], prefix: str, errors: list[str]) -> str:
    matches = [line for line in lines if line.startswith(prefix)]
    if len(matches) != 1:
        errors.append(f"expected exactly one {prefix} line, got {len(matches)}")
        return ""
    return matches[0]


def _fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def _require(fields: dict[str, str], expected: dict[str, str],
             label: str, errors: list[str]) -> None:
    for key, value in expected.items():
        if fields.get(key) != value:
            errors.append(
                f"{label}: {key}={fields.get(key)!r}, expected {value!r}"
            )


def validate_text(text: str) -> list[str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    errors: list[str] = []

    init = _one(lines, "NP2PRESENT_INIT", errors)
    basic = _one(lines, "NP2PRESENT_BASIC", errors)
    immutable = _one(lines, "NP2PRESENT_IMMUTABLE", errors)
    coalesce = _one(lines, "NP2PRESENT_COALESCE", errors)
    resize = _one(lines, "NP2PRESENT_RESIZE", errors)
    terminal = [line for line in lines if line.startswith("NP2PRESENT_RESULT=")]
    if terminal != ["NP2PRESENT_RESULT=PASS"]:
        errors.append("expected exactly one terminal NP2PRESENT_RESULT=PASS")
    if any("NP2PRESENT_RESULT=FAIL" in line or
           "NP2PRESENT_RESULT=HARNESS_ERROR" in line for line in lines):
        errors.append("failure or harness-error terminal is present")

    if init:
        _require(_fields(init), {
            "guest_external": "1",
            "slot0_external": "1",
            "slot1_external": "1",
            "slots": "2",
            "slot_bytes": "512000",
            "atomic32_lock_free": "1",
            "slot_state0_lock_free": "1",
            "slot_state1_lock_free": "1",
        }, "INIT", errors)
    if basic:
        _require(_fields(basic), {
            "result": "PASS",
            "guest_external": "1",
            "presentation_external": "1",
            "ptr_distinct": "1",
            "width": "640",
            "height": "400",
            "pitch": "1280",
            "published_sequence": "1",
        }, "BASIC", errors)
    if immutable:
        _require(_fields(immutable), {
            "result": "PASS",
            "guest_locked": "1",
            "unchanged": "1",
        }, "IMMUTABLE", errors)
    if coalesce:
        _require(_fields(coalesce), {
            "result": "PASS",
            "acquired_frame": "4",
            "published_sequence": "4",
            "coalesced_count": "2",
            "dropped_count": "0",
        }, "COALESCE", errors)
    if resize:
        _require(_fields(resize), {
            "result": "PASS",
            "old_generation": "1",
            "new_generation": "2",
            "old_frame_unchanged": "1",
            "width": "320",
            "height": "200",
            "pitch": "640",
            "source_generation": "2",
        }, "RESIZE", errors)

    memory_lines = [line for line in lines
                    if line.startswith("NP2PRESENT_MEMORY")]
    memory_phases = {_fields(line).get("phase") for line in memory_lines}
    for phase in ("before_guest", "after_guest", "after_slots", "after_resize"):
        if phase not in memory_phases:
            errors.append(f"missing memory phase {phase}")
    after_slots = next((line for line in memory_lines
                        if _fields(line).get("phase") == "after_slots"), "")
    if after_slots:
        _require(_fields(after_slots), {
            "guest_external": "1",
            "slot0_external": "1",
            "slot1_external": "1",
            "presentation_bytes": "1024000",
        }, "MEMORY after_slots", errors)

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    errors = validate_text(args.log.read_text(encoding="utf-8", errors="replace"))
    if errors:
        for error in errors:
            print(f"NP2PRESENT_VALIDATION_ERROR {error}")
        print("NP2PRESENT_VALIDATION=FAIL")
        return 1
    print("NP2PRESENT_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
