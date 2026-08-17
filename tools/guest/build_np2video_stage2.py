#!/usr/bin/env python3
"""Build the Step 7A.3c raw image with a project-owned stage2 payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import tempfile
from typing import Any, Iterable


IMAGE_SIZE = 1_261_568
IPL_SIZE = 1024
SECTOR_SIZE = 1024
MAX_STAGE2_SIZE = 32 * 1024
MAX_STAGE2_SECTORS = 32
IPL_LOAD_PHYSICAL = 0x1FC00
STAGE2_LOAD_PHYSICAL = 0x20000
STAGE2_LOAD_SEGMENT = 0x2000
STAGE2_ENTRY_OFFSET = 8
STAGE2_HEADER_SIZE = 8
CONTROL_PHYSICAL = 0x2A000
CONTROL_SIZE = 32
STACK_PHYSICAL = 0x28000
PROJECT_NAME_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
FIXTURE_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,63}$")


class FixtureError(Exception):
    """A deterministic fixture contract was violated."""


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise FixtureError(f"{name} must be an object")
    return value


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise FixtureError(f"{name} must be an integer")
    return value


def _load_layout(path: pathlib.Path) -> dict[str, Any]:
    try:
        root = _mapping(json.loads(path.read_text(encoding="utf-8")), "layout")
    except (OSError, json.JSONDecodeError) as error:
        raise FixtureError(f"cannot read layout: {error}") from error

    if root.get("schema_version") != 1:
        raise FixtureError("layout schema_version must be 1")
    project_name = root.get("name")
    if (not isinstance(project_name, str) or
            PROJECT_NAME_PATTERN.fullmatch(project_name) is None):
        raise FixtureError("layout.name must be a non-empty ASCII project name")
    image = _mapping(root.get("image"), "image")
    geometry = _mapping(image.get("geometry"), "image.geometry")
    if image.get("format") != "raw" or image.get("extension") != ".hdm":
        raise FixtureError("image must be a raw .hdm fixture")
    if _integer(image.get("size_bytes"), "image.size_bytes") != IMAGE_SIZE:
        raise FixtureError(f"image size must be {IMAGE_SIZE} bytes")
    if geometry != {
        "cylinders": 77,
        "heads": 2,
        "sectors_per_track": 8,
        "bytes_per_sector": SECTOR_SIZE,
    }:
        raise FixtureError("image geometry does not match the NP2kai 2HD HDM profile")

    fixture = _mapping(root.get("fixture"), "fixture")
    fixture_id = fixture.get("id")
    if not isinstance(fixture_id, str) or FIXTURE_ID_PATTERN.fullmatch(fixture_id) is None:
        raise FixtureError(
            "fixture.id must contain 1..63 ASCII letters, digits, '.', '_' or '-'")
    scene_id = _integer(fixture.get("scene_id"), "fixture.scene_id")
    if not 1 <= scene_id <= 65535:
        raise FixtureError("fixture.scene_id must be a positive uint16")

    ipl = _mapping(root.get("ipl"), "ipl")
    if (
        ipl.get("implemented") is not True
        or ipl.get("source") != "src/ipl.asm"
        or _integer(ipl.get("binary_size"), "ipl.binary_size") != IPL_SIZE
        or _integer(ipl.get("sector_offset"), "ipl.sector_offset") != 0
        or _integer(ipl.get("sector_bytes"), "ipl.sector_bytes") != SECTOR_SIZE
        or _integer(ipl.get("load_physical"), "ipl.load_physical") != IPL_LOAD_PHYSICAL
    ):
        raise FixtureError("IPL layout is not the reviewed 1024-byte boot placement")
    if ipl.get("signatures") != [
        {"offset": 510, "bytes": "55aa"},
        {"offset": 1022, "bytes": "55aa"},
    ]:
        raise FixtureError("IPL signatures do not match the reviewed boot contract")

    payload = _mapping(root.get("payload"), "payload")
    expected_payload = {
        "implemented": True,
        "source": "src/stage2.asm",
        "format": "flat-16-bit",
        "disk_offset": IPL_SIZE,
        "sector_offset": 1,
        "max_size_bytes": MAX_STAGE2_SIZE,
        "max_sector_count": MAX_STAGE2_SECTORS,
        "load_physical": STAGE2_LOAD_PHYSICAL,
        "load_segment": STAGE2_LOAD_SEGMENT,
        "entry_offset": STAGE2_ENTRY_OFFSET,
        "header_size": STAGE2_HEADER_SIZE,
        "padding": "zero",
    }
    for key, expected in expected_payload.items():
        if payload.get(key) != expected:
            raise FixtureError(f"payload.{key} must be {expected!r}")

    control = _mapping(root.get("control_block"), "control_block")
    if (
        control.get("protocol") != "NP2V"
        or _integer(control.get("version"), "control_block.version") != 1
        or _integer(control.get("physical_address"), "control_block.physical_address") != CONTROL_PHYSICAL
        or _integer(control.get("size_bytes"), "control_block.size_bytes") != CONTROL_SIZE
        or _integer(control.get("state_offset"), "control_block.state_offset") != 31
        or control.get("state_written_last") is not True
    ):
        raise FixtureError("NP2V control block does not match v1")

    memory = _mapping(root.get("memory"), "memory")
    address_space = _mapping(memory.get("address_space"), "memory.address_space")
    address_start = _integer(address_space.get("start"), "memory.address_space.start")
    address_size = _integer(address_space.get("size_bytes"), "memory.address_space.size_bytes")
    address_end = address_start + address_size
    if address_start < 0 or address_size <= 0:
        raise FixtureError("memory address space is invalid")

    owned = memory.get("owned_regions")
    if not isinstance(owned, list):
        raise FixtureError("memory.owned_regions must be an array")
    owned_ranges: list[tuple[str, int, int]] = []
    for index, region_value in enumerate(owned):
        region = _mapping(region_value, f"memory.owned_regions[{index}]")
        name = region.get("name")
        if not isinstance(name, str) or not name:
            raise FixtureError(f"memory.owned_regions[{index}].name is invalid")
        start = _integer(region.get("start"), f"memory.owned_regions[{index}].start")
        size = _integer(region.get("size_bytes"), f"memory.owned_regions[{index}].size_bytes")
        alignment = _integer(region.get("alignment"), f"memory.owned_regions[{index}].alignment")
        if start < address_start or size <= 0 or start + size > address_end:
            raise FixtureError(f"owned region {name} is outside RAM")
        if alignment <= 0 or start % alignment:
            raise FixtureError(f"owned region {name} has invalid alignment")
        owned_ranges.append((name, start, start + size))
    for index, (_, start, end) in enumerate(owned_ranges):
        for other_name, other_start, other_end in owned_ranges[index + 1 :]:
            if start < other_end and other_start < end:
                raise FixtureError(f"owned regions overlap: {owned_ranges[index][0]} and {other_name}")

    excluded = memory.get("excluded_ranges")
    if not isinstance(excluded, list):
        raise FixtureError("memory.excluded_ranges must be an array")
    excluded_ranges: list[tuple[str, int, int]] = []
    for index, region_value in enumerate(excluded):
        region = _mapping(region_value, f"memory.excluded_ranges[{index}]")
        name = region.get("name", f"excluded[{index}]")
        start = _integer(region.get("start"), f"memory.excluded_ranges[{index}].start")
        size = _integer(region.get("size_bytes"), f"memory.excluded_ranges[{index}].size_bytes")
        if size <= 0 or start < address_start or start + size > address_end:
            raise FixtureError(f"excluded range {name} is outside RAM")
        excluded_ranges.append((str(name), start, start + size))
    for name, start, end in owned_ranges:
        for excluded_name, excluded_start, excluded_end in excluded_ranges:
            if start < excluded_end and excluded_start < end:
                raise FixtureError(f"owned region {name} overlaps excluded range {excluded_name}")

    required_regions = {
        "ipl": (IPL_LOAD_PHYSICAL, IPL_SIZE),
        "payload-reserve": (STAGE2_LOAD_PHYSICAL, MAX_STAGE2_SIZE),
        "stack": (STACK_PHYSICAL, 4096),
        "video-control": (CONTROL_PHYSICAL, CONTROL_SIZE),
    }
    for name, (start, size) in required_regions.items():
        matching = [entry for entry in owned_ranges if entry[0] == name]
        if len(matching) != 1 or matching[0][1:] != (start, start + size):
            raise FixtureError(f"owned region {name} does not match the loader contract")

    return root


def _assemble(
    nasm: str,
    source: pathlib.Path,
    output: pathlib.Path,
    defines: Iterable[str] = (),
) -> None:
    command = [nasm, "-f", "bin"]
    command.extend(f"-d{define}" for define in defines)
    command.extend(["-o", str(output), str(source)])
    try:
        subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise FixtureError(f"cannot execute assembler {nasm}: {error}") from error
    except subprocess.CalledProcessError as error:
        diagnostic = error.stderr.decode("utf-8", errors="replace").strip()
        raise FixtureError(f"assembly failed for {source.name}: {diagnostic}") from error


def _check_memory_contract(layout: dict[str, Any], stage2_size: int) -> None:
    memory = _mapping(layout["memory"], "memory")
    for value in memory["excluded_ranges"]:
        excluded = _mapping(value, "excluded range")
        start = _integer(excluded["start"], "excluded.start")
        end = start + _integer(excluded["size_bytes"], "excluded.size_bytes")
        if STAGE2_LOAD_PHYSICAL < end and start < STAGE2_LOAD_PHYSICAL + stage2_size:
            raise FixtureError(f"stage2 overlaps excluded range {excluded.get('name', 'unnamed')}")


def build(layout_path: pathlib.Path, output_path: pathlib.Path, nasm: str) -> str:
    layout = _load_layout(layout_path)
    fixture_dir = layout_path.parent
    ipl_source = fixture_dir / layout["ipl"]["source"]
    stage2_source = fixture_dir / layout["payload"]["source"]
    if not ipl_source.is_file():
        raise FixtureError(f"IPL source does not exist: {ipl_source}")
    if not stage2_source.is_file():
        raise FixtureError(f"stage2 source does not exist: {stage2_source}")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="np2video-stage2-build-") as temp_name:
        temp = pathlib.Path(temp_name)
        stage2_binary = temp / "stage2.bin"
        _assemble(nasm, stage2_source, stage2_binary)
        stage2 = stage2_binary.read_bytes()
        if len(stage2) < STAGE2_HEADER_SIZE:
            raise FixtureError(f"stage2 is empty or shorter than its header ({len(stage2)} bytes)")
        if len(stage2) > MAX_STAGE2_SIZE:
            raise FixtureError(f"stage2 is {len(stage2)} bytes; maximum is {MAX_STAGE2_SIZE}")
        stage2_sectors = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
        if not 1 <= stage2_sectors <= MAX_STAGE2_SECTORS:
            raise FixtureError(f"stage2 sector count {stage2_sectors} is outside 1..{MAX_STAGE2_SECTORS}")
        if stage2[:4] != b"ST2V":
            raise FixtureError("stage2 header magic is not ST2V")
        if int.from_bytes(stage2[4:6], "little") != 1:
            raise FixtureError("stage2 header version is not 1")
        if int.from_bytes(stage2[6:8], "little") != len(stage2):
            raise FixtureError("stage2 header size does not equal the assembled binary size")
        if len(stage2) > stage2_sectors * SECTOR_SIZE:
            raise FixtureError("stage2 sector calculation overflow")

        ipl_binary = temp / "ipl.bin"
        _assemble(
            nasm,
            ipl_source,
            ipl_binary,
            [f"STAGE2_SIZE={len(stage2)}", f"STAGE2_SECTORS={stage2_sectors}"],
        )
        ipl = ipl_binary.read_bytes()

    if len(ipl) != IPL_SIZE:
        raise FixtureError(f"assembled IPL is {len(ipl)} bytes, expected {IPL_SIZE}")
    if ipl[510:512] != b"\x55\xaa" or ipl[1022:1024] != b"\x55\xaa":
        raise FixtureError("assembled IPL signatures are incorrect")
    _check_memory_contract(layout, len(stage2))

    image = bytearray(IMAGE_SIZE)
    image[:IPL_SIZE] = ipl
    payload_end = IPL_SIZE + stage2_sectors * SECTOR_SIZE
    image[IPL_SIZE : IPL_SIZE + len(stage2)] = stage2
    if any(image[IPL_SIZE + len(stage2) : payload_end]):
        raise FixtureError("stage2 sector padding is not zero")
    if any(image[payload_end:]):
        raise FixtureError("bytes after the stage2 payload are not zero")
    output_path.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    stage2_digest = hashlib.sha256(stage2).hexdigest()
    output_path.with_name(output_path.name + ".sha256").write_text(
        f"{digest}  {output_path.name}\n", encoding="ascii"
    )
    manifest = {
        "fixture_id": layout["fixture"]["id"],
        "scene_id": layout["fixture"]["scene_id"],
        "image_size": IMAGE_SIZE,
        "sha256": digest,
        "stage2_size": len(stage2),
        "stage2_sector_count": stage2_sectors,
        "stage2_sha256": stage2_digest,
        "stage2_disk_offset": IPL_SIZE,
        "stage2_load_physical": STAGE2_LOAD_PHYSICAL,
        "stage2_entry_offset": STAGE2_ENTRY_OFFSET,
        "control_physical_address": CONTROL_PHYSICAL,
        "control_size_bytes": CONTROL_SIZE,
        "generated": "build artifact",
    }
    output_path.with_name(output_path.name + ".manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    return digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    try:
        digest = build(args.layout, args.output, args.nasm)
    except FixtureError as error:
        parser.error(str(error))
    print(f"np2video stage2 image={args.output} size={IMAGE_SIZE} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
