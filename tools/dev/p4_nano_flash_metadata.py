#!/usr/bin/env python3
"""Strict parser for ESP-IDF generated flasher_args.json metadata.

The parser is intentionally independent of the build system.  It is shared by
the exact-flash writer and the post-flash app identity verifier so both tools
select the same generated app entry and payload layout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn


class MetadataError(ValueError):
    """Raised when generated flash metadata cannot be used safely."""


@dataclass(frozen=True)
class Payload:
    role: str
    offset: int
    path: Path
    byte_count: int
    sha256: str


@dataclass(frozen=True)
class FlashPlan:
    build_dir: Path
    metadata_path: Path
    payloads: tuple[Payload, ...]
    app: Payload
    chip: str
    before: str
    after: str
    write_flash_args: tuple[str, ...]
    variant: str | None
    board: str | None
    audio_profile: str | None
    audio_opt: str | None
    elf: Payload | None
    map_file: Payload | None


def _fail(message: str) -> NoReturn:
    raise MetadataError(message)


def _parse_offset(value: Any, label: str) -> int:
    if isinstance(value, bool):
        _fail(f"{label} is boolean")
    if isinstance(value, int):
        result = value
    elif isinstance(value, str):
        text = value.strip()
        if not text:
            _fail(f"{label} is empty")
        try:
            result = int(text, 0 if text.lower().startswith("0x") else 10)
        except ValueError:
            _fail(f"{label} is not an integer: {value!r}")
    else:
        _fail(f"{label} has an unsupported type")
    if result < 0:
        _fail(f"{label} is negative")
    return result


def _relative_payload(build_dir: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        _fail(f"{label} is missing or not a string")
    relative = Path(value)
    if relative.is_absolute():
        _fail(f"{label} must be relative to the build directory")
    if any(ord(char) < 0x20 for char in value):
        _fail(f"{label} contains a control character")
    try:
        path = (build_dir / relative).resolve()
    except (OSError, ValueError) as exc:
        _fail(f"{label} is not a valid relative path: {exc}")
    try:
        path.relative_to(build_dir)
    except ValueError:
        _fail(f"{label} escapes the build directory")
    try:
        mode = path.stat().st_mode
    except (OSError, ValueError) as exc:
        _fail(f"{label} does not exist: {path}: {exc}")
    if not stat.S_ISREG(mode):
        _fail(f"{label} is not a regular file: {path}")
    return path


def _identity(path: Path) -> tuple[int, str]:
    try:
        data = path.read_bytes()
        return len(data), hashlib.sha256(data).hexdigest()
    except OSError as exc:
        _fail(f"cannot read payload {path}: {exc}")


def _cache_values(build_dir: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return {}
    values: dict[str, str] = {}
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        _fail(f"cannot read {cache_path}: {exc}")
    for line in lines:
        if line.startswith("#") or ":" not in line or "=" not in line:
            continue
        name_type, value = line.split("=", 1)
        name = name_type.split(":", 1)[0]
        values[name] = value
    return values


def _marker(build_dir: Path, name: str) -> str | None:
    path = build_dir / name
    if not path.is_file():
        return None
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        _fail(f"cannot read marker {path}: {exc}")
    if not value or "\n" in value or "\r" in value:
        _fail(f"marker {path} is empty or multiline")
    return value


def _settings(metadata: dict[str, Any]) -> tuple[str, ...]:
    raw_args = metadata.get("write_flash_args")
    if not isinstance(raw_args, list) or not raw_args:
        _fail("write_flash_args is missing or not a non-empty list")
    args: list[str] = []
    for index, value in enumerate(raw_args):
        if not isinstance(value, str) or not value or any(ord(char) < 0x20 for char in value):
            _fail(f"write_flash_args[{index}] is invalid")
        args.append(value)

    settings = metadata.get("flash_settings")
    if not isinstance(settings, dict):
        _fail("flash_settings is missing or not an object")
    for key in ("flash_mode", "flash_freq", "flash_size"):
        value = settings.get(key)
        if not isinstance(value, str) or not value or any(ord(char) < 0x20 for char in value):
            _fail(f"flash_settings.{key} is missing or invalid")
        option = f"--{key}"
        matches = [index for index, arg in enumerate(args) if arg == option]
        if len(matches) != 1 or matches[0] + 1 >= len(args):
            _fail(f"write_flash_args does not contain exactly one {option}")
        if args[matches[0] + 1] != value:
            _fail(f"write_flash_args {option} disagrees with flash_settings")
    return tuple(args)


def _top_level_entry(metadata: dict[str, Any], name: str) -> tuple[int, str] | None:
    value = metadata.get(name)
    if value is None:
        return None
    if not isinstance(value, dict) or set(("offset", "file")) - value.keys():
        _fail(f"{name} entry must contain offset and file")
    return _parse_offset(value["offset"], f"{name}.offset"), value["file"]


def parse_flash_plan(
    build_dir_value: str | os.PathLike[str],
    *,
    expected_variant: str | None = None,
    expected_board: str | None = None,
) -> FlashPlan:
    build_dir = Path(build_dir_value).resolve()
    if not build_dir.is_dir():
        _fail(f"build directory does not exist: {build_dir}")
    metadata_path = build_dir / "flasher_args.json"
    if not metadata_path.is_file():
        _fail(f"generated metadata is missing: {metadata_path}")
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail(f"cannot parse {metadata_path}: {exc}")
    if not isinstance(metadata, dict):
        _fail("metadata top-level value is not an object")

    raw_flash_files = metadata.get("flash_files")
    if not isinstance(raw_flash_files, dict) or not raw_flash_files:
        _fail("flash_files is missing or not a non-empty object")

    top_entries: dict[int, tuple[str, str]] = {}
    for name in ("bootloader", "partition-table", "app"):
        entry = _top_level_entry(metadata, name)
        if entry is None:
            continue
        offset, file_value = entry
        if not isinstance(file_value, str) or not file_value.strip():
            _fail(f"{name}.file is missing or invalid")
        if offset in top_entries:
            _fail(f"duplicate offset in top-level payload entries: 0x{offset:x}")
        top_entries[offset] = (name, file_value)

    app_entry = _top_level_entry(metadata, "app")
    if app_entry is None:
        _fail("explicit app entry is missing")
    app_offset, app_file = app_entry

    payloads: list[Payload] = []
    seen_offsets: set[int] = set()
    seen_paths: set[Path] = set()
    app_matches = 0
    for raw_offset, raw_file in raw_flash_files.items():
        offset = _parse_offset(raw_offset, "flash_files offset")
        if offset in seen_offsets:
            _fail(f"duplicate flash_files offset: 0x{offset:x}")
        seen_offsets.add(offset)
        path = _relative_payload(build_dir, raw_file, f"flash_files[0x{offset:x}]")
        if path in seen_paths:
            _fail(f"payload path is duplicated at multiple offsets: {path}")
        seen_paths.add(path)
        role_info = top_entries.get(offset)
        if role_info is not None:
            role, expected_file = role_info
            if expected_file != raw_file:
                _fail(f"{role} and flash_files disagree about the payload file")
        else:
            role = "payload"
        if offset == app_offset:
            app_matches += 1
            if raw_file != app_file:
                _fail("app and flash_files disagree about the application file")
            role = "app"
        byte_count, sha256 = _identity(path)
        payloads.append(Payload(role, offset, path, byte_count, sha256))

    if app_matches != 1:
        _fail("app offset is missing or ambiguous in flash_files")
    app_payload = next(payload for payload in payloads if payload.offset == app_offset)
    if app_payload.path in {payload.path for payload in payloads if payload is not app_payload}:
        _fail("app payload aliases another flash payload")

    extra = metadata.get("extra_esptool_args")
    if not isinstance(extra, dict):
        _fail("extra_esptool_args is missing or not an object")
    chip = extra.get("chip")
    before = extra.get("before")
    after = extra.get("after")
    for name, value in (("chip", chip), ("before", before), ("after", after)):
        if not isinstance(value, str) or not value or any(char in value for char in "\x00\t\r\n"):
            _fail(f"extra_esptool_args.{name} is missing or invalid")

    write_flash_args = _settings(metadata)
    cache = _cache_values(build_dir)
    target = cache.get("IDF_TARGET")
    if target != "esp32p4":
        _fail(f"build cache IDF_TARGET is {target!r}, expected 'esp32p4'")
    variant = _marker(build_dir, ".p4-production-variant")
    board = _marker(build_dir, ".p4-production-board")
    if expected_variant is not None and variant != expected_variant:
        _fail(f"variant marker is {variant!r}, expected {expected_variant!r}")
    if expected_board is not None and board != expected_board:
        _fail(f"board marker is {board!r}, expected {expected_board!r}")
    if expected_variant is not None and variant is None:
        _fail("variant marker is missing")
    if expected_board is not None and board is None:
        _fail("board marker is missing")

    def optional_provenance(name: str, role: str) -> Payload | None:
        path = build_dir / name
        if not path.is_file():
            return None
        resolved = path.resolve()
        try:
            resolved.relative_to(build_dir)
        except ValueError:
            return None
        if not stat.S_ISREG(resolved.stat().st_mode):
            return None
        byte_count, sha256 = _identity(resolved)
        return Payload(role, -1, resolved, byte_count, sha256)

    return FlashPlan(
        build_dir=build_dir,
        metadata_path=metadata_path,
        payloads=tuple(sorted(payloads, key=lambda payload: payload.offset)),
        app=app_payload,
        chip=chip,
        before=before,
        after=after,
        write_flash_args=write_flash_args,
        variant=variant,
        board=board,
        audio_profile=cache.get("P4_NANO_AUDIO_ONLY_BENCHMARK_PROFILE"),
        audio_opt=cache.get("P4_NANO_AUDIO_OPT"),
        elf=optional_provenance("esp_np2kai.elf", "elf"),
        map_file=optional_provenance("esp_np2kai.map", "map"),
    )


def app_identity_fields(plan: FlashPlan) -> tuple[str, int, int, str, str]:
    return str(plan.app.path), plan.app.offset, plan.app.byte_count, plan.app.sha256, plan.chip


def _cli() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--app-identity", action="store_true")
    args = parser.parse_args()
    try:
        plan = parse_flash_plan(args.build_dir)
    except MetadataError as exc:
        print(f"metadata invalid: {exc}", file=sys.stderr)
        return 2
    if args.app_identity:
        fields = app_identity_fields(plan)
        print("\t".join((fields[0], str(fields[1]), str(fields[2]), fields[3], fields[4])))
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
