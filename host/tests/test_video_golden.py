#!/usr/bin/env python3
"""Negative and parser tests for the tracked np2video golden descriptors."""

from __future__ import annotations

import pathlib
import tempfile

from run_video_golden import GoldenError, load_descriptor, validate_stdout


ROOT = pathlib.Path(__file__).resolve().parents[2]
DESCRIPTORS = (
    ROOT / "tests/guest/np2video/golden.json",
    ROOT / "tests/guest/np2video-gfx-vram/golden.json",
    ROOT / "tests/guest/np2video-gdc/golden.json",
)


def _valid_output(descriptor: dict[str, object]) -> bytes:
    return (
        f"NP2VIDEO_FIXTURE fixture_id={descriptor['fixture_id']} scene_id={descriptor['scene_id']} "
        f"fixture_sha256={descriptor['fixture_sha256']} image_bytes={descriptor['image_size']}\n"
        f"NP2VIDEO_READY fixture_id={descriptor['fixture_id']} scene_id={descriptor['scene_id']} state=SCENE_READY "
        "generation=1 surface_update_sequence=0\n"
        f"NP2VIDEO_FRAMEBUFFER fixture_id={descriptor['fixture_id']} scene_id={descriptor['scene_id']} "
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
    for descriptor_path in DESCRIPTORS:
        descriptor = load_descriptor(descriptor_path)
        valid = _valid_output(descriptor)
        validate_stdout(valid, descriptor)

        wrong_sha = valid.replace(
            f"fixture_sha256={descriptor['fixture_sha256']}".encode(),
            b"fixture_sha256=" + b"0" * 64,
            1,
        )
        _must_reject(wrong_sha, descriptor)

        wrong_fixture = valid.replace(
            f"fixture_id={descriptor['fixture_id']}".encode(),
            b"fixture_id=synthetic-wrong-fixture",
            1,
        )
        _must_reject(wrong_fixture, descriptor)

        wrong_crc = valid.replace(
            f"crc32={descriptor['crc32']}".encode(), b"crc32=0xffffffff", 1
        )
        _must_reject(wrong_crc, descriptor)

        wrong_scene = valid.replace(
            f"scene_id={descriptor['scene_id']}".encode(), b"scene_id=65535", 1
        )
        _must_reject(wrong_scene, descriptor)

        for field, value in (
            ("width", b"639"), ("height", b"399"), ("bpp", b"15"),
            ("pitch", b"1278"), ("bytes", b"511998"),
        ):
            wrong_field = valid.replace(
                f"{field}={descriptor['width' if field == 'width' else 'height' if field == 'height' else field if field != 'bytes' else 'visible_bytes']}".encode(),
                field.encode() + b"=" + value,
                1,
            )
            _must_reject(wrong_field, descriptor)
        wrong_pixel_format = valid.replace(
            f"format={descriptor['pixel_format']}".encode(), b"format=rgb555le", 1
        )
        _must_reject(wrong_pixel_format, descriptor)
        wrong_crc_algorithm = valid.replace(
            f"crc_algorithm={descriptor['crc_algorithm']}".encode(),
            b"crc_algorithm=crc32_wrong",
            1,
        )
        _must_reject(wrong_crc_algorithm, descriptor)

        changed_generation = valid.replace(
            b"generation=1 surface_update_sequence=1",
            b"generation=2 surface_update_sequence=1",
            1,
        )
        _must_reject(changed_generation, descriptor)
        no_update = valid.replace(
            b"generation=1 surface_update_sequence=1",
            b"generation=1 surface_update_sequence=0",
            1,
        )
        _must_reject(no_update, descriptor)
        missing_field = valid.replace(f" pitch={descriptor['pitch']}".encode(), b"", 1)
        _must_reject(missing_field, descriptor)
        missing_result = valid.replace(b"NP2VIDEO_RESULT=REFERENCE_READY\n", b"", 1)
        _must_reject(missing_result, descriptor)
        duplicate_result = valid + b"NP2VIDEO_RESULT=REFERENCE_READY\n"
        _must_reject(duplicate_result, descriptor)
        malformed_result = valid.replace(
            b"NP2VIDEO_RESULT=REFERENCE_READY", b"NP2VIDEO_RESULT=PASS", 1
        )
        _must_reject(malformed_result, descriptor)

    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as malformed:
        malformed.write("{}")
        malformed.flush()
        try:
            load_descriptor(pathlib.Path(malformed.name))
        except GoldenError:
            pass
        else:
            raise AssertionError("malformed descriptor was accepted")
    print("video golden checker negative tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
