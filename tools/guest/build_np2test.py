#!/usr/bin/env python3
"""Build the deterministic, filesystem-less NP2TEST foundation image."""

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
EXPECTED_SIZE = 1_261_568
SIGNATURE = bytes.fromhex("55aa")


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


def validate_layout(layout: Any) -> dict[str, Any]:
    root = _mapping(layout, "layout")
    if root.get("schema_version") != 1:
        raise LayoutError("schema_version must be 1")
    if root.get("name") != "np2test":
        raise LayoutError("name must be np2test")

    image = _mapping(root.get("image"), "image")
    if image.get("format") != "raw":
        raise LayoutError("image.format must be raw")
    if image.get("extension") is not None:
        raise LayoutError("image.extension must remain null until extension selection")
    if image.get("extension_selection") != "deferred-to-3.5a-1":
        raise LayoutError("image.extension_selection must remain deferred-to-3.5a-1")
    candidates = image.get("extension_candidates")
    if not isinstance(candidates, list) or not candidates:
        raise LayoutError("image.extension_candidates must be a non-empty list")
    if len(set(candidates)) != len(candidates):
        raise LayoutError("image.extension_candidates must not contain duplicates")
    for extension in candidates:
        if not isinstance(extension, str) or not re.fullmatch(r"\.[A-Za-z0-9][A-Za-z0-9._-]*", extension):
            raise LayoutError(f"invalid candidate extension: {extension!r}")
    geometry = _mapping(image.get("geometry"), "image.geometry")
    if geometry != EXPECTED_GEOMETRY:
        raise LayoutError(f"image.geometry must be exactly {EXPECTED_GEOMETRY}")
    if _integer(image.get("size_bytes"), "image.size_bytes") != EXPECTED_SIZE:
        raise LayoutError(f"image.size_bytes must be {EXPECTED_SIZE}")
    if image.get("d88_required") is not False:
        raise LayoutError("image.d88_required must be false")

    ipl = _mapping(root.get("ipl"), "ipl")
    if ipl.get("implemented") is not False:
        raise LayoutError("ipl.implemented must be false in the foundation milestone")
    if _integer(ipl.get("sector_offset"), "ipl.sector_offset") != 0:
        raise LayoutError("ipl.sector_offset must be zero")
    if _integer(ipl.get("sector_bytes"), "ipl.sector_bytes") != 1024:
        raise LayoutError("ipl.sector_bytes must be 1024")
    if _integer(ipl.get("load_physical"), "ipl.load_physical") != 0x1FC00:
        raise LayoutError("ipl.load_physical must be 0x1fc00")
    entry = _mapping(ipl.get("entry"), "ipl.entry")
    if _integer(entry.get("cs"), "ipl.entry.cs") != 0x1FC0:
        raise LayoutError("ipl.entry.cs must be 0x1fc0")
    if _integer(entry.get("ip"), "ipl.entry.ip") != 0:
        raise LayoutError("ipl.entry.ip must be zero")
    signatures = ipl.get("signatures")
    if signatures != [
        {"offset": 510, "bytes": "55aa"},
        {"offset": 1022, "bytes": "55aa"},
    ]:
        raise LayoutError("ipl.signatures must contain the two 55aa markers")
    for index, signature in enumerate(signatures):
        signature_object = _mapping(signature, f"ipl.signatures[{index}]")
        if _integer(signature_object.get("offset"), f"ipl.signatures[{index}].offset") < 0:
            raise LayoutError("signature offset must not be negative")
        if _hex_bytes(signature_object.get("bytes"), f"ipl.signatures[{index}].bytes") != SIGNATURE:
            raise LayoutError("signature bytes must be 55aa")

    payload = _mapping(root.get("payload"), "payload")
    if payload.get("implemented") is not False or payload.get("placements") != []:
        raise LayoutError("payload must be unimplemented with no placements")

    result = _mapping(root.get("result"), "result")
    if result.get("protocol") != "result-v1":
        raise LayoutError("result.protocol must be result-v1")
    if result.get("physical_address") is not None or result.get("size_bytes") is not None:
        raise LayoutError("result numeric placement must remain deferred")
    if result.get("numeric_layout_status") != "deferred-to-3.5a-2":
        raise LayoutError("result numeric layout must be deferred-to-3.5a-2")

    toolchain = _mapping(root.get("toolchain"), "toolchain")
    if toolchain.get("assembler") != "nasm" or toolchain.get("version") != EXPECTED_NASM:
        raise LayoutError("toolchain must pin NASM 2.16.01")
    if toolchain.get("output") != "bin" or toolchain.get("python") != "python3":
        raise LayoutError("toolchain output must be NASM bin with python3")
    artifact = _mapping(root.get("artifact"), "artifact")
    if artifact.get("extension_status") != "deferred-to-3.5a-1" or artifact.get("golden_tracked") is not False:
        raise LayoutError("artifact must remain unselected and without a golden image")
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
        raise LayoutError("nasm is required but was not found on PATH")
    try:
        result = subprocess.run([executable, "-v"], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise LayoutError(f"unable to query nasm: {exc}") from exc
    match = re.search(r"NASM version ([0-9]+(?:\.[0-9]+){1,2})", result.stdout + result.stderr)
    version = match.group(1) if match else "unknown"
    if version != expected:
        raise LayoutError(f"NASM {expected} is required (found {version})")
    return version


def build(layout_path: Path, output_path: Path, sha256_path: Path | None = None,
          manifest_path: Path | None = None) -> str:
    layout = load_layout(layout_path)
    nasm_version = check_nasm(layout["toolchain"]["version"])
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
        "toolchain": {"assembler": "nasm", "version": nasm_version, "python": "python3"},
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
