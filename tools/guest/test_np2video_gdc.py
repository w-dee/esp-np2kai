#!/usr/bin/env python3
"""Structural and reproducibility checks for the Step 7A.3d GDC fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/guest/np2video-gdc"
LAYOUT = FIXTURE / "layout.json"
BUILDER = ROOT / "tools/guest/build_np2video_stage2.py"
IMAGE_SIZE = 1_261_568
IPL_SIZE = 1024
MAX_STAGE2_SIZE = 32768
SECTOR_SIZE = 1024
DRAW_LINE_PATTERN = re.compile(r"^    DRAW_LINE (.+)$", re.MULTILINE)


def run_build(layout: pathlib.Path, output: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(BUILDER), "--layout", str(layout), "--output", str(output)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def copy_fixture(temp: pathlib.Path, layout: dict, stage2_source: str | None = None) -> pathlib.Path:
    (temp / "src").mkdir(parents=True)
    shutil.copy2(FIXTURE / "src/ipl.asm", temp / "src/ipl.asm")
    if stage2_source is None:
        shutil.copy2(FIXTURE / "src/stage2.asm", temp / "src/stage2.asm")
    else:
        (temp / "src/stage2.asm").write_text(stage2_source, encoding="ascii")
    layout_path = temp / "layout.json"
    layout_path.write_text(json.dumps(layout, indent=2) + "\n", encoding="utf-8")
    return layout_path


def expect_rejected(name: str, layout: dict, temp_root: pathlib.Path) -> None:
    case = temp_root / name
    case.mkdir()
    layout_path = copy_fixture(case, layout)
    result = run_build(layout_path, case / "bad.image")
    if result.returncode == 0:
        raise AssertionError(f"{name}: malformed fixture was accepted")


def assert_layout_contract(layout: dict) -> None:
    assert layout["name"] == "np2video-gdc"
    assert layout["fixture"] == {
        "id": "np2video-7a3d-gdc",
        "scene_id": 3,
        "description": "640x400 deterministic GDC VECTL analog 16-color reference scene",
    }
    assert layout["scene"]["width"] == 640
    assert layout["scene"]["height"] == 400
    assert layout["scene"]["gdc_commands"] is True
    assert layout["scene"]["primitive"] == "VECTL"
    assert layout["gdc"]["operation"] == {
        "command": "0x2b",
        "name": "SET",
        "persistent": True,
    }
    assert layout["gdc"]["plane_selectors"] == {"0": "E", "1": "B", "2": "R", "3": "G"}
    assert layout["gdc"]["textw"]["bytes"] == [255, 255, 0, 0, 0, 0, 0, 0]
    assert layout["payload"] == {
        "implemented": True,
        "source": "src/stage2.asm",
        "format": "flat-16-bit",
        "disk_offset": 1024,
        "sector_offset": 1,
        "max_size_bytes": 32768,
        "max_sector_count": 32,
        "load_physical": 131072,
        "load_segment": 8192,
        "entry_offset": 8,
        "header_size": 8,
        "padding": "zero",
    }
    assert len(layout["scene"]["lines"]) == 14


def parse_draw_calls(source: str) -> list[tuple[int, ...]]:
    calls: list[tuple[int, ...]] = []
    for raw in DRAW_LINE_PATTERN.findall(source):
        values = tuple(int(value.strip(), 0) for value in raw.split(","))
        if len(values) != 11:
            raise AssertionError(f"DRAW_LINE has {len(values)} fields: {raw}")
        calls.append(values)
    return calls


def assert_source_contract(layout: dict) -> None:
    source = (FIXTURE / "src/stage2.asm").read_text(encoding="ascii")
    if "VECTR" in source or "VECTC" in source or "VECTT" in source:
        raise AssertionError("forbidden non-VECTL GDC operation appears in stage2")
    for required in (
        "GDC_STATUS_BUSY",
        "GDC_STATUS_FIFO_EMPTY",
        "GDC_STATUS_FIFO_FULL",
        "gdc_wait_complete",
        "gdc_wait_fifo_space",
        "gdc_set_csrw",
        "gdc_set_vectw",
        "gdc_execute_vector",
        "mov al, 0x2b",
        "mov al, 0x78",
        "mov al, 0x6c",
        "clear_planes",
        "STATE_SCENE_READY",
        "STATE_ERROR",
    ):
        if required not in source:
            raise AssertionError(f"stage2 is missing required GDC contract element: {required}")

    expected_calls = []
    for line in layout["scene"]["lines"]:
        expected_calls.append(
            (
                *line["from"],
                *line["to"],
                line["color"],
                int(line["ope"], 0),
                line["DC"],
                line["D"],
                line["D2"],
                line["D1"],
                line["DM"],
            )
        )
    if parse_draw_calls(source) != expected_calls:
        raise AssertionError("stage2 DRAW_LINE calls do not match layout scene")


def test_generic_builder_rejections(layout: dict, temp_root: pathlib.Path) -> None:
    temp_root.mkdir(parents=True, exist_ok=True)
    invalid_name = json.loads(json.dumps(layout))
    invalid_name["name"] = ""
    expect_rejected("empty-project-name", invalid_name, temp_root)

    invalid_fixture = json.loads(json.dumps(layout))
    invalid_fixture["fixture"]["id"] = "fixture id with spaces"
    expect_rejected("invalid-fixture-id", invalid_fixture, temp_root)

    invalid_scene = json.loads(json.dumps(layout))
    invalid_scene["fixture"]["scene_id"] = 0
    expect_rejected("non-positive-scene-id", invalid_scene, temp_root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reproducibility", action="store_true")
    args = parser.parse_args()
    layout = json.loads(LAYOUT.read_text(encoding="utf-8"))
    assert_layout_contract(layout)
    assert_source_contract(layout)

    with tempfile.TemporaryDirectory(prefix="np2video-gdc-test-") as temp_name:
        temp = pathlib.Path(temp_name)
        first = temp / "first.image"
        first_result = run_build(LAYOUT, first)
        if first_result.returncode != 0:
            raise AssertionError(first_result.stderr)
        first_bytes = first.read_bytes()
        if len(first_bytes) != IMAGE_SIZE:
            raise AssertionError(f"fixture size is {len(first_bytes)}, expected {IMAGE_SIZE}")
        if first_bytes[510:512] != b"\x55\xaa" or first_bytes[1022:1024] != b"\x55\xaa":
            raise AssertionError("IPL signatures are incorrect")

        manifest = json.loads(
            first.with_name(first.name + ".manifest.json").read_text(encoding="utf-8")
        )
        stage2_size = manifest["stage2_size"]
        stage2_sectors = manifest["stage2_sector_count"]
        if not 8 <= stage2_size <= MAX_STAGE2_SIZE:
            raise AssertionError("stage2 size is outside the loader contract")
        if stage2_sectors != (stage2_size + SECTOR_SIZE - 1) // SECTOR_SIZE:
            raise AssertionError("stage2 sector count is not derived from its byte size")
        stage2 = first_bytes[IPL_SIZE : IPL_SIZE + stage2_size]
        if stage2[:4] != b"ST2V" or int.from_bytes(stage2[4:6], "little") != 1:
            raise AssertionError("stage2 header is invalid")
        if int.from_bytes(stage2[6:8], "little") != stage2_size:
            raise AssertionError("stage2 header size is invalid")
        payload_end = IPL_SIZE + stage2_sectors * SECTOR_SIZE
        if first_bytes[IPL_SIZE + stage2_size : payload_end] != b"\0" * (
            payload_end - IPL_SIZE - stage2_size
        ):
            raise AssertionError("stage2 sector padding is not zero")
        if first_bytes[payload_end:] != b"\0" * (IMAGE_SIZE - payload_end):
            raise AssertionError("bytes after the stage2 payload are not zero")

        first_digest = hashlib.sha256(first_bytes).hexdigest()
        if manifest["sha256"] != first_digest:
            raise AssertionError("manifest SHA-256 does not match image")
        if manifest["fixture_id"] != layout["fixture"]["id"] or manifest["scene_id"] != 3:
            raise AssertionError("manifest fixture identity is incorrect")

        second = temp / "second.image"
        second_result = run_build(LAYOUT, second)
        if second_result.returncode != 0:
            raise AssertionError(second_result.stderr)
        second_bytes = second.read_bytes()
        second_digest = hashlib.sha256(second_bytes).hexdigest()
        if second_bytes != first_bytes or second_digest != first_digest:
            raise AssertionError("two fixture builds are not byte-identical")

        test_generic_builder_rejections(layout, temp / "rejections")

    print(
        f"np2video-gdc fixture tests passed stage2_size={stage2_size} "
        f"stage2_sectors={stage2_sectors} sha256_run1={first_digest} "
        f"sha256_run2={second_digest} reproducible=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
