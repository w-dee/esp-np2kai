#!/usr/bin/env python3
"""Negative tests for the JSON-driven ESP np2video log validator."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

from validate_np2video_esp_log import ValidationError, _load_descriptor, validate_text


ROOT = Path(__file__).resolve().parents[2]
DESCRIPTOR_PATHS = (
    ROOT / "tests/guest/np2video/golden.json",
    ROOT / "tests/guest/np2video-gfx-vram/golden.json",
)


def valid_log(descriptor: dict[str, object]) -> str:
    frame_descriptor = dict(descriptor)
    frame_descriptor["crc32"] = f"0x{descriptor['crc32']:08x}"
    return "\n".join([
        "NP2VIDEO_PROFILE profile=esp32p4-reduced-video formal_extmem=13 effective_extmem=8",
        "NP2VIDEO_MEMORY extmem_mb=8 actual_bytes=8388608 ptr_external=1",
        "NP2VIDEO_FIXTURE fixture_id={fixture_id} scene_id={scene_id} fixture_sha256={fixture_sha256} image_bytes={image_size} partition=np2test".format(**descriptor),
        "NP2VIDEO_READY fixture_id={fixture_id} scene_id={scene_id} state={ready_state} generation=7 surface_update_sequence=3".format(**descriptor),
        "NP2VIDEO_FRAMEBUFFER fixture_id={fixture_id} scene_id={scene_id} width={width} height={height} bytes={visible_bytes} format={pixel_format} bpp={bpp} pitch={pitch} generation=7 surface_update_sequence=4 crc_algorithm={crc_algorithm} crc32={crc32} storage_external=1".format(**frame_descriptor),
        "NP2VIDEO_GOLDEN_RESULT=PASS",
    ])


def expect_rejected(name: str, text: str, descriptor: dict[str, object]) -> None:
    try:
        validate_text(text, descriptor)
    except ValidationError:
        return
    raise AssertionError(f"validator accepted invalid log: {name}")


def replace_line(text: str, prefix: str, replacement: str) -> str:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines[index] = replacement
            return "\n".join(lines)
    raise AssertionError(f"missing line for test: {prefix}")


def expect_descriptor_rejected(name: str, root: dict[str, object]) -> None:
    with tempfile.TemporaryDirectory(prefix="np2video-esp-descriptor-") as directory:
        path = Path(directory) / "golden.json"
        path.write_text(json.dumps(root), encoding="utf-8")
        try:
            _load_descriptor(path)
        except ValidationError:
            return
    raise AssertionError(f"validator accepted invalid descriptor: {name}")


def main() -> int:
    # The test derives its valid values from the tracked descriptor and never
    # copies the approved SHA or CRC into test source.
    total_cases = 0
    for descriptor_path in DESCRIPTOR_PATHS:
        descriptor = _load_descriptor(descriptor_path)
        baseline = valid_log(descriptor)
        wrong_crc = f"0x{descriptor['crc32'] ^ 1:08x}"
        wrong_sha = "0" * 64
        wrong_fixture = "synthetic-wrong-fixture"

        cases = {
            "wrong fixture_id in fixture": replace_line(
                baseline, "NP2VIDEO_FIXTURE",
                baseline.splitlines()[2].replace(
                    f"fixture_id={descriptor['fixture_id']}",
                    f"fixture_id={wrong_fixture}"),
            ),
            "wrong fixture_id in READY": replace_line(
                baseline, "NP2VIDEO_READY",
                baseline.splitlines()[3].replace(
                    f"fixture_id={descriptor['fixture_id']}",
                    f"fixture_id={wrong_fixture}"),
            ),
            "wrong fixture_id in framebuffer": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace(
                    f"fixture_id={descriptor['fixture_id']}",
                    f"fixture_id={wrong_fixture}"),
            ),
            "wrong fixture SHA": replace_line(
                baseline, "NP2VIDEO_FIXTURE",
                baseline.splitlines()[2].replace(descriptor["fixture_sha256"], wrong_sha),
            ),
            "wrong framebuffer CRC": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace(f"0x{descriptor['crc32']:08x}", wrong_crc),
            ),
            "wrong width": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace(f"width={descriptor['width']}", f"width={descriptor['width'] + 1}"),
            ),
            "wrong height": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace(f"height={descriptor['height']}", f"height={descriptor['height'] + 1}"),
            ),
            "wrong format": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace(f"format={descriptor['pixel_format']}", "format=invalid"),
            ),
            "missing READY": "\n".join(line for line in baseline.splitlines() if not line.startswith("NP2VIDEO_READY")),
            "generation changed": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace("generation=7", "generation=8"),
            ),
            "sequence did not advance": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace("surface_update_sequence=4", "surface_update_sequence=3"),
            ),
            "EXTMEM not external": replace_line(
                baseline, "NP2VIDEO_MEMORY",
                baseline.splitlines()[1].replace("ptr_external=1", "ptr_external=0"),
            ),
            "framebuffer not external": replace_line(
                baseline, "NP2VIDEO_FRAMEBUFFER",
                baseline.splitlines()[4].replace("storage_external=1", "storage_external=0"),
            ),
            "duplicate terminal": baseline + "\nNP2VIDEO_GOLDEN_RESULT=PASS",
            "FAIL terminal": baseline.replace("NP2VIDEO_GOLDEN_RESULT=PASS", "NP2VIDEO_GOLDEN_RESULT=FAIL reason=synthetic"),
            "HARNESS_ERROR terminal": baseline.replace("NP2VIDEO_GOLDEN_RESULT=PASS", "NP2VIDEO_GOLDEN_RESULT=HARNESS_ERROR reason=synthetic"),
            "malformed field": replace_line(
                baseline, "NP2VIDEO_READY",
                baseline.splitlines()[3].replace("generation=7", "generation"),
            ),
        }
        for name, text in cases.items():
            expect_rejected(name, text, descriptor)
        validate_text(baseline, descriptor)
        total_cases += len(cases)

    for descriptor_path in DESCRIPTOR_PATHS:
        raw_descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
        missing_fixture = dict(raw_descriptor)
        missing_fixture.pop("fixture_id")
        expect_descriptor_rejected("missing fixture_id", missing_fixture)
        malformed_fixture = dict(raw_descriptor)
        malformed_fixture["fixture_id"] = "fixture id with spaces"
        expect_descriptor_rejected("malformed fixture_id", malformed_fixture)
        total_cases += 2

    print(f"np2video ESP log validator negative tests: PASS ({total_cases} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
