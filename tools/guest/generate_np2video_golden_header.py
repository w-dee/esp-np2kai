#!/usr/bin/env python3
"""Generate the build-only ESP np2video golden header from golden.json."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


class GoldenError(ValueError):
    pass


FIXTURE_ID_PATTERN = re.compile(r"[A-Za-z0-9._-]{1,63}")


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise GoldenError(f"{name} must be a non-negative integer")
    return value


def _load(path: Path) -> dict[str, Any]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GoldenError(f"cannot read golden descriptor: {error}") from error
    if not isinstance(root, dict):
        raise GoldenError("golden descriptor must be an object")
    if root.get("schema_version") != 1:
        raise GoldenError("golden schema_version must be 1")
    required = (
        "fixture_id", "scene_id", "fixture_sha256", "image_size", "width", "height", "bpp",
        "pitch", "visible_bytes", "pixel_format", "crc_algorithm", "crc32",
    )
    for name in required:
        if name not in root:
            raise GoldenError(f"golden descriptor is missing {name}")
    fixture_id = root["fixture_id"]
    if (not isinstance(fixture_id, str) or
            FIXTURE_ID_PATTERN.fullmatch(fixture_id) is None):
        raise GoldenError(
            "fixture_id must contain 1-63 ASCII letters, digits, '.', '_' or '-'")
    digest = root["fixture_sha256"]
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise GoldenError("fixture_sha256 must be 64 lowercase hexadecimal digits")
    crc = root["crc32"]
    if not isinstance(crc, str) or re.fullmatch(r"0x[0-9a-fA-F]{8}", crc) is None:
        raise GoldenError("crc32 must be an 8-digit hexadecimal value with 0x prefix")
    if root["pixel_format"] != "rgb565le":
        raise GoldenError("unsupported pixel_format")
    if root["crc_algorithm"] != "crc32_iso_hdlc":
        raise GoldenError("unsupported crc_algorithm")
    values = {
        name: _integer(root[name], name)
        for name in ("scene_id", "image_size", "width", "height", "bpp",
                     "pitch", "visible_bytes")
    }
    values["fixture_id"] = fixture_id
    values["fixture_sha256"] = digest
    values["crc32"] = int(crc, 16)
    return values


def _header(values: dict[str, Any]) -> str:
    digest = ", ".join(f"0x{byte:02x}" for byte in bytes.fromhex(values["fixture_sha256"]))
    return f"""/* Generated from golden.json; do not edit or commit. */
#ifndef NP2VIDEO_GOLDEN_H
#define NP2VIDEO_GOLDEN_H

#include <stdint.h>

static const char np2video_golden_fixture_id[] = "{values["fixture_id"]}";
static const uint16_t np2video_golden_scene_id = {values["scene_id"]}U;
static const uint32_t np2video_golden_fixture_image_size = {values["image_size"]}U;
static const uint8_t np2video_golden_fixture_sha256[32] = {{
    {digest}
}};
static const uint16_t np2video_golden_width = {values["width"]}U;
static const uint16_t np2video_golden_height = {values["height"]}U;
static const uint32_t np2video_golden_pixel_format_rgb565le = 1U;
static const uint16_t np2video_golden_bpp = {values["bpp"]}U;
static const uint32_t np2video_golden_pitch = {values["pitch"]}U;
static const uint32_t np2video_golden_visible_bytes = {values["visible_bytes"]}U;
static const uint32_t np2video_golden_crc_algorithm_iso_hdlc = 1U;
static const uint32_t np2video_golden_crc_version = 1U;
static const uint32_t np2video_golden_crc32 = 0x{values["crc32"]:08x}U;

#endif /* NP2VIDEO_GOLDEN_H */
"""


def generate(descriptor: Path, output: Path) -> None:
    values = _load(descriptor)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(_header(values), encoding="ascii", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.descriptor, args.output)
    except GoldenError as error:
        parser.error(str(error))
    print(f"np2video golden header={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
