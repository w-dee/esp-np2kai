#!/usr/bin/env python3
"""Validate an E1 OPNGEN fixture log against the accepted deterministic golden."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


PREFIXES = (
    "E1_OPNGEN_META",
    "E1_OPNGEN_TABLE",
    "E1_OPNGEN_PCM",
    "E1_OPNGEN_METRICS",
    "E1_OPNGEN_INVARIANTS",
    "E1_OPNGEN_TIMING",
)


def one(lines: list[str], prefix: str, errors: list[str]) -> str:
    matches = [line for line in lines if line.startswith(prefix + " ")]
    if len(matches) != 1:
        errors.append(f"expected exactly one {prefix} line, got {len(matches)}")
        return ""
    return matches[0]


def fields(line: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def require(values: dict[str, str], keys: tuple[str, ...], label: str,
            errors: list[str]) -> None:
    for key in keys:
        if key not in values or values[key] == "":
            errors.append(f"{label}: missing {key}")


def decimal(value: str, label: str, errors: list[str], signed: bool = False) -> None:
    pattern = r"-?[0-9]+" if signed else r"[0-9]+"
    if not re.fullmatch(pattern, value):
        errors.append(f"{label}: malformed decimal {value!r}")


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def load_golden(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("golden_version") != 1:
        raise ValueError("golden_version must be 1")

    fixture = document.get("fixture")
    if not isinstance(fixture, dict):
        raise ValueError("golden fixture descriptor is missing")
    for key in ("version", "rate_hz", "frames", "channels", "pcm_bytes", "volume"):
        if not _is_int(fixture.get(key)):
            raise ValueError(f"golden fixture {key} must be an integer")
    if fixture.get("canonical_pcm_format") != "s16le-interleaved-stereo":
        raise ValueError("golden canonical PCM format is invalid")
    if not isinstance(fixture.get("vector_revision"), str) or not fixture["vector_revision"]:
        raise ValueError("golden vector_revision is missing")

    provenance = document.get("provenance")
    if not isinstance(provenance, dict):
        raise ValueError("golden provenance is missing")
    commit = provenance.get("np2kai_upstream_commit")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("golden NP2kai commit is malformed")
    vendor_sha256 = provenance.get("vendor_sha256")
    if not isinstance(vendor_sha256, dict):
        raise ValueError("golden vendor SHA-256 map is missing")
    for name in ("opngenc.c", "opngeng.c", "opngencfg.h"):
        value = vendor_sha256.get(name)
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError(f"golden vendor SHA-256 is malformed for {name}")

    deterministic = document.get("deterministic")
    if not isinstance(deterministic, dict):
        raise ValueError("golden deterministic descriptor is missing")
    table = deterministic.get("table")
    if not isinstance(table, dict):
        raise ValueError("golden table descriptor is missing")
    for key in ("ratebit", "calc1024", "fmvol"):
        if not _is_int(table.get(key)):
            raise ValueError(f"golden table {key} must be an integer")
    for key in ("sintable_crc32", "envtable_crc32", "envcurve_crc32"):
        value = table.get(key)
        if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-f]{8}", value):
            raise ValueError(f"golden table {key} is malformed")

    pcm = deterministic.get("pcm")
    if not isinstance(pcm, dict):
        raise ValueError("golden PCM descriptor is missing")
    if pcm.get("crc_algorithm") != "crc32_iso_hdlc":
        raise ValueError("golden PCM CRC algorithm is invalid")
    for key in ("crc32",):
        value = pcm.get(key)
        if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-f]{8}", value):
            raise ValueError(f"golden PCM {key} is malformed")
    sha256 = pcm.get("sha256")
    if not isinstance(sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise ValueError("golden PCM SHA-256 is malformed")
    for key in ("s32_abs_peak", "nonzero_s16_samples", "clip_samples"):
        if not _is_int(pcm.get(key)):
            raise ValueError(f"golden PCM {key} must be an integer")

    metrics = deterministic.get("metrics")
    if not isinstance(metrics, dict):
        raise ValueError("golden metrics descriptor is missing")
    for key in ("l_sumsq", "r_sumsq", "l_rms_q16", "r_rms_q16"):
        if not _is_int(metrics.get(key)):
            raise ValueError(f"golden metrics {key} must be an integer")

    invariants = deterministic.get("invariants")
    if not isinstance(invariants, dict):
        raise ValueError("golden invariant descriptor is missing")
    for key in ("repeat", "partition_240", "partition_1", "silence",
                "frequency_change", "keyoff", "nontrivial"):
        if invariants.get(key) != "PASS":
            raise ValueError(f"golden invariant {key} is not PASS")
    return document


def _compare_expected(values: dict[str, str], expected: dict[str, Any],
                      label: str, errors: list[str]) -> None:
    for key, value in expected.items():
        expected_value = str(value)
        if values.get(key) != expected_value:
            errors.append(f"{label} {key}={values.get(key)!r}, expected {expected_value!r}")


def validate_text(text: str, golden: dict[str, Any] | None = None) -> list[str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    errors: list[str] = []
    parsed = {prefix: one(lines, prefix, errors) for prefix in PREFIXES}
    results = [line for line in lines if line.startswith("E1_OPNGEN_RESULT=")]
    if results != ["E1_OPNGEN_RESULT=PASS"]:
        errors.append("expected exactly one terminal E1_OPNGEN_RESULT=PASS")
    if any("E1_OPNGEN_RESULT=FAIL" in line for line in lines):
        errors.append("failure terminal is present")

    meta = fields(parsed["E1_OPNGEN_META"]) if parsed["E1_OPNGEN_META"] else {}
    if meta:
        require(meta, ("version", "rate_hz", "frames", "channels", "pcm_bytes", "volume"),
                "META", errors)
        for key in ("version", "rate_hz", "frames", "channels", "pcm_bytes", "volume"):
            if key in meta:
                decimal(meta[key], f"META {key}", errors)

    table = fields(parsed["E1_OPNGEN_TABLE"]) if parsed["E1_OPNGEN_TABLE"] else {}
    if table:
        require(table, ("ratebit", "calc1024", "fmvol", "sintable_crc32",
                        "envtable_crc32", "envcurve_crc32"), "TABLE", errors)
        for key in ("ratebit", "calc1024", "fmvol"):
            if key in table:
                decimal(table[key], f"TABLE {key}", errors, signed=True)
        for key in ("sintable_crc32", "envtable_crc32", "envcurve_crc32"):
            if key in table and not re.fullmatch(r"0x[0-9a-fA-F]{8}", table[key]):
                errors.append(f"TABLE {key}: malformed CRC")

    pcm = fields(parsed["E1_OPNGEN_PCM"]) if parsed["E1_OPNGEN_PCM"] else {}
    if pcm:
        require(pcm, ("crc_algorithm", "crc32", "sha256", "s32_abs_peak",
                      "nonzero_s16_samples", "clip_samples"), "PCM", errors)
        if pcm.get("crc_algorithm") != "crc32_iso_hdlc":
            errors.append("PCM crc_algorithm is not crc32_iso_hdlc")
        if "crc32" in pcm and not re.fullmatch(r"0x[0-9a-fA-F]{8}", pcm["crc32"]):
            errors.append("PCM crc32 is malformed")
        if "sha256" in pcm and not re.fullmatch(r"[0-9a-fA-F]{64}", pcm["sha256"]):
            errors.append("PCM sha256 is malformed")
        for key in ("s32_abs_peak", "nonzero_s16_samples", "clip_samples"):
            if key in pcm:
                decimal(pcm[key], f"PCM {key}", errors)

    metrics = fields(parsed["E1_OPNGEN_METRICS"]) if parsed["E1_OPNGEN_METRICS"] else {}
    if metrics:
        require(metrics, ("l_sumsq", "r_sumsq", "l_rms_q16", "r_rms_q16"),
                "METRICS", errors)
        for key in ("l_sumsq", "r_sumsq", "l_rms_q16", "r_rms_q16"):
            if key in metrics:
                decimal(metrics[key], f"METRICS {key}", errors)

    invariants = fields(parsed["E1_OPNGEN_INVARIANTS"]) if parsed["E1_OPNGEN_INVARIANTS"] else {}
    if invariants:
        required = ("repeat", "partition_240", "partition_1", "silence",
                    "frequency_change", "keyoff", "nontrivial")
        require(invariants, required, "INVARIANTS", errors)
        for key in required:
            if invariants.get(key) != "PASS":
                errors.append(f"INVARIANTS {key} is not PASS")

    timing = fields(parsed["E1_OPNGEN_TIMING"]) if parsed["E1_OPNGEN_TIMING"] else {}
    if timing:
        require(timing, ("init_us", "render_us", "us_per_frame_q16",
                         "realtime_factor_q16"), "TIMING", errors)
        for key in ("init_us", "render_us", "us_per_frame_q16", "realtime_factor_q16"):
            if key in timing:
                decimal(timing[key], f"TIMING {key}", errors)
    if golden is not None and not errors:
        fixture = golden["fixture"]
        deterministic = golden["deterministic"]
        _compare_expected(meta, {
            "version": fixture["version"],
            "rate_hz": fixture["rate_hz"],
            "frames": fixture["frames"],
            "channels": fixture["channels"],
            "pcm_bytes": fixture["pcm_bytes"],
            "volume": fixture["volume"],
        }, "GOLDEN META", errors)
        _compare_expected(table, deterministic["table"], "GOLDEN TABLE", errors)
        _compare_expected(pcm, deterministic["pcm"], "GOLDEN PCM", errors)
        _compare_expected(metrics, deterministic["metrics"], "GOLDEN METRICS", errors)
        _compare_expected(invariants, deterministic["invariants"],
                          "GOLDEN INVARIANTS", errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--golden", required=True, type=Path)
    args = parser.parse_args()
    try:
        golden = load_golden(args.golden)
        errors = validate_text(
            args.log.read_text(encoding="utf-8", errors="replace"), golden)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors = [f"golden: {error}"]
    if errors:
        for error in errors:
            print(f"E1_OPNGEN_VALIDATION_ERROR {error}")
        print("E1_OPNGEN_VALIDATION=FAIL")
        return 1
    print("E1_OPNGEN_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
