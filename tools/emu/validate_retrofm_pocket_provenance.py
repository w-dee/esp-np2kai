#!/usr/bin/env python3
"""Fail-closed validation for the curated RetroFM provenance manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TOP_KEYS = {"manifest_version", "upstream", "source_vgm", "converter",
            "raw_derived_s98", "curation", "strict_derivative"}
RAW_SHA256 = "87de61e3d155d8ef9b44e78fae2d59204785ab4885539e38462d82fde7666a28"
STRICT_SHA256 = "702d8b3003d2d81449fd1003aa2231afdacaae9d680f73fdf11d8195edb046c2"
SOURCE_SHA256 = "b71eb66b4b54771e92b23b2e300889c163dfbbc815a8e946af04e4ea10617142"


def _keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        actual = set(value) if isinstance(value, dict) else set()
        raise ValueError(f"{label} keys mismatch missing={sorted(expected - actual)} "
                         f"extra={sorted(actual - expected)}")


def _equal(value: Any, expected: Any, label: str) -> None:
    if value != expected:
        raise ValueError(f"{label} mismatch: {value!r} != {expected!r}")


def _artifact(root: Path, record: dict[str, Any], label: str) -> bytes:
    path = root / record["path"]
    if not path.is_file():
        raise ValueError(f"{label} artifact is missing: {record['path']}")
    data = path.read_bytes()
    _equal(len(data), record["bytes"], f"{label}.bytes")
    _equal(hashlib.sha256(data).hexdigest(), record["sha256"], f"{label}.sha256")
    return data


def _reject_pcm_keys(value: Any, path: str = "manifest") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key.lower() in {"pcm", "pcm_crc32", "pcm_sha256"}:
                raise ValueError(f"candidate PCM identity is not allowed: {path}.{key}")
            _reject_pcm_keys(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _reject_pcm_keys(child, f"{path}[{index}]")


def validate_manifest(path: Path, repository_root: Path = REPOSITORY_ROOT) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    _keys(document, TOP_KEYS, "manifest")
    _reject_pcm_keys(document)
    _equal(document["manifest_version"], 1, "manifest_version")

    upstream = document["upstream"]
    _keys(upstream, {"repository", "commit", "generator_path", "generator_bytes",
                     "generator_sha256", "license", "provenance"}, "upstream")
    for key, expected in {
        "repository": "https://github.com/Keitark/RetroFM-Pocket",
        "commit": "1a6f7e9ac0d8bd7ad0a52219a0700f981be9aebb",
        "generator_path": "scripts/generate_retrofm_demo.py",
        "generator_bytes": 5219,
        "generator_sha256": "d54a2f38cfadfe19154a2289696f4b7646854b18a968bdb0a02e47034b5d0981",
        "license": "MIT",
        "provenance": "deterministic original rights-cleared FM demo; generated VGM contains no third-party music data",
    }.items():
        _equal(upstream[key], expected, f"upstream.{key}")

    source = document["source_vgm"]
    _keys(source, {"path", "bytes", "sha256", "license", "declared_ym2203_clock_hz",
                   "total_samples", "version", "data_offset", "loop_offset", "gd3_offset"},
          "source_vgm")
    for key, expected in {
        "path": "testdata/vgm/retrofm-pocket-demo.vgm", "bytes": 4194,
        "sha256": SOURCE_SHA256, "license": "CC0-1.0",
        "declared_ym2203_clock_hz": 4000000, "total_samples": 530082,
        "version": 337, "data_offset": 256, "loop_offset": 0, "gd3_offset": 3794,
    }.items():
        _equal(source[key], expected, f"source_vgm.{key}")
    _artifact(repository_root, source, "source_vgm")

    converter = document["converter"]
    _keys(converter, {"repository", "commit", "script_path", "script_bytes",
                      "script_sha256", "command", "license_status"}, "converter")
    for key, expected in {
        "repository": "https://github.com/autch/vgm2s98",
        "commit": "5a1204c245789971a2bc32501ab96b7a48bf5e87",
        "script_path": "vgm2s98.py", "script_bytes": 16379,
        "script_sha256": "e457fe9dd436cb7ff2da5a817fc6eef1ddf6991c9a66c7969a429b2e3ed3e11f",
        "command": "python3 <fixed-vgm2s98>/vgm2s98.py retrofm-pocket-demo.vgm retrofm-pocket-demo.s98 --sync 1/44100",
        "license_status": "NOT VENDORABLE / NO SOURCE COPYING",
    }.items():
        _equal(converter[key], expected, f"converter.{key}")

    raw = document["raw_derived_s98"]
    _keys(raw, {"bytes", "sha256", "timer_numerator", "timer_denominator",
                "compression", "device_count", "device_type",
                "declared_device_clock_hz", "pan", "reserved", "data_offset",
                "tag_offset", "loop_offset", "write_count", "final_sync"},
          "raw_derived_s98")
    for key, expected in {
        "bytes": 3762, "sha256": RAW_SHA256, "timer_numerator": 1,
        "timer_denominator": 44100, "compression": 0, "device_count": 1,
        "device_type": 2, "declared_device_clock_hz": 4000000, "pan": 0,
        "reserved": 0, "data_offset": 48, "tag_offset": 3587,
        "loop_offset": 0, "write_count": 1050, "final_sync": 530082,
    }.items():
        _equal(raw[key], expected, f"raw_derived_s98.{key}")

    curation = document["curation"]
    _keys(curation, {"tool_path", "input_sha256", "removed_write_count",
                     "removed_writes", "preservation"}, "curation")
    _equal(curation["tool_path"], "tools/emu/curate_retrofm_s98.py", "curation.tool_path")
    _equal(curation["input_sha256"], RAW_SHA256, "curation.input_sha256")
    _equal(curation["removed_write_count"], 3, "curation.removed_write_count")
    expected_removed = [
        {"write_index": 0, "sync": 0, "register": 34, "value": 0, "occurrence_count": 1},
        {"write_index": 1, "sync": 0, "register": 39, "value": 0, "occurrence_count": 1},
        {"write_index": 2, "sync": 0, "register": 7, "value": 63, "occurrence_count": 1},
    ]
    _equal(curation["removed_writes"], expected_removed, "curation.removed_writes")
    _equal(curation["preservation"], "all other writes, register values, order, waits, cumulative timing, FD, and tag bytes are preserved", "curation.preservation")
    if not (repository_root / curation["tool_path"]).is_file():
        raise ValueError("curation tool is missing")

    derivative = document["strict_derivative"]
    _keys(derivative, {"path", "bytes", "sha256", "s98_version", "timer_numerator",
                       "timer_denominator", "compression", "device_count",
                       "device_type", "declared_device_clock_hz",
                       "effective_opngen_clock_hz", "clock_policy", "data_offset",
                       "tag_offset", "loop_offset", "write_count",
                       "ignored_write_count", "final_sync", "end_frame"},
          "strict_derivative")
    for key, expected in {
        "path": "testdata/s98/retrofm-pocket-demo-strict.s98", "bytes": 3753,
        "sha256": STRICT_SHA256, "s98_version": 3, "timer_numerator": 1,
        "timer_denominator": 44100, "compression": 0, "device_count": 1,
        "device_type": 2, "declared_device_clock_hz": 4000000,
        "effective_opngen_clock_hz": 3993600,
        "clock_policy": "WORKLOAD_CLOCK_MISMATCH", "data_offset": 48,
        "tag_offset": 3578, "loop_offset": 0, "write_count": 1047,
        "ignored_write_count": 0, "final_sync": 530082, "end_frame": 576960,
    }.items():
        _equal(derivative[key], expected, f"strict_derivative.{key}")
    _artifact(repository_root, derivative, "strict_derivative")
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("retrofm_pocket_fixture_provenance.json"))
    args = parser.parse_args()
    try:
        validate_manifest(args.manifest)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"RETROFM_PROVENANCE=FAIL reason={error}")
        return 1
    print("RETROFM_PROVENANCE=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
