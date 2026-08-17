#!/usr/bin/env python3
"""Deterministic checks for the Step 7A.3c direct-VRAM fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/guest/np2video-gfx-vram"
LAYOUT = FIXTURE / "layout.json"
GOLDEN = FIXTURE / "golden.json"
BUILDER = ROOT / "tools/guest/build_np2video_stage2.py"
IMAGE_SIZE = 1_261_568
IPL_SIZE = 1024
MAX_STAGE2_SIZE = 32768
SECTOR_SIZE = 1024


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


def expect_rejected(name: str, layout: dict, temp_root: pathlib.Path, stage2_source: str | None = None) -> None:
    case = temp_root / name
    case.mkdir()
    layout_path = copy_fixture(case, layout, stage2_source)
    result = run_build(layout_path, case / "bad.image")
    if result.returncode == 0:
        raise AssertionError(f"{name}: malformed fixture was accepted")


def assert_layout_contract(layout: dict) -> None:
    assert layout["fixture"]["id"] == "np2video-7a3c-gfx-vram"
    assert layout["fixture"]["scene_id"] == 2
    assert layout["scene"]["width"] == 640
    assert layout["scene"]["height"] == 400
    assert layout["scene"]["gdc_commands"] is False
    assert layout["graphics_vram"]["bytes_per_row"] == 80
    assert layout["graphics_vram"]["pixels_per_byte"] == 8
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
    assert layout["palette"]["entries"] == [
        "000", "007", "070", "077", "700", "707", "770", "777",
        "444", "00f", "0f0", "0ff", "f00", "f0f", "ff0", "fff",
    ]


def test_rejections(layout: dict, temp_root: pathlib.Path) -> None:
    temp_root.mkdir(parents=True, exist_ok=True)
    expect_rejected(
        "oversized",
        layout,
        temp_root,
        "bits 16\norg 0\ntimes 32769 db 0\n",
    )
    expect_rejected("empty", layout, temp_root, "bits 16\norg 0\n")
    expect_rejected(
        "bad-magic",
        layout,
        temp_root,
        'bits 16\norg 0\ndb "BAD!"\ndw 1\ndw 8\n',
    )

    wrong_load = json.loads(json.dumps(layout))
    wrong_load["payload"]["load_physical"] = 0x21000
    expect_rejected("wrong-load-address", wrong_load, temp_root)

    overlap = json.loads(json.dumps(layout))
    overlap["memory"]["excluded_ranges"].append(
        {"name": "bad-stage2-overlap", "start": 0x20000, "size_bytes": 1024}
    )
    expect_rejected("ram-overlap", overlap, temp_root)

    bad_ipl_size = json.loads(json.dumps(layout))
    bad_ipl_size["ipl"]["binary_size"] = 1023
    expect_rejected("invalid-ipl-size", bad_ipl_size, temp_root)

    bad_signatures = json.loads(json.dumps(layout))
    bad_signatures["ipl"]["signatures"][0]["bytes"] = "aaaa"
    expect_rejected("bad-signatures", bad_signatures, temp_root)

    disk_overflow = json.loads(json.dumps(layout))
    disk_overflow["payload"]["disk_offset"] = 1025
    expect_rejected("payload-disk-overflow", disk_overflow, temp_root)

    max_sectors = json.loads(json.dumps(layout))
    max_sectors["payload"]["max_sector_count"] = 31
    expect_rejected("max-sector-count-restriction", max_sectors, temp_root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reproducibility", action="store_true")
    args = parser.parse_args()
    layout = json.loads(LAYOUT.read_text(encoding="utf-8"))
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    assert_layout_contract(layout)
    assert golden["fixture_id"] == layout["fixture"]["id"]
    assert golden["scene_id"] == layout["fixture"]["scene_id"]

    with tempfile.TemporaryDirectory(prefix="np2video-gfx-vram-test-") as temp_name:
        temp = pathlib.Path(temp_name)
        first = temp / "first.image"
        first_result = run_build(LAYOUT, first)
        if first_result.returncode != 0:
            raise AssertionError(first_result.stderr)
        first_bytes = first.read_bytes()
        assert len(first_bytes) == IMAGE_SIZE
        assert first_bytes[510:512] == b"\x55\xaa"
        assert first_bytes[1022:1024] == b"\x55\xaa"

        manifest = json.loads(
            first.with_name(first.name + ".manifest.json").read_text(encoding="utf-8")
        )
        stage2_size = manifest["stage2_size"]
        stage2_sectors = manifest["stage2_sector_count"]
        assert 8 <= stage2_size <= MAX_STAGE2_SIZE
        assert stage2_sectors == (stage2_size + SECTOR_SIZE - 1) // SECTOR_SIZE
        stage2 = first_bytes[IPL_SIZE : IPL_SIZE + stage2_size]
        assert stage2[:4] == b"ST2V"
        assert int.from_bytes(stage2[4:6], "little") == 1
        assert int.from_bytes(stage2[6:8], "little") == stage2_size
        payload_end = IPL_SIZE + stage2_sectors * SECTOR_SIZE
        assert first_bytes[IPL_SIZE + stage2_size : payload_end] == b"\0" * (
            payload_end - IPL_SIZE - stage2_size
        )
        assert first_bytes[payload_end:] == b"\0" * (IMAGE_SIZE - payload_end)

        digest = hashlib.sha256(first_bytes).hexdigest()
        assert digest == golden["fixture_sha256"]
        assert manifest["sha256"] == digest
        assert manifest["stage2_sha256"] == hashlib.sha256(stage2).hexdigest()
        sidecar = first.with_name(first.name + ".sha256").read_text(encoding="ascii")
        assert sidecar == f"{digest}  {first.name}\n"

        second = temp / "second.image"
        second_result = run_build(LAYOUT, second)
        if second_result.returncode != 0:
            raise AssertionError(second_result.stderr)
        assert second.read_bytes() == first_bytes
        assert hashlib.sha256(second.read_bytes()).hexdigest() == digest

        test_rejections(layout, temp / "rejections")

    print(
        f"np2video-gfx-vram fixture tests passed stage2_size={stage2_size} "
        f"stage2_sectors={stage2_sectors} sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
