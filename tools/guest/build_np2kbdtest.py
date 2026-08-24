#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build the standalone deterministic NP2 keyboard fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any


EXPECTED_GEOMETRY = {
    "cylinders": 77,
    "heads": 2,
    "sectors_per_track": 8,
    "bytes_per_sector": 1024,
}
EXPECTED_IMAGE_SIZE = 1_261_568
EXPECTED_IPL_SIZE = 1024
EXPECTED_IPL_LOAD = 0x1FC00
EXPECTED_CONTROL = (0x27FC0, 64)
EXPECTED_STACK = (0x28000, 0x1000)
EXPECTED_RESULT = (0x29000, 128)
EXPECTED_EXCLUDED = {
    "pc98-low-bios-data": (0x00000, 0x01000),
    "pc98-text-vram": (0xA0000, 0x08000),
    "pc98-graphics-vram-b-r-g": (0xA8000, 0x18000),
    "pc98-graphics-vram-e": (0xE0000, 0x08000),
    "pc98-bios-firmware": (0xE8000, 0x18000),
}
EXPECTED_NASM = "2.16.01"


class LayoutError(ValueError):
    pass


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise LayoutError(f"{name} must be an integer")
    return value


def _region(item: Any, name: str) -> tuple[str, int, int, int]:
    if not isinstance(item, dict):
        raise LayoutError(f"{name} must be an object")
    region_name = item.get("name")
    kind = item.get("kind")
    start = _integer(item.get("start"), f"{name}.start")
    size = _integer(item.get("size_bytes"), f"{name}.size_bytes")
    alignment = _integer(item.get("alignment"), f"{name}.alignment")
    if not isinstance(region_name, str) or not isinstance(kind, str):
        raise LayoutError(f"{name} name/kind must be strings")
    if start < 0 or size <= 0 or alignment <= 0 or start % alignment:
        raise LayoutError(f"{name} has invalid range/alignment")
    return region_name, start, size, alignment


def _non_overlapping(regions: list[tuple[str, int, int, int]]) -> None:
    ordered = sorted(regions, key=lambda item: item[1])
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] + previous[2] > current[1]:
            raise LayoutError(f"memory regions overlap: {previous[0]} and {current[0]}")


def load_layout(path: Path) -> dict[str, Any]:
    try:
        layout = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LayoutError(f"cannot read layout: {exc}") from exc
    image = layout.get("image", {})
    if image.get("geometry") != EXPECTED_GEOMETRY or image.get("size_bytes") != EXPECTED_IMAGE_SIZE:
        raise LayoutError("image geometry/size is not the validated FD1232 contract")
    ipl = layout.get("ipl", {})
    if ipl.get("binary_size") != EXPECTED_IPL_SIZE or ipl.get("load_physical") != EXPECTED_IPL_LOAD:
        raise LayoutError("IPL placement is invalid")
    if ipl.get("sector_offset") != 0 or ipl.get("sector_bytes") != EXPECTED_IPL_SIZE:
        raise LayoutError("IPL must occupy raw image sector zero")
    signatures = ipl.get("signatures")
    if signatures != [{"offset": 510, "bytes": "55aa"}, {"offset": 1022, "bytes": "55aa"}]:
        raise LayoutError("IPL signatures are invalid")

    regions = [_region(item, f"memory.owned_regions[{index}]")
               for index, item in enumerate(layout.get("memory", {}).get("owned_regions", []))]
    expected_regions = {
        "ipl": ("ipl", 0x1FC00, 0x400, 0x400),
        "control": ("control", 0x27FC0, 64, 16),
        "stack": ("stack", 0x28000, 0x1000, 16),
        "result": ("result", 0x29000, 128, 16),
    }
    _non_overlapping(regions)
    if {name: (name, start, size, alignment) for name, start, size, alignment in regions} != expected_regions:
        raise LayoutError("owned memory regions do not match keyboard fixture contract")
    excluded = layout.get("memory", {}).get("excluded_ranges", [])
    actual_excluded = {}
    for index, item in enumerate(excluded):
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise LayoutError(f"excluded[{index}] must contain a string name")
        start = _integer(item.get("start"), f"excluded[{index}].start")
        size = _integer(item.get("size_bytes"), f"excluded[{index}].size_bytes")
        if start < 0 or size <= 0:
            raise LayoutError(f"excluded[{index}] has invalid range")
        actual_excluded[item["name"]] = (start, size)
    if actual_excluded != EXPECTED_EXCLUDED:
        raise LayoutError("PC-98 excluded ranges do not match the reviewed map")
    for name, start, size, _ in regions:
        for excluded_name, (excluded_start, excluded_size) in EXPECTED_EXCLUDED.items():
            if start < excluded_start + excluded_size and excluded_start < start + size:
                raise LayoutError(f"owned region {name} overlaps {excluded_name}")

    control = layout.get("control", {})
    if (control.get("physical_address"), control.get("size_bytes")) != EXPECTED_CONTROL:
        raise LayoutError("control placement is invalid")
    if control.get("physical_address") + control.get("size_bytes") != EXPECTED_STACK[0]:
        raise LayoutError("control must end exactly before stack")
    result = layout.get("result", {})
    if (result.get("physical_address"), result.get("size_bytes")) != EXPECTED_RESULT:
        raise LayoutError("result placement is invalid")
    if control.get("wire", {}).get("crc_coverage_end") != 56 or control.get("wire", {}).get("state_offset") != 60:
        raise LayoutError("control CRC/state offsets are invalid")
    if control.get("expected_make") != 0x1D or control.get("expected_break") != 0x9D:
        raise LayoutError("control expected bytes are invalid")
    if result.get("total_count") != 1 or result.get("test_id") != 0x0C01:
        raise LayoutError("keyboard result contract is invalid")
    return layout


def _nasm_version(nasm: str) -> str:
    output = subprocess.run([nasm, "-v"], check=True, capture_output=True, text=True).stdout.strip()
    version = output.rsplit(" ", 1)[-1]
    if version != EXPECTED_NASM:
        raise LayoutError(f"nasm {version} is not the pinned {EXPECTED_NASM}")
    return version


def _assemble(root: Path, source: Path, nasm: str) -> tuple[bytes, str]:
    version = _nasm_version(nasm)
    with tempfile.TemporaryDirectory(prefix="np2kbdtest-") as temporary:
        output = Path(temporary) / "ipl.bin"
        subprocess.run([nasm, "-f", "bin", "-I", f"{source.parent}/", "-o", str(output), str(source)],
                       check=True, cwd=root)
        binary = output.read_bytes()
    if len(binary) != EXPECTED_IPL_SIZE:
        raise LayoutError(f"assembled IPL is {len(binary)} bytes, expected {EXPECTED_IPL_SIZE}")
    if binary[510:512] != b"\x55\xaa" or binary[1022:1024] != b"\x55\xaa":
        raise LayoutError("assembled IPL signatures are invalid")
    if any(binary[512:1022]):
        raise LayoutError("second IPL half must remain zero for the standalone fixture")
    code_end = max((index + 1 for index, byte in enumerate(binary[:510]) if byte), default=0)
    if code_end >= 510:
        raise LayoutError("keyboard IPL executable/data exceeds first signature budget")
    return binary, version


def build(layout_path: Path, output_path: Path, manifest_path: Path | None = None,
          nasm: str = "nasm") -> str:
    layout = load_layout(layout_path)
    root = layout_path.parents[3]
    source = layout_path.parent / layout["ipl"]["source"]
    binary, nasm_version = _assemble(root, source, nasm)
    image = bytearray(layout["image"]["size_bytes"])
    image[:EXPECTED_IPL_SIZE] = binary
    digest = hashlib.sha256(image).hexdigest()
    expected = layout["artifact"].get("expected_sha256")
    if expected is not None and digest != expected:
        raise LayoutError(f"sha256 is {digest}, expected {expected}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)
    output_path.with_suffix(output_path.suffix + ".sha256").write_text(
        f"{digest}  {output_path.name}\n", encoding="ascii")
    code_end = max((index + 1 for index, byte in enumerate(binary[:510]) if byte), default=0)
    if manifest_path is not None:
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(json.dumps({
            "schema_version": 1,
            "fixture": "np2kbdtest",
            "image": output_path.name,
            "image_size": len(image),
            "sha256": digest,
            "ipl_size": len(binary),
            "ipl_code_end": code_end,
            "nasm": nasm_version,
            "payload_loaded": False,
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    try:
        print(build(args.layout, args.output, args.manifest, args.nasm))
    except (LayoutError, OSError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
