#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build and structurally validate the deterministic NP2TEST foundation image."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


EXPECTED_GEOMETRY = {
    "cylinders": 77,
    "heads": 2,
    "sectors_per_track": 8,
    "bytes_per_sector": 1024,
}
EXPECTED_NASM = "2.16.01"
EXPECTED_PYTHON = (3, 12)
EXPECTED_SIZE = 1_261_568
EXPECTED_RESULT_SIZE = 128
SIGNATURE = bytes.fromhex("55aa")
PENDING_EXTENSION_STATUS = "pending-np2kai-reference-attachment-validation"
EXPECTED_RESULT_FIELDS = (
    ("magic", 0, 4, "bytes", "included"),
    ("version", 4, 2, "u16le", "included"),
    ("header_size", 6, 2, "u16le", "included"),
    ("block_size", 8, 2, "u16le", "included"),
    ("flags", 10, 2, "u16le", "included"),
    ("suite_id", 12, 4, "u32le", "included"),
    ("build_id", 16, 4, "u32le", "included"),
    ("total_count", 20, 2, "u16le", "included"),
    ("completed_count", 22, 2, "u16le", "included"),
    ("passed_count", 24, 2, "u16le", "included"),
    ("failed_count", 26, 2, "u16le", "included"),
    ("first_failed_id", 28, 2, "u16le", "included"),
    ("diagnostic_length", 30, 2, "u16le", "included"),
    ("diagnostic_data", 32, 64, "utf8-zero-padded-bytes", "included"),
    ("reserved_body", 96, 24, "reserved-zero", "included"),
    ("checksum", 120, 4, "u32le", "excluded"),
    ("state", 124, 1, "u8", "excluded"),
    ("reserved_tail", 125, 3, "reserved-zero", "excluded"),
)
EXPECTED_REGIONS = {
    "ipl": ("ipl", 0x1FC00, 0x400, 0x400),
    "payload-reserve": ("payload", 0x20000, 0x8000, 0x400),
    "stack": ("stack", 0x28000, 0x1000, 0x10),
    "result": ("result", 0x29000, EXPECTED_RESULT_SIZE, 0x10),
}
EXPECTED_EXCLUDED_RANGES = {
    "pc98-low-bios-data": (0x00000, 0x1000),
    "pc98-text-vram": (0xA0000, 0x8000),
    "pc98-graphics-vram-b-r-g": (0xA8000, 0x18000),
    "pc98-graphics-vram-e": (0xE0000, 0x8000),
    "pc98-itf-rom": (0xF8000, 0x8000),
}


class LayoutError(ValueError):
    """Raised when the machine-readable NP2TEST contract is invalid."""


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise LayoutError(f"{name} must be an object")
    return value


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise LayoutError(f"{name} must be an integer")
    return value


def _hex_bytes(value: Any, name: str) -> bytes:
    if not isinstance(value, str) or len(value) % 2:
        raise LayoutError(f"{name} must be an even-length hexadecimal string")
    try:
        return bytes.fromhex(value)
    except ValueError as exc:
        raise LayoutError(f"{name} is not hexadecimal") from exc


def _validate_range(start: Any, size: Any, name: str, address_end: int) -> tuple[int, int]:
    start_value = _integer(start, f"{name}.start")
    size_value = _integer(size, f"{name}.size_bytes")
    if start_value < 0 or size_value <= 0 or start_value + size_value > address_end:
        raise LayoutError(f"{name} is outside the declared address space")
    return start_value, size_value


def _validate_non_overlapping(ranges: list[tuple[int, int, str]], name: str) -> None:
    previous_end = -1
    previous_name = ""
    for start, size, range_name in sorted(ranges):
        if start < previous_end:
            raise LayoutError(f"{name} overlaps: {previous_name} and {range_name}")
        previous_end = start + size
        previous_name = range_name


def _validate_memory(root: dict[str, Any]) -> dict[str, dict[str, int]]:
    memory = _mapping(root.get("memory"), "memory")
    address_space = _mapping(memory.get("address_space"), "memory.address_space")
    if _integer(address_space.get("start"), "memory.address_space.start") != 0:
        raise LayoutError("memory address space must start at zero")
    address_size = _integer(address_space.get("size_bytes"), "memory.address_space.size_bytes")
    if address_size != 0x100000:
        raise LayoutError("memory address space must cover exactly 1 MiB")

    excluded = memory.get("excluded_ranges")
    if not isinstance(excluded, list):
        raise LayoutError("memory.excluded_ranges must be a list")
    excluded_ranges: list[tuple[int, int, str]] = []
    for index, item in enumerate(excluded):
        item_object = _mapping(item, f"memory.excluded_ranges[{index}]")
        name = item_object.get("name")
        if not isinstance(name, str):
            raise LayoutError(f"memory.excluded_ranges[{index}].name must be a string")
        start, size = _validate_range(item_object.get("start"), item_object.get("size_bytes"),
                                      f"memory.excluded_ranges[{index}]", address_size)
        excluded_ranges.append((start, size, name))
    _validate_non_overlapping(excluded_ranges, "memory.excluded_ranges")
    if {name: (start, size) for start, size, name in excluded_ranges} != EXPECTED_EXCLUDED_RANGES:
        raise LayoutError("memory.excluded_ranges do not match the PC-98 reserved map")

    owned = memory.get("owned_regions")
    if not isinstance(owned, list):
        raise LayoutError("memory.owned_regions must be a list")
    owned_ranges: list[tuple[int, int, str]] = []
    owned_by_name: dict[str, dict[str, int]] = {}
    for index, item in enumerate(owned):
        item_object = _mapping(item, f"memory.owned_regions[{index}]")
        name = item_object.get("name")
        if not isinstance(name, str) or name in owned_by_name:
            raise LayoutError(f"memory.owned_regions[{index}].name must be unique")
        kind = item_object.get("kind")
        start, size = _validate_range(item_object.get("start"), item_object.get("size_bytes"),
                                      f"memory.owned_regions[{index}]", address_size)
        alignment = _integer(item_object.get("alignment"), f"memory.owned_regions[{index}].alignment")
        if alignment <= 0 or alignment & (alignment - 1) or start % alignment:
            raise LayoutError(f"memory.owned_regions[{index}] has invalid alignment")
        owned_ranges.append((start, size, name))
        owned_by_name[name] = {"kind": kind, "start": start, "size_bytes": size, "alignment": alignment}
    _validate_non_overlapping(owned_ranges, "memory.owned_regions")
    if set(owned_by_name) != set(EXPECTED_REGIONS):
        raise LayoutError("memory.owned_regions must declare IPL, payload, stack, and result")
    for name, (kind, start, size, alignment) in EXPECTED_REGIONS.items():
        if owned_by_name[name] != {"kind": kind, "start": start, "size_bytes": size, "alignment": alignment}:
            raise LayoutError(f"memory.owned_regions.{name} does not match the reviewed layout")
    for start, size, name in owned_ranges:
        for excluded_start, excluded_size, excluded_name in excluded_ranges:
            if start < excluded_start + excluded_size and excluded_start < start + size:
                raise LayoutError(f"owned region {name} overlaps excluded range {excluded_name}")
    return owned_by_name


def _validate_result(root: dict[str, Any], owned_regions: dict[str, dict[str, int]]) -> None:
    result = _mapping(root.get("result"), "result")
    if result.get("protocol") != "result-v1":
        raise LayoutError("result.protocol must be result-v1")
    physical_address = _integer(result.get("physical_address"), "result.physical_address")
    size_bytes = _integer(result.get("size_bytes"), "result.size_bytes")
    alignment = _integer(result.get("alignment"), "result.alignment")
    if size_bytes != EXPECTED_RESULT_SIZE or alignment != 16 or physical_address != 0x29000:
        raise LayoutError("result physical placement must be 0x29000, 128 bytes, aligned to 16")
    if result.get("reserved_region") != "result" or owned_regions["result"]["start"] != physical_address:
        raise LayoutError("result must be backed by the owned result region")

    wire = _mapping(result.get("wire"), "result.wire")
    if wire.get("byte_order") != "little":
        raise LayoutError("result.wire.byte_order must be little")
    fields = wire.get("fields")
    if not isinstance(fields, list) or len(fields) != len(EXPECTED_RESULT_FIELDS):
        raise LayoutError("result.wire.fields must contain the complete v1 field list")
    field_ranges: list[tuple[int, int, str]] = []
    field_by_name: dict[str, dict[str, Any]] = {}
    for index, expected in enumerate(EXPECTED_RESULT_FIELDS):
        field = _mapping(fields[index], f"result.wire.fields[{index}]")
        name, offset, width, field_type, coverage = expected
        if (field.get("name"), field.get("offset"), field.get("width"), field.get("type"), field.get("coverage")) != expected:
            raise LayoutError(f"result.wire.fields[{index}] does not match the v1 field layout")
        if name in field_by_name:
            raise LayoutError(f"duplicate result field {name}")
        if offset + width > EXPECTED_RESULT_SIZE:
            raise LayoutError(f"result field {name} is outside the block")
        field_ranges.append((offset, width, name))
        field_by_name[name] = field
    _validate_non_overlapping(field_ranges, "result.wire.fields")
    if field_by_name["magic"].get("value_hex") != "4e503254":
        raise LayoutError("result magic must be the ASCII bytes NP2T")
    for name, value in (("version", 1), ("header_size", 32), ("block_size", 128), ("flags", 0)):
        if field_by_name[name].get("value") != value:
            raise LayoutError(f"result field {name} has an invalid fixed value")
    if field_by_name["first_failed_id"].get("none_value") != 65535:
        raise LayoutError("first_failed_id none value must be 0xffff")
    if field_by_name["diagnostic_length"].get("max_value") != 64 or field_by_name["diagnostic_data"].get("max_bytes") != 64:
        raise LayoutError("diagnostic data must be bounded to 64 bytes")

    checksum = _mapping(wire.get("checksum"), "result.wire.checksum")
    if checksum.get("field") != "checksum" or checksum.get("algorithm") != "CRC-32/ISO-HDLC":
        raise LayoutError("result checksum must be CRC-32/ISO-HDLC over the body")
    if checksum.get("polynomial") != "0x04c11db7" or checksum.get("initial") != "0xffffffff" or checksum.get("final_xor") != "0xffffffff":
        raise LayoutError("result checksum parameters are invalid")
    if checksum.get("byte_order") != "little" or checksum.get("coverage_ranges") != [{"start": 0, "end_exclusive": 120}]:
        raise LayoutError("result checksum coverage must be exactly bytes [0, 120)")
    checksum_field = field_by_name["checksum"]
    for coverage_range in checksum["coverage_ranges"]:
        start = _integer(coverage_range["start"], "result checksum coverage start")
        end = _integer(coverage_range["end_exclusive"], "result checksum coverage end")
        if start < 0 or end <= start or end > EXPECTED_RESULT_SIZE:
            raise LayoutError("result checksum coverage is outside the block")
        if start < checksum_field["offset"] + checksum_field["width"] and checksum_field["offset"] < end:
            raise LayoutError("result checksum coverage includes its own field")

    state = _mapping(wire.get("state"), "result.wire.state")
    if state.get("field") != "state" or state.get("values") != {"UNINITIALIZED": 0, "RUNNING": 1, "PASS": 2, "FAIL": 3}:
        raise LayoutError("result state encoding is invalid")
    if state.get("guest_fail_meaning") != "one or more guest tests completed with a failing result" or state.get("host_integrity_failures") != "INVALID":
        raise LayoutError("result guest FAIL and host INVALID semantics are ambiguous")
    if field_by_name["state"].get("write_order") != "last":
        raise LayoutError("result state must be written last")
    future = _mapping(wire.get("future_extensions"), "result.wire.future_extensions")
    if future != {
        "version_1_requires_exact_block_size": True,
        "unsupported_versions_are_invalid": True,
        "reserved_bytes_must_be_zero": True,
        "append_only_for_future_versions": True,
    }:
        raise LayoutError("result future-extension rules are invalid")


def validate_layout(layout: Any) -> dict[str, Any]:
    root = _mapping(layout, "layout")
    if root.get("schema_version") != 1 or root.get("name") != "np2test":
        raise LayoutError("schema_version must be 1 and name must be np2test")

    image = _mapping(root.get("image"), "image")
    if image.get("format") != "raw" or image.get("extension") is not None:
        raise LayoutError("image must be raw with no selected extension yet")
    if image.get("extension_selection") != PENDING_EXTENSION_STATUS:
        raise LayoutError("image extension status must name the remaining NP2kai validation")
    candidates = image.get("extension_candidates")
    if not isinstance(candidates, list) or not candidates or len(set(candidates)) != len(candidates):
        raise LayoutError("image.extension_candidates must be a non-empty unique list")
    for extension in candidates:
        if not isinstance(extension, str) or not re.fullmatch(r"\.[A-Za-z0-9][A-Za-z0-9._-]*", extension):
            raise LayoutError(f"invalid candidate extension: {extension!r}")
    geometry = _mapping(image.get("geometry"), "image.geometry")
    if geometry != EXPECTED_GEOMETRY:
        raise LayoutError(f"image.geometry must be exactly {EXPECTED_GEOMETRY}")
    computed_size = 1
    for key in ("cylinders", "heads", "sectors_per_track", "bytes_per_sector"):
        computed_size *= geometry[key]
    if _integer(image.get("size_bytes"), "image.size_bytes") != computed_size or computed_size != EXPECTED_SIZE:
        raise LayoutError(f"image.size_bytes must be {EXPECTED_SIZE}")
    if image.get("d88_required") is not False:
        raise LayoutError("image.d88_required must be false")

    ipl = _mapping(root.get("ipl"), "ipl")
    if ipl.get("implemented") is not False or ipl.get("reserved_region") != "ipl":
        raise LayoutError("IPL must remain unimplemented and use the declared IPL region")
    if _integer(ipl.get("sector_offset"), "ipl.sector_offset") != 0 or _integer(ipl.get("sector_bytes"), "ipl.sector_bytes") != 1024:
        raise LayoutError("ipl must start at sector offset zero with 1024-byte sectors")
    if _integer(ipl.get("load_physical"), "ipl.load_physical") != 0x1FC00:
        raise LayoutError("ipl.load_physical must be 0x1fc00")
    entry = _mapping(ipl.get("entry"), "ipl.entry")
    if _integer(entry.get("cs"), "ipl.entry.cs") != 0x1FC0 or _integer(entry.get("ip"), "ipl.entry.ip") != 0:
        raise LayoutError("ipl entry must be 1fc0:0000")
    signatures = ipl.get("signatures")
    if signatures != [{"offset": 510, "bytes": "55aa"}, {"offset": 1022, "bytes": "55aa"}]:
        raise LayoutError("ipl.signatures must contain the two 55aa markers")
    for index, signature in enumerate(signatures):
        signature_object = _mapping(signature, f"ipl.signatures[{index}]")
        if _integer(signature_object.get("offset"), f"ipl.signatures[{index}].offset") < 0:
            raise LayoutError("signature offset must not be negative")
        if _hex_bytes(signature_object.get("bytes"), f"ipl.signatures[{index}].bytes") != SIGNATURE:
            raise LayoutError("signature bytes must be 55aa")

    payload = _mapping(root.get("payload"), "payload")
    if payload.get("implemented") is not False or payload.get("reserved_region") != "payload-reserve" or payload.get("placements") != []:
        raise LayoutError("payload must remain unimplemented with no placements")
    owned_regions = _validate_memory(root)
    _validate_result(root, owned_regions)

    toolchain = _mapping(root.get("toolchain"), "toolchain")
    assembler = _mapping(toolchain.get("assembler"), "toolchain.assembler")
    if assembler.get("command") != "nasm" or assembler.get("version") != EXPECTED_NASM or assembler.get("ubuntu_24_04_package") != "nasm=2.16.01-1build1":
        raise LayoutError("toolchain must pin Ubuntu 24.04 NASM 2.16.01-1build1")
    if assembler.get("artifact_sha256") is not None or assembler.get("artifact_pin") != "canonical-ci-environment":
        raise LayoutError("toolchain must distinguish the unpinned package checksum from the CI image pin")
    if not isinstance(assembler.get("artifact_pin_note"), str) or "not SHA-pinned" not in assembler["artifact_pin_note"]:
        raise LayoutError("toolchain artifact checksum policy must be documented")
    python_tool = _mapping(toolchain.get("python"), "toolchain.python")
    if (python_tool.get("command"), python_tool.get("implementation"), python_tool.get("supported_major"), python_tool.get("supported_minor"), python_tool.get("stdlib_only")) != ("python3", "CPython", 3, 12, True):
        raise LayoutError("toolchain must pin CPython 3.12 standard-library execution")
    if python_tool.get("ubuntu_24_04_source") != "Ubuntu 24.04 CPython 3.12 package":
        raise LayoutError("toolchain Python provenance is incomplete")
    if toolchain.get("output") != "bin":
        raise LayoutError("toolchain output must be NASM bin")
    artifact = _mapping(root.get("artifact"), "artifact")
    if artifact.get("extension_status") != PENDING_EXTENSION_STATUS or artifact.get("golden_tracked") is not False:
        raise LayoutError("artifact extension status or golden policy is invalid")
    return root


def load_layout(path: Path) -> dict[str, Any]:
    try:
        layout = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LayoutError(f"cannot read layout {path}: {exc}") from exc
    return validate_layout(layout)


def check_nasm(expected: str = EXPECTED_NASM) -> str:
    executable = shutil.which("nasm")
    if executable is None:
        raise LayoutError("nasm is required for image builds but was not found on PATH")
    try:
        result = subprocess.run([executable, "-v"], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise LayoutError(f"unable to query nasm: {exc}") from exc
    match = re.search(r"NASM version ([0-9]+(?:\.[0-9]+){1,2})", result.stdout + result.stderr)
    version = match.group(1) if match else "unknown"
    if version != expected:
        raise LayoutError(f"NASM {expected} is required (found {version})")
    return version


def check_python(expected: tuple[int, int] = EXPECTED_PYTHON) -> dict[str, Any]:
    actual = (sys.version_info.major, sys.version_info.minor)
    if sys.implementation.name != "cpython" or actual != expected:
        raise LayoutError(f"CPython {expected[0]}.{expected[1]} is required for image builds (found {sys.implementation.name} {actual[0]}.{actual[1]})")
    return {
        "implementation": sys.implementation.name,
        "version_major": sys.version_info.major,
        "version_minor": sys.version_info.minor,
        "version_patch": sys.version_info.micro,
    }


def build(layout_path: Path, output_path: Path, sha256_path: Path | None = None,
          manifest_path: Path | None = None) -> str:
    layout = load_layout(layout_path)
    nasm_version = check_nasm(layout["toolchain"]["assembler"]["version"])
    python_version = check_python((layout["toolchain"]["python"]["supported_major"], layout["toolchain"]["python"]["supported_minor"]))
    image = bytearray(layout["image"]["size_bytes"])
    for signature in layout["ipl"]["signatures"]:
        offset = signature["offset"]
        image[offset:offset + len(SIGNATURE)] = SIGNATURE

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    sha256_path = sha256_path or output_path.with_name(output_path.name + ".sha256")
    sha256_path.parent.mkdir(parents=True, exist_ok=True)
    sha256_path.write_text(f"{digest}  {output_path.name}\n", encoding="ascii")
    manifest_path = manifest_path or output_path.with_name(output_path.name + ".manifest.json")
    manifest = {
        "schema_version": 1,
        "artifact": output_path.name,
        "size_bytes": len(image),
        "sha256": digest,
        "layout_file": layout_path.name,
        "layout_sha256": hashlib.sha256(layout_path.read_bytes()).hexdigest(),
        "toolchain": {
            "assembler": {"command": "nasm", "version": nasm_version},
            "python": python_version,
        },
        "stage": "3.5a-1-empty-image",
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    return digest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sha256-output", type=Path)
    parser.add_argument("--manifest-output", type=Path)
    args = parser.parse_args(argv)
    try:
        digest = build(args.layout, args.output, args.sha256_output, args.manifest_output)
    except LayoutError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"built {args.output} ({EXPECTED_SIZE} bytes, sha256={digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
