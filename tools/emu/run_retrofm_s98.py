#!/usr/bin/env python3
"""Repeat the curated RetroFM S98 -> SPSC -> OPNGEN pipeline."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


def _fields(line: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in line.split()[1:]:
        key, separator, value = token.partition("=")
        if not separator or key in values:
            raise ValueError(f"malformed output token: {token}")
        values[key] = value
    return values


def _parse(output: str) -> dict[str, dict[str, str]]:
    records: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        for prefix in ("S98_S3_SOURCE", "S98_S3_META", "S98_S3_EVENTS", "S98_S3_PCM"):
            if line.startswith(prefix + " "):
                if prefix in records:
                    raise ValueError(f"duplicate {prefix}")
                records[prefix] = _fields(line)
    if "S98_S3_RESULT fixture=retrofm_pocket_demo PASS" not in output:
        raise ValueError(f"integration did not PASS:\n{output}")
    if set(records) != {"S98_S3_SOURCE", "S98_S3_META", "S98_S3_EVENTS", "S98_S3_PCM"}:
        raise ValueError("integration output is incomplete")
    return records


def _check(records: dict[str, dict[str, str]], source: bytes,
           expected_source_sha: str, expected_source_bytes: int) -> None:
    source_fields = records["S98_S3_SOURCE"]
    meta = records["S98_S3_META"]
    events = records["S98_S3_EVENTS"]
    pcm = records["S98_S3_PCM"]
    source_sha = hashlib.sha256(source).hexdigest()
    if (int(source_fields["source_bytes"]), source_fields["source_sha256"]) != (
        len(source), source_sha
    ):
        raise ValueError("source identity does not cover fixture bytes")
    if (len(source), source_sha) != (expected_source_bytes, expected_source_sha):
        raise ValueError("source identity differs from provenance manifest")
    for key, expected in {
        "s98_version": "3", "device_count": "1", "device_type": "2",
        "declared_clock": "4000000", "effective_clock": "3993600",
        "clock_policy": "WORKLOAD_CLOCK_MISMATCH", "raw_timer": "1/44100",
        "effective_timer": "1/44100", "data_offset": "48",
        "loop_offset": "0", "source_writes": "1047", "ignored_writes": "0",
        "final_sync": "530082", "end_frame": "576960",
    }.items():
        if meta.get(key) != expected:
            raise ValueError(f"metadata {key}={meta.get(key)!r}, expected {expected!r}")
    for view in ("preflight", "producer", "consumer"):
        if (events.get(f"{view}_count") != events.get("preflight_count") or
                events.get(f"{view}_crc32") != events.get("preflight_crc32") or
                events.get(f"{view}_sha256") != events.get("preflight_sha256")):
            raise ValueError("preflight/producer/consumer event identity mismatch")
    if events.get("preflight_count") != "1047" or events.get("sequence_errors") != "0":
        raise ValueError("event count or sequence error contract failed")
    if pcm.get("frames") != "576960" or pcm.get("bytes") != "2307840":
        raise ValueError("PCM size contract failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.repetitions < 3:
        parser.error("at least three fresh processes are required")
    source = args.fixture.read_bytes()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    expected = manifest["strict_derivative"]
    expected_sha = expected["sha256"]
    expected_bytes = expected["bytes"]
    if hashlib.sha256(source).hexdigest() != expected_sha or len(source) != expected_bytes:
        raise SystemExit("fixture does not match strict_derivative manifest")
    with tempfile.TemporaryDirectory(prefix="retrofm-s98-", dir=args.binary.parent) as root:
        root_path = Path(root)
        first_records: dict[str, dict[str, str]] | None = None
        first_pcm: bytes | None = None
        for iteration in range(1, args.repetitions + 1):
            pcm_path = root_path / f"run-{iteration}.pcm"
            completed = subprocess.run(
                [str(args.binary), "--fixture-file", str(args.fixture),
                 "--pcm-out", str(pcm_path)],
                capture_output=True, text=True, check=False, timeout=60,
            )
            if completed.returncode != 0:
                raise SystemExit(
                    f"fresh process {iteration} failed:\n{completed.stdout}\n{completed.stderr}"
                )
            records = _parse(completed.stdout)
            pcm = pcm_path.read_bytes()
            _check(records, source, expected_sha, expected_bytes)
            if first_records is None:
                first_records = records
                first_pcm = pcm
            elif records != first_records or pcm != first_pcm:
                raise SystemExit(f"identity changed at fresh process {iteration}")
        assert first_records is not None and first_pcm is not None
        events = first_records["S98_S3_EVENTS"]
        pcm = first_records["S98_S3_PCM"]
        print("RETROFM_PIPELINE=PASS fresh_processes="
              f"{args.repetitions} full_pcm_memcmp=PASS")
        print("RETROFM_EVENTS " + " ".join(
            f"{key}={events[key]}" for key in (
                "preflight_count", "preflight_crc32", "preflight_sha256",
                "producer_count", "producer_crc32", "producer_sha256",
                "consumer_count", "consumer_crc32", "consumer_sha256",
                "sequence_errors")))
        print("RETROFM_PCM " + " ".join(
            f"{key}={pcm[key]}" for key in ("frames", "bytes", "crc32", "sha256")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
