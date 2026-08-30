#!/usr/bin/env python3
"""Validate the deterministic 86R.3 synchronous guest PCM oracle."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


FIELD_NAMES = (
    "SOURCE_86R2_HEAD",
    "AUDIO_EVENTS_SEMANTIC_COUNT",
    "AUDIO_EVENTS_SERIALIZED_BYTES",
    "AUDIO_EVENTS_CRC32",
    "AUDIO_EVENTS_SHA256",
    "PCM86_BYTES_PAYLOAD_BYTES",
    "PCM86_BYTES_SERIALIZED_BYTES",
    "PCM86_BYTES_CRC32",
    "PCM86_BYTES_SHA256",
    "PCM86_DATA_RUNS_SEMANTIC_COUNT",
    "PCM86_DATA_RUNS_PAYLOAD_BYTES",
    "PCM86_DATA_RUNS_SERIALIZED_BYTES",
    "PCM86_DATA_RUNS_CRC32",
    "PCM86_DATA_RUNS_SHA256",
    "WORKER_APPLY_TRACE_SEMANTIC_COUNT",
    "WORKER_APPLY_TRACE_SERIALIZED_BYTES",
    "WORKER_APPLY_TRACE_CRC32",
    "WORKER_APPLY_TRACE_SHA256",
    "HIGHEST_EVENT_FRAME",
    "RENDER_HORIZON_FRAMES",
    "TAIL_FRAMES",
    "PRE_RESET_PCM_FRAMES",
    "PRE_RESET_PCM_BYTES",
    "PRE_RESET_PCM_CRC32",
    "PRE_RESET_PCM_SHA256",
    "PRE_RESET_PCM_PEAK",
    "PRE_RESET_PCM_NONZERO",
    "PRE_RESET_PCM_FIRST_NONZERO",
    "PRE_RESET_PCM_CLAMP",
    "FULL_REPLAY_PCM_FRAMES",
    "FULL_REPLAY_PCM_BYTES",
    "FULL_REPLAY_PCM_CRC32",
    "FULL_REPLAY_PCM_SHA256",
    "FULL_REPLAY_PCM_PEAK",
    "FULL_REPLAY_PCM_NONZERO",
    "FULL_REPLAY_PCM_FIRST_NONZERO",
    "FULL_REPLAY_PCM_CLAMP",
    "FM_COVERAGE",
    "PSG_COVERAGE",
    "RHYTHM_COVERAGE",
    "PCM86_COVERAGE",
)

MARKERS = (
    "AUDIO86_GUEST_SYNC_INPUT",
    "AUDIO86_GUEST_SYNC_GLOBAL_SEQUENCE_VALIDATION",
    "MERGED_ACTION_SEQUENCE_VALIDATION",
    "PCM_BYTE_IMMUTABILITY_CHECKER_SENSITIVE",
    "AUDIO86_GUEST_REPLAY_EVENTS_IMMUTABLE",
    "AUDIO86_GUEST_REPLAY_DATA_RUNS_IMMUTABLE",
    "AUDIO86_GUEST_REPLAY_PCM_BYTES_IMMUTABLE",
    "AUDIO86_GUEST_SYNC_WORKER_APPLY",
    "AUDIO86_GUEST_SYNC_DOMAIN_A_PCM_SPLIT",
    "AUDIO86_GUEST_SYNC_FAIL_CLOSED",
    "AUDIO86_GUEST_SYNC_PCM",
    "AUDIO86_GUEST_SYNC_SERIALIZED_REPLAY",
    "AUDIO86_GUEST_SYNC_QUANTUM_INDEPENDENCE",
    "AUDIO86_GUEST_REPLAY_INPUT_IMMUTABLE",
    "AUDIO86_GUEST_SYNC_PCM_DETERMINISM",
    "AUDIO86_GUEST_SYNC_NEGATIVE_TESTS",
    "AUDIO86_GUEST_SYNC_BOUNDARY_TESTS",
    "AUDIO86_GUEST_SYNC_RESULT",
)


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in FIELD_NAMES or key in MARKERS:
            if key in values:
                raise RuntimeError(f"duplicate oracle marker: {key}")
            values[key] = value
    return values


def expected(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise RuntimeError("unsupported 86R.3 golden schema")
    values = document.get("values")
    if not isinstance(values, dict):
        raise RuntimeError("golden values missing")
    result = {name: str(values[name]) for name in FIELD_NAMES}
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.repetitions < 3:
        raise RuntimeError("86R.3 requires at least three fresh processes")
    golden = expected(args.golden)
    snapshots: list[dict[str, str]] = []
    for _ in range(args.repetitions):
        completed = subprocess.run(
            [str(args.binary)], check=True, capture_output=True, text=True
        )
        values = parse(completed.stdout)
        for key, value in golden.items():
            if values.get(key) != value:
                raise RuntimeError(
                    f"{key} mismatch: got {values.get(key)!r}, expected {value!r}"
                )
        for marker in MARKERS:
            if values.get(marker) != "PASS":
                raise RuntimeError(f"missing {marker}=PASS")
        if values.get("PCM86_COVERAGE") != "NOT_EXERCISED":
            raise RuntimeError("PCM86 coverage classification changed")
        snapshots.append(values)
    if any(snapshot != snapshots[0] for snapshot in snapshots[1:]):
        raise RuntimeError("fresh-process synchronous output is not identical")
    print(f"AUDIO86_GUEST_SYNC_PCM_REPETITIONS=PASS count={args.repetitions}")
    print("AUDIO86_GUEST_SYNC_PCM_GOLDEN=PASS")
    print("AUDIO86_GUEST_SYNC_PCM_DETERMINISM=PASS")
    print("AUDIO86_GUEST_SYNC_PCM_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
