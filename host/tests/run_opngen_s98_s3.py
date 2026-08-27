#!/usr/bin/env python3
"""Repeat the S3 S98 -> SPSC -> bounded OPNGEN integration in fresh processes."""

import argparse
import subprocess
import tempfile
from pathlib import Path


FIXTURES = (
    "fm_single_tone",
    "fm_frequency_change",
    "fm_three_channel",
    "fm_same_timestamp_burst",
    "fm_envelope",
    "fm_algorithm_feedback",
)


def parse_output(text: str) -> dict[str, str]:
    records: dict[str, str] = {}
    for line in text.splitlines():
        if not line.startswith(("S98_S3_SOURCE ", "S98_S3_META ", "S98_S3_EVENTS ", "S98_S3_PCM ", "S98_S3_RESULT ")):
            continue
        fields = line.split()
        key = fields[0]
        records[key] = " ".join(fields[1:])
    if "S98_S3_RESULT" not in records or not records["S98_S3_RESULT"].endswith(" PASS"):
        raise RuntimeError(f"integration binary did not report PASS:\n{text}")
    return records


def run_once(binary: Path, fixture_dir: Path, fixture: str, pcm_path: Path) -> dict[str, str]:
    completed = subprocess.run(
        [str(binary), "--fixture-dir", str(fixture_dir), "--fixture", fixture, "--pcm-out", str(pcm_path)],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{fixture} integration failed ({completed.returncode}):\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return parse_output(completed.stdout)


def run_failure(binary: Path, fixture_dir: Path) -> None:
    completed = subprocess.run(
        [str(binary), "--fixture-dir", str(fixture_dir), "--failure-test"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    expected = "S98_S3_FAILURE second_pass_parser=PASS first_error=GENERATOR"
    if completed.returncode != 0 or expected not in completed.stdout:
        raise RuntimeError(
            f"second-pass failure test failed ({completed.returncode}):\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    print("S98_S3_FAILURE_RESULT=PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    args = parser.parse_args()

    if not args.binary.is_file() or not args.fixture_dir.is_dir():
        parser.error("binary and fixture directory must exist")

    run_failure(args.binary, args.fixture_dir)
    with tempfile.TemporaryDirectory(prefix="opngen-s98-s3-", dir=args.fixture_dir.parent) as temporary:
        temporary_dir = Path(temporary)
        for fixture in FIXTURES:
            first = run_once(args.binary, args.fixture_dir, fixture, temporary_dir / f"{fixture}.run1.pcm")
            second = run_once(args.binary, args.fixture_dir, fixture, temporary_dir / f"{fixture}.run2.pcm")
            if first != second:
                raise RuntimeError(f"{fixture} structured identity changed between fresh processes")
            first_pcm = (temporary_dir / f"{fixture}.run1.pcm").read_bytes()
            second_pcm = (temporary_dir / f"{fixture}.run2.pcm").read_bytes()
            if first_pcm != second_pcm:
                raise RuntimeError(f"{fixture} full PCM bytes changed between fresh processes")
            for key in ("S98_S3_SOURCE", "S98_S3_META", "S98_S3_EVENTS", "S98_S3_PCM"):
                print(f"{key} {first[key]}")
            print(
                f"S98_S3_INVARIANTS fixture={fixture} parser_repeat=PASS "
                "transport_identity=PASS pcm_repeat=PASS source_immutable=PASS"
            )
            print(f"S98_S3_RESULT fixture={fixture} PASS")
            print(f"S98_S3_REPEAT fixture={fixture} fresh_processes=2 pcm_memcmp=PASS")
    print("S98_S3_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
