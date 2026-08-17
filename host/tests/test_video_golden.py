#!/usr/bin/env python3
"""Negative and parser tests for the tracked np2video golden descriptor."""

from __future__ import annotations

import pathlib
import sys

from run_video_golden import GoldenError, load_descriptor, validate_stdout


ROOT = pathlib.Path(__file__).resolve().parents[2]
DESCRIPTOR = ROOT / "tests/guest/np2video/golden.json"


def _valid_output(descriptor: dict[str, object]) -> bytes:
    return (
        f"NP2VIDEO_FIXTURE scene_id={descriptor['scene_id']} "
        f"fixture_sha256={descriptor['fixture_sha256']} image_bytes={descriptor['image_size']}\n"
        f"NP2VIDEO_READY scene_id={descriptor['scene_id']} state=SCENE_READY "
        "generation=1 surface_update_sequence=0\n"
        f"NP2VIDEO_FRAMEBUFFER scene_id={descriptor['scene_id']} "
        f"width={descriptor['width']} height={descriptor['height']} "
        f"bytes={descriptor['visible_bytes']} format={descriptor['pixel_format']} "
        f"bpp={descriptor['bpp']} pitch={descriptor['pitch']} generation=1 "
        "surface_update_sequence=1 "
        f"crc_algorithm={descriptor['crc_algorithm']} crc32={descriptor['crc32']} "
        "storage_external=0\n"
        "NP2VIDEO_RESULT=REFERENCE_READY\n"
    ).encode("ascii")


def _must_reject(output: bytes, descriptor: dict[str, object]) -> None:
    try:
        validate_stdout(output, descriptor)
    except GoldenError:
        return
    raise AssertionError("golden checker accepted invalid input")


def main() -> int:
    descriptor = load_descriptor(DESCRIPTOR)
    valid = _valid_output(descriptor)
    validate_stdout(valid, descriptor)

    wrong_sha = valid.replace(
        f"fixture_sha256={descriptor['fixture_sha256']}".encode(),
        b"fixture_sha256=" + b"0" * 64,
        1,
    )
    _must_reject(wrong_sha, descriptor)

    wrong_crc = valid.replace(
        f"crc32={descriptor['crc32']}".encode(), b"crc32=0xffffffff", 1
    )
    _must_reject(wrong_crc, descriptor)

    wrong_width = valid.replace(
        f"width={descriptor['width']}".encode(), b"width=639", 1
    )
    _must_reject(wrong_width, descriptor)
    wrong_height = valid.replace(
        f"height={descriptor['height']}".encode(), b"height=399", 1
    )
    _must_reject(wrong_height, descriptor)
    wrong_pixel_format = valid.replace(
        f"format={descriptor['pixel_format']}".encode(), b"format=rgb555le", 1
    )
    _must_reject(wrong_pixel_format, descriptor)

    missing_field = valid.replace(f" pitch={descriptor['pitch']}".encode(), b"", 1)
    _must_reject(missing_field, descriptor)
    missing_result = valid.replace(b"NP2VIDEO_RESULT=REFERENCE_READY\n", b"", 1)
    _must_reject(missing_result, descriptor)
    malformed_result = valid.replace(
        b"NP2VIDEO_RESULT=REFERENCE_READY", b"NP2VIDEO_RESULT=PASS", 1
    )
    _must_reject(malformed_result, descriptor)
    print("video golden checker negative tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
