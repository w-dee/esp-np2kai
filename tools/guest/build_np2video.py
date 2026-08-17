#!/usr/bin/env python3
"""Build the deterministic Step 7A.3a raw HDM video fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
from typing import Any


IMAGE_SIZE = 1_261_568
IPL_SIZE = 1024
IPL_LOAD_PHYSICAL = 0x1FC00
CONTROL_PHYSICAL = 0x2A000
CONTROL_SIZE = 32


class FixtureError(Exception):
    pass


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

    image = _mapping(root.get("image"), "image")
    geometry = _mapping(image.get("geometry"), "image.geometry")
    if root.get("schema_version") != 1 or root.get("name") != "np2video":
        raise FixtureError("layout identity is not np2video schema v1")
    if image.get("format") != "raw" or image.get("extension") != ".hdm":
        raise FixtureError("image must be a raw .hdm fixture")
    if _integer(image.get("size_bytes"), "image.size_bytes") != IMAGE_SIZE:
        raise FixtureError("image size must be 1261568 bytes")
    expected_geometry = {
        "cylinders": 77,
        "heads": 2,
        "sectors_per_track": 8,
        "bytes_per_sector": 1024,
    }
    if geometry != expected_geometry:
        raise FixtureError("image geometry does not match the NP2kai 2HD HDM profile")

    ipl = _mapping(root.get("ipl"), "ipl")
    if (ipl.get("implemented") is not True or ipl.get("source") != "src/ipl.asm" or
            _integer(ipl.get("binary_size"), "ipl.binary_size") != IPL_SIZE or
            _integer(ipl.get("load_physical"), "ipl.load_physical") != IPL_LOAD_PHYSICAL):
        raise FixtureError("IPL layout is not the reviewed 1024-byte boot placement")
    if ipl.get("signatures") != [
        {"offset": 510, "bytes": "55aa"},
        {"offset": 1022, "bytes": "55aa"},
    ]:
        raise FixtureError("IPL signatures do not match the reviewed boot contract")

    control = _mapping(root.get("control_block"), "control_block")
    if (control.get("protocol") != "NP2V" or
            _integer(control.get("version"), "control_block.version") != 1 or
            _integer(control.get("physical_address"), "control_block.physical_address") != CONTROL_PHYSICAL or
            _integer(control.get("size_bytes"), "control_block.size_bytes") != CONTROL_SIZE or
            _integer(control.get("state_offset"), "control_block.state_offset") != 31 or
            control.get("state_written_last") is not True):
        raise FixtureError("NP2V control block does not match v1")
    scene = _mapping(root.get("scene"), "scene")
    if scene.get("rows_text") is not None and len(scene["rows_text"]) != 25:
        raise FixtureError("scene.rows_text must contain 25 rows")
    return root


def _assemble(nasm: str, source: pathlib.Path, output: pathlib.Path) -> None:
    try:
        subprocess.run(
            [nasm, "-f", "bin", "-o", str(output), str(source)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise FixtureError(f"cannot execute assembler {nasm}: {error}") from error
    except subprocess.CalledProcessError as error:
        diagnostic = error.stderr.decode("utf-8", errors="replace").strip()
        raise FixtureError(f"IPL assembly failed: {diagnostic}") from error


def build(layout_path: pathlib.Path, output_path: pathlib.Path, nasm: str) -> str:
    layout = _load_layout(layout_path)
    source = layout_path.parent / layout["ipl"]["source"]
    if not source.is_file():
        raise FixtureError(f"IPL source does not exist: {source}")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="np2video-build-") as temp_name:
        assembled_path = pathlib.Path(temp_name) / "ipl.bin"
        _assemble(nasm, source, assembled_path)
        ipl = assembled_path.read_bytes()
    if len(ipl) != IPL_SIZE:
        raise FixtureError(f"assembled IPL is {len(ipl)} bytes, expected {IPL_SIZE}")
    if ipl[510:512] != b"\x55\xaa" or ipl[1022:1024] != b"\x55\xaa":
        raise FixtureError("assembled IPL signatures are incorrect")

    image = bytearray(IMAGE_SIZE)
    # The disk stores the IPL in sector zero; NP2kai BIOS loads it at
    # IPL_LOAD_PHYSICAL before transferring control to 1fc0:0000.
    image[:IPL_SIZE] = ipl
    output_path.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    output_path.with_name(output_path.name + ".sha256").write_text(
        f"{digest}  {output_path.name}\n", encoding="ascii"
    )
    manifest = {
        "fixture_id": layout["fixture"]["id"],
        "scene_id": layout["fixture"]["scene_id"],
        "image_size": IMAGE_SIZE,
        "sha256": digest,
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
    print(f"np2video image={args.output} size={IMAGE_SIZE} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
