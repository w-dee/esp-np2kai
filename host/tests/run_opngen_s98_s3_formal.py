#!/usr/bin/env python3
"""Run the project-generated S98 pipeline in fresh generator/process pairs."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools" / "emu"))


FIXTURES = (
    "fm_single_tone",
    "fm_frequency_change",
    "fm_three_channel",
    "fm_same_timestamp_burst",
    "fm_envelope",
    "fm_algorithm_feedback",
)

EXPECTED = {
    "fm_single_tone": (89, 12, "8fac7f6d", 2400, 9600, "768635f5"),
    "fm_frequency_change": (76, 7, "5bf3a304", 320, 1280, "fcc39d65"),
    "fm_three_channel": (105, 18, "fba7a7f3", 1200, 4800, "bf3ee216"),
    "fm_same_timestamp_burst": (71, 7, "5804f6b4", 48, 192, "de71c38a"),
    "fm_envelope": (87, 12, "62660d17", 384, 1536, "25850aa0"),
    "fm_algorithm_feedback": (80, 9, "6c5d3ff1", 384, 1536, "6c1ce17e"),
}


def fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            raise ValueError(f"malformed token: {token}")
        key, value = token.split("=", 1)
        if key in result:
            raise ValueError(f"duplicate field: {key}")
        result[key] = value
    return result


def parse_output(text: str, fixture: str) -> dict[str, str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    result_lines = [line for line in lines if line.startswith("S98_S3_RESULT ")]
    if result_lines != [f"S98_S3_RESULT fixture={fixture} PASS"]:
        raise ValueError("missing or duplicate terminal PASS result")
    if lines[-1] != result_lines[0]:
        raise ValueError("terminal result is not final")
    records: dict[str, str] = {}
    for prefix in ("S98_S3_SOURCE", "S98_S3_META", "S98_S3_EVENTS", "S98_S3_PCM"):
        matches = [line for line in lines if line.startswith(prefix + " ")]
        if len(matches) != 1:
            raise ValueError(f"expected one {prefix} line")
        records[prefix] = matches[0]
    records["SOURCE_FIELDS"] = fields(records["S98_S3_SOURCE"])
    records["META_FIELDS"] = fields(records["S98_S3_META"])
    records["EVENTS_FIELDS"] = fields(records["S98_S3_EVENTS"])
    records["PCM_FIELDS"] = fields(records["S98_S3_PCM"])
    return records


def run_process(binary: Path, generator: Path, fixture: str,
                run_dir: Path, descriptor: dict[str, object] | None) -> tuple[dict[str, str], bytes, bytes]:
    run_dir.mkdir()
    generated = subprocess.run(
        [sys.executable, str(generator), "--output-dir", str(run_dir)],
        capture_output=True, text=True, check=False, timeout=30,
    )
    if generated.returncode != 0:
        raise RuntimeError(f"generator failed for {fixture}:\n{generated.stdout}\n{generated.stderr}")
    source = (run_dir / f"{fixture}.s98").read_bytes()
    pcm_path = run_dir / f"{fixture}.pcm"
    executed = subprocess.run(
        [str(binary), "--fixture-dir", str(run_dir), "--fixture", fixture,
         "--pcm-out", str(pcm_path)],
        capture_output=True, text=True, check=False, timeout=30,
    )
    if executed.returncode != 0:
        raise RuntimeError(f"pipeline failed for {fixture}:\n{executed.stdout}\n{executed.stderr}")
    records = parse_output(executed.stdout, fixture)
    if descriptor is not None:
        from validate_opngen_s98_fixture_log import validate_text
        errors = validate_text(executed.stdout, descriptor, fixture)
        if errors:
            raise ValueError(f"{fixture}: descriptor validation failed: {'; '.join(errors)}")
    return records, source, pcm_path.read_bytes()


def check_candidate(fixture: str, records: dict[str, str], source: bytes,
                    pcm: bytes) -> None:
    source_fields = records["SOURCE_FIELDS"]
    meta = records["META_FIELDS"]
    events = records["EVENTS_FIELDS"]
    pcm_fields = records["PCM_FIELDS"]
    (
        expected_source_bytes,
        expected_count,
        expected_event_crc,
        expected_end,
        expected_pcm_bytes,
        expected_pcm_crc,
    ) = EXPECTED[fixture]
    source_sha = hashlib.sha256(source).hexdigest()
    if (len(source), int(source_fields["source_bytes"]),
            source_fields["source_sha256"]) != (len(source), len(source), source_sha):
        raise ValueError(f"{fixture}: source identity mismatch")
    if len(source) != expected_source_bytes:
        raise ValueError(f"{fixture}: source size candidate changed")
    if int(events["preflight_count"]) != expected_count or \
            events["preflight_crc32"] != expected_event_crc:
        raise ValueError(f"{fixture}: event candidate changed")
    for view in ("preflight", "producer", "consumer"):
        if events[f"{view}_count"] != events["preflight_count"] or \
                events[f"{view}_crc32"] != events["preflight_crc32"] or \
                events[f"{view}_sha256"] != events["preflight_sha256"]:
            raise ValueError(f"{fixture}: three-way event identity mismatch")
    if events["sequence_errors"] != "0":
        raise ValueError(f"{fixture}: sequence errors")
    pcm_sha = hashlib.sha256(pcm).hexdigest()
    pcm_crc = f"{zlib.crc32(pcm) & 0xffffffff:08x}"
    if pcm_fields["sha256"] != pcm_sha or pcm_fields["crc32"] != pcm_crc:
        raise ValueError(f"{fixture}: PCM digest does not cover collected bytes")
    if int(meta["end_frame"]) != expected_end or \
            int(pcm_fields["frames"]) != expected_end or \
            int(pcm_fields["bytes"]) != expected_pcm_bytes or \
            pcm_fields["crc32"] != expected_pcm_crc or len(pcm) != expected_pcm_bytes:
        raise ValueError(f"{fixture}: PCM candidate changed")
    if int(meta["declared_clock"]) != 3993600 or \
            meta["clock_policy"] != "EXACT_NP2" or \
            meta["ignored_writes"] != "0" or meta["loop_offset"] != "0":
        raise ValueError(f"{fixture}: parser metadata contract changed")
    if fixture == "fm_same_timestamp_burst" and (
            events["same_timestamp_pairs"] != "5" or events["preflight_count"] != "7"):
        raise ValueError("same-timestamp semantic assertion failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--descriptor", type=Path)
    args = parser.parse_args()
    if args.repetitions <= 0:
        parser.error("repetitions must be positive")
    if not args.binary.is_file() or not args.generator.is_file():
        parser.error("binary and generator must exist")
    descriptor = None
    if args.descriptor is not None:
        try:
            from validate_opngen_s98_fixture_log import load_descriptor
            descriptor = load_descriptor(args.descriptor)
        except (OSError, ValueError) as error:
            parser.error(f"invalid descriptor: {error}")

    try:
        with tempfile.TemporaryDirectory(prefix="opngen-s98-s3-formal-", dir=args.binary.parent) as root:
            root_path = Path(root)
            for fixture in FIXTURES:
                expected_records = None
                expected_source = None
                expected_pcm = None
                for iteration in range(1, args.repetitions + 1):
                    records, source, pcm = run_process(
                        args.binary, args.generator, fixture,
                        root_path / f"{fixture}-{iteration}", descriptor)
                    check_candidate(fixture, records, source, pcm)
                    if expected_records is None:
                        expected_records = records
                        expected_source = source
                        expected_pcm = pcm
                    elif records != expected_records or source != expected_source or pcm != expected_pcm:
                        raise ValueError(f"{fixture}: identity changed at repetition {iteration}")
                assert expected_records is not None
                print(expected_records["S98_S3_SOURCE"])
                print(expected_records["S98_S3_META"])
                print(expected_records["S98_S3_EVENTS"])
                print(expected_records["S98_S3_PCM"])
                print(f"S98_S3_FORMAL fixture={fixture} repetitions={args.repetitions}/{args.repetitions} source_bytes={len(expected_source or b'')} pcm_bytes={len(expected_pcm or b'')} direct_source_memcmp=PASS full_pcm_memcmp=PASS")
        print("S98_S3_FORMAL_RESULT=PASS")
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"S98_S3_FORMAL_RESULT=FAIL\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
