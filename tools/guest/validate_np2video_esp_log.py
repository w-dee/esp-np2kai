#!/usr/bin/env python3
"""Validate the ESP np2video machine-readable emulator log."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


class ValidationError(ValueError):
    """Raised when an ESP np2video log violates the golden contract."""


FIXTURE_ID_PATTERN = re.compile(r"[A-Za-z0-9._-]{1,63}")


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValidationError(f"{name} must be a non-negative integer")
    return value


def _load_descriptor(path: Path) -> dict[str, Any]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot read golden descriptor: {error}") from error
    if not isinstance(root, dict) or root.get("schema_version") != 1:
        raise ValidationError("golden descriptor must be schema v1")

    required = (
        "fixture_id", "scene_id", "fixture_sha256", "image_size", "width", "height",
        "pixel_format", "bpp", "pitch", "visible_bytes", "crc_algorithm",
        "crc32", "synchronization",
    )
    missing = [name for name in required if name not in root]
    if missing:
        raise ValidationError(f"golden descriptor is missing {', '.join(missing)}")
    fixture_id = root["fixture_id"]
    if (not isinstance(fixture_id, str) or
            FIXTURE_ID_PATTERN.fullmatch(fixture_id) is None):
        raise ValidationError(
            "fixture_id must contain 1-63 ASCII letters, digits, '.', '_' or '-'")
    digest = root["fixture_sha256"]
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise ValidationError("fixture_sha256 is not lowercase hexadecimal SHA-256")
    crc = root["crc32"]
    if not isinstance(crc, str) or re.fullmatch(r"0x[0-9a-fA-F]{8}", crc) is None:
        raise ValidationError("crc32 is not an 8-digit hexadecimal value")
    if root["pixel_format"] != "rgb565le":
        raise ValidationError("unsupported golden pixel_format")
    if root["crc_algorithm"] != "crc32_iso_hdlc":
        raise ValidationError("unsupported golden crc_algorithm")
    synchronization = root["synchronization"]
    if not isinstance(synchronization, dict) or synchronization.get("ready_state") != "SCENE_READY":
        raise ValidationError("golden synchronization ready_state must be SCENE_READY")

    values = {
        name: _integer(root[name], name)
        for name in (
            "scene_id", "image_size", "width", "height", "bpp", "pitch",
            "visible_bytes",
        )
    }
    values.update({
        "fixture_id": fixture_id,
        "fixture_sha256": digest,
        "pixel_format": root["pixel_format"],
        "crc_algorithm": root["crc_algorithm"],
        "crc32": int(crc, 16),
        "ready_state": synchronization["ready_state"],
    })
    return values


def _parse_fields(line: str, marker: str, required: set[str], *, exact: bool) -> dict[str, str]:
    if line == marker:
        raise ValidationError(f"{marker} has no fields")
    prefix = marker + " "
    if not line.startswith(prefix):
        raise ValidationError(f"malformed {marker} line")
    fields: dict[str, str] = {}
    for token in line[len(prefix):].split():
        if "=" not in token:
            raise ValidationError(f"malformed field in {marker}: {token}")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValidationError(f"malformed field in {marker}: {token}")
        fields[key] = value
    if not required.issubset(fields):
        missing = sorted(required - fields.keys())
        raise ValidationError(f"{marker} is missing fields: {', '.join(missing)}")
    if exact and set(fields) != required:
        unexpected = sorted(set(fields) - required)
        raise ValidationError(f"{marker} has unexpected fields: {', '.join(unexpected)}")
    return fields


def _field_int(fields: dict[str, str], name: str) -> int:
    value = fields[name]
    if re.fullmatch(r"0|[1-9][0-9]*", value) is None:
        raise ValidationError(f"{name} is not a decimal integer")
    return int(value, 10)


def _field_crc(fields: dict[str, str]) -> int:
    value = fields["crc32"]
    if re.fullmatch(r"0x[0-9a-fA-F]{8}", value) is None:
        raise ValidationError("crc32 is not an 8-digit hexadecimal value")
    return int(value, 16)


def _one(lines: dict[str, list[dict[str, str]]], marker: str) -> dict[str, str]:
    values = lines.get(marker, [])
    if len(values) != 1:
        raise ValidationError(f"expected exactly one {marker} line, got {len(values)}")
    return values[0]


def validate_text(text: str, descriptor: dict[str, Any]) -> None:
    markers = {
        "NP2VIDEO_PROFILE": {"profile", "formal_extmem", "effective_extmem"},
        "NP2VIDEO_MEMORY": {"extmem_mb", "actual_bytes", "ptr_external"},
        "NP2VIDEO_FIXTURE": {"fixture_id", "scene_id", "fixture_sha256", "image_bytes", "partition"},
        "NP2VIDEO_READY": {"fixture_id", "scene_id", "state", "generation", "surface_update_sequence"},
        "NP2VIDEO_FRAMEBUFFER": {
            "fixture_id", "scene_id", "width", "height", "bytes", "format", "bpp", "pitch",
            "generation", "surface_update_sequence", "crc_algorithm", "crc32",
            "storage_external",
        },
    }
    parsed: dict[str, list[dict[str, str]]] = {}
    line_positions: dict[str, int] = {}
    terminals: list[str] = []

    for position, raw_line in enumerate(text.splitlines()):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("NP2VIDEO_GOLDEN_RESULT"):
            if re.fullmatch(
                r"NP2VIDEO_GOLDEN_RESULT=(PASS|FAIL|HARNESS_ERROR)(?: reason=[^ ]+)?",
                line,
            ) is None:
                raise ValidationError("malformed NP2VIDEO_GOLDEN_RESULT line")
            terminals.append(line)
            continue

        matched = False
        for marker, required in markers.items():
            if line == marker or line.startswith(marker + " "):
                if marker in parsed:
                    raise ValidationError(f"duplicate {marker} line")
                parsed[marker] = [_parse_fields(line, marker, required, exact=marker != "NP2VIDEO_MEMORY")]
                line_positions[marker] = position
                matched = True
                break
        if not matched and line.startswith("NP2VIDEO_"):
            raise ValidationError(f"unexpected or malformed NP2VIDEO line: {line}")

    if len(terminals) != 1:
        raise ValidationError(f"expected exactly one terminal result, got {len(terminals)}")
    if terminals[0] != "NP2VIDEO_GOLDEN_RESULT=PASS":
        raise ValidationError(f"terminal result is not PASS: {terminals[0]}")

    profile = _one(parsed, "NP2VIDEO_PROFILE")
    if profile["profile"] != "esp32p4-reduced-video":
        raise ValidationError("firmware profile is not hardware-neutral")
    if profile["formal_extmem"] != "13" or profile["effective_extmem"] != "8":
        raise ValidationError("profile EXTMEM contract is invalid")

    memory = _one(parsed, "NP2VIDEO_MEMORY")
    if (_field_int(memory, "extmem_mb") != 8 or
            _field_int(memory, "actual_bytes") != 8 * 1024 * 1024 or
            memory["ptr_external"] != "1"):
        raise ValidationError("external EXTMEM evidence is invalid")

    fixture = _one(parsed, "NP2VIDEO_FIXTURE")
    if (fixture["fixture_id"] != descriptor["fixture_id"] or
            _field_int(fixture, "scene_id") != descriptor["scene_id"] or
            fixture["fixture_sha256"] != descriptor["fixture_sha256"] or
            _field_int(fixture, "image_bytes") != descriptor["image_size"] or
            fixture["partition"] != "np2test"):
        raise ValidationError("fixture identity does not match golden.json")

    ready = _one(parsed, "NP2VIDEO_READY")
    if (ready["fixture_id"] != descriptor["fixture_id"] or
            _field_int(ready, "scene_id") != descriptor["scene_id"] or
            ready["state"] != descriptor["ready_state"]):
        raise ValidationError("READY state does not match golden synchronization contract")
    ready_generation = _field_int(ready, "generation")
    ready_sequence = _field_int(ready, "surface_update_sequence")

    framebuffer = _one(parsed, "NP2VIDEO_FRAMEBUFFER")
    if line_positions["NP2VIDEO_READY"] > line_positions["NP2VIDEO_FRAMEBUFFER"]:
        raise ValidationError("framebuffer line precedes READY line")
    expected_framebuffer = {
        "fixture_id": descriptor["fixture_id"],
        "scene_id": descriptor["scene_id"],
        "width": descriptor["width"],
        "height": descriptor["height"],
        "bytes": descriptor["visible_bytes"],
        "bpp": descriptor["bpp"],
        "pitch": descriptor["pitch"],
    }
    if framebuffer["fixture_id"] != expected_framebuffer["fixture_id"]:
        raise ValidationError("framebuffer fixture_id does not match golden.json")
    for name, expected in expected_framebuffer.items():
        if name == "fixture_id":
            continue
        if _field_int(framebuffer, name) != expected:
            raise ValidationError(f"framebuffer {name} does not match golden.json")
    if framebuffer["format"] != descriptor["pixel_format"]:
        raise ValidationError("framebuffer pixel format does not match golden.json")
    if framebuffer["crc_algorithm"] != descriptor["crc_algorithm"]:
        raise ValidationError("framebuffer CRC algorithm does not match golden.json")
    if _field_crc(framebuffer) != descriptor["crc32"]:
        raise ValidationError("framebuffer CRC does not match golden.json")
    if framebuffer["storage_external"] != "1":
        raise ValidationError("framebuffer storage is not external")
    if _field_int(framebuffer, "generation") != ready_generation:
        raise ValidationError("framebuffer generation changed after READY")
    if _field_int(framebuffer, "surface_update_sequence") <= ready_sequence:
        raise ValidationError("surface update sequence did not advance after READY")


def validate(descriptor_path: Path, log_path: Path) -> None:
    descriptor = _load_descriptor(descriptor_path)
    try:
        text = log_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValidationError(f"cannot read emulator log: {error}") from error
    validate_text(text, descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.descriptor, args.log)
    except ValidationError as error:
        print(f"NP2VIDEO_VALIDATION=FAIL reason={error}")
        return 1
    print("NP2VIDEO_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
