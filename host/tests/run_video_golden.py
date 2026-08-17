#!/usr/bin/env python3
"""Validate a selected np2video reference against its tracked descriptor."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional, Sequence


class GoldenError(ValueError):
    """Raised when the golden descriptor or runner output is invalid."""


REQUIRED_DESCRIPTOR_FIELDS = (
    "schema_version",
    "fixture_id",
    "scene_id",
    "fixture_sha256",
    "image_size",
    "width",
    "height",
    "pixel_format",
    "bpp",
    "pitch",
    "visible_bytes",
    "crc_algorithm",
    "crc32",
)
KEY_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
FIXTURE_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,63}$")


def load_descriptor(path: Path) -> dict[str, Any]:
    try:
        descriptor = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GoldenError(f"cannot read golden descriptor: {error}") from error
    if not isinstance(descriptor, dict):
        raise GoldenError("golden descriptor must be an object")
    missing = [field for field in REQUIRED_DESCRIPTOR_FIELDS if field not in descriptor]
    if missing:
        raise GoldenError(f"golden descriptor is missing fields: {', '.join(missing)}")
    if descriptor["schema_version"] != 1:
        raise GoldenError("unsupported golden descriptor schema")
    if (not isinstance(descriptor["fixture_id"], str) or
            not FIXTURE_ID_PATTERN.fullmatch(descriptor["fixture_id"])):
        raise GoldenError("fixture_id must contain only ASCII letters, digits, '.', '_' or '-'")
    for field in ("scene_id", "image_size", "width", "height", "bpp", "pitch", "visible_bytes"):
        if isinstance(descriptor[field], bool) or not isinstance(descriptor[field], int):
            raise GoldenError(f"{field} must be an integer")
    if not 0 <= descriptor["scene_id"] <= 65535:
        raise GoldenError("scene_id must fit in uint16")
    if not SHA256_PATTERN.fullmatch(descriptor["fixture_sha256"]):
        raise GoldenError("fixture_sha256 must be lowercase SHA-256")
    if not isinstance(descriptor["pixel_format"], str) or not descriptor["pixel_format"]:
        raise GoldenError("pixel_format must be a non-empty string")
    if not isinstance(descriptor["crc_algorithm"], str) or not descriptor["crc_algorithm"]:
        raise GoldenError("crc_algorithm must be a non-empty string")
    if (not isinstance(descriptor["crc32"], str) or
            not re.fullmatch(r"0x[0-9a-fA-F]{8}", descriptor["crc32"])):
        raise GoldenError("crc32 must be an 8-digit hexadecimal string")
    synchronization = descriptor.get("synchronization")
    if synchronization != {
        "ready_state": "SCENE_READY",
        "generation": "must_remain_stable",
        "surface_update_sequence": "must_increase_after_ready",
    }:
        raise GoldenError("synchronization policy is invalid")
    if descriptor.get("bmp") != "diagnostic-only":
        raise GoldenError("BMP must remain diagnostic-only")
    return descriptor


def _parse_fields(line: str, prefix: str) -> dict[str, str]:
    parts = line.split()
    if not parts or parts[0] != prefix:
        raise GoldenError(f"malformed {prefix} line")
    fields: dict[str, str] = {}
    for token in parts[1:]:
        if token.count("=") != 1:
            raise GoldenError(f"malformed field in {prefix}: {token}")
        key, value = token.split("=", 1)
        if not KEY_PATTERN.fullmatch(key) or not value or key in fields:
            raise GoldenError(f"malformed or duplicate field in {prefix}: {token}")
        fields[key] = value
    return fields


def _integer(fields: dict[str, str], key: str, line_name: str) -> int:
    value = fields.get(key)
    if value is None:
        raise GoldenError(f"{line_name} is missing {key}")
    try:
        return int(value, 0)
    except ValueError as error:
        raise GoldenError(f"{line_name} has a non-numeric {key}") from error


def _require(fields: dict[str, str], key: str, line_name: str) -> str:
    value = fields.get(key)
    if value is None or value == "":
        raise GoldenError(f"{line_name} is missing {key}")
    return value


def validate_stdout(stdout: bytes, descriptor: dict[str, Any]) -> None:
    try:
        lines = stdout.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise GoldenError("runner output is not ASCII") from error
    if any(not line for line in lines):
        raise GoldenError("runner output contains an empty line")

    fixture_lines = [line for line in lines if line.startswith("NP2VIDEO_FIXTURE ")]
    ready_lines = [line for line in lines if line.startswith("NP2VIDEO_READY ")]
    framebuffer_lines = [line for line in lines if line.startswith("NP2VIDEO_FRAMEBUFFER ")]
    result_lines = [line for line in lines if line.startswith("NP2VIDEO_RESULT=")]
    known_lines = set(fixture_lines + ready_lines + framebuffer_lines + result_lines)
    if len(known_lines) != len(lines):
        raise GoldenError("runner output contains an unexpected or malformed line")
    if len(fixture_lines) != 1 or len(ready_lines) != 1 or len(framebuffer_lines) != 1:
        raise GoldenError("runner output must contain one fixture, READY, and framebuffer line")
    if result_lines != ["NP2VIDEO_RESULT=REFERENCE_READY"]:
        raise GoldenError("runner did not emit exactly one REFERENCE_READY result")

    fixture = _parse_fields(fixture_lines[0], "NP2VIDEO_FIXTURE")
    ready = _parse_fields(ready_lines[0], "NP2VIDEO_READY")
    framebuffer = _parse_fields(framebuffer_lines[0], "NP2VIDEO_FRAMEBUFFER")

    for fields, line_name in ((fixture, "NP2VIDEO_FIXTURE"),
                              (ready, "NP2VIDEO_READY"),
                              (framebuffer, "NP2VIDEO_FRAMEBUFFER")):
        _integer(fields, "scene_id", line_name)
    if _integer(fixture, "scene_id", "NP2VIDEO_FIXTURE") != descriptor["scene_id"]:
        raise GoldenError("fixture scene_id mismatch")
    if _require(fixture, "fixture_id", "NP2VIDEO_FIXTURE") != descriptor["fixture_id"]:
        raise GoldenError("fixture_id mismatch")
    if _require(fixture, "fixture_sha256", "NP2VIDEO_FIXTURE") != descriptor["fixture_sha256"]:
        raise GoldenError("fixture SHA-256 mismatch")
    if _integer(fixture, "image_bytes", "NP2VIDEO_FIXTURE") != descriptor["image_size"]:
        raise GoldenError("fixture image size mismatch")

    if _integer(ready, "scene_id", "NP2VIDEO_READY") != descriptor["scene_id"]:
        raise GoldenError("READY scene_id mismatch")
    if _require(ready, "fixture_id", "NP2VIDEO_READY") != descriptor["fixture_id"]:
        raise GoldenError("READY fixture_id mismatch")
    if _require(ready, "state", "NP2VIDEO_READY") != "SCENE_READY":
        raise GoldenError("READY state is not SCENE_READY")
    ready_generation = _integer(ready, "generation", "NP2VIDEO_READY")
    ready_sequence = _integer(ready, "surface_update_sequence", "NP2VIDEO_READY")
    if ready_generation < 1 or ready_sequence < 0:
        raise GoldenError("invalid READY synchronization diagnostics")

    expected_framebuffer = {
        "scene_id": descriptor["scene_id"],
        "width": descriptor["width"],
        "height": descriptor["height"],
        "format": descriptor["pixel_format"],
        "bpp": descriptor["bpp"],
        "pitch": descriptor["pitch"],
        "bytes": descriptor["visible_bytes"],
        "crc_algorithm": descriptor["crc_algorithm"],
    }
    for key, expected in expected_framebuffer.items():
        if key == "format":
            actual: object = _require(framebuffer, key, "NP2VIDEO_FRAMEBUFFER")
        elif key == "crc_algorithm":
            actual = _require(framebuffer, key, "NP2VIDEO_FRAMEBUFFER")
        else:
            actual = _integer(framebuffer, key, "NP2VIDEO_FRAMEBUFFER")
        if actual != expected:
            raise GoldenError(f"framebuffer {key} mismatch: expected {expected}, got {actual}")
    if _require(framebuffer, "fixture_id", "NP2VIDEO_FRAMEBUFFER") != descriptor["fixture_id"]:
        raise GoldenError("framebuffer fixture_id mismatch")
    if _require(framebuffer, "crc32", "NP2VIDEO_FRAMEBUFFER").lower() != descriptor["crc32"].lower():
        raise GoldenError("framebuffer CRC mismatch")
    framebuffer_generation = _integer(framebuffer, "generation", "NP2VIDEO_FRAMEBUFFER")
    framebuffer_sequence = _integer(framebuffer, "surface_update_sequence", "NP2VIDEO_FRAMEBUFFER")
    if framebuffer_generation != ready_generation:
        raise GoldenError("surface generation changed after READY")
    if framebuffer_sequence <= ready_sequence:
        raise GoldenError("no framebuffer update occurred after READY")


def _run_child(runner: str, fixture: str, descriptor: dict[str, Any],
		timeout: float) -> tuple[int, bytes, bytes]:
    try:
        process = subprocess.Popen(
            [runner, fixture, "--fixture-id", descriptor["fixture_id"],
			 "--scene-id", str(descriptor["scene_id"])],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise GoldenError(f"cannot start video_runner: {error}") from error
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        process.kill()
        stdout, stderr = process.communicate()
        raise GoldenError("video_runner timed out") from error
    return process.returncode, stdout, stderr


def run_golden(runner: str, fixture: str, descriptor_path: Path, timeout: float) -> None:
    descriptor = load_descriptor(descriptor_path)
    returncode, stdout, stderr = _run_child(runner, fixture, descriptor, timeout)
    if stderr:
        sys.stderr.buffer.write(stderr)
        sys.stderr.buffer.flush()
    if returncode != 0:
        raise GoldenError(f"video_runner exited with status {returncode}")
    validate_stdout(stdout, descriptor)
    sys.stdout.buffer.write(stdout)
    sys.stdout.buffer.flush()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("runner")
    parser.add_argument("fixture")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        run_golden(args.runner, args.fixture, args.descriptor, args.timeout)
    except GoldenError as error:
        print(f"video golden regression: {error}", file=sys.stderr)
        return 5
    print("NP2VIDEO_GOLDEN_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
