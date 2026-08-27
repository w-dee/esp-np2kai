#!/usr/bin/env python3
"""Validate the structural E1 OPNGEN fixture log (without golden values)."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


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


def validate_text(text: str) -> list[str]:
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
        expected = {"version": "1", "rate_hz": "48000", "frames": "28800",
                    "channels": "2", "pcm_bytes": "115200", "volume": "128"}
        for key, value in expected.items():
            if meta.get(key) != value:
                errors.append(f"META {key}={meta.get(key)!r}, expected {value!r}")

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
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    errors = validate_text(args.log.read_text(encoding="utf-8", errors="replace"))
    if errors:
        for error in errors:
            print(f"E1_OPNGEN_VALIDATION_ERROR {error}")
        print("E1_OPNGEN_VALIDATION=FAIL")
        return 1
    print("E1_OPNGEN_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
