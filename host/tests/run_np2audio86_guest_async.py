#!/usr/bin/env python3
"""Validate the live 86R.4A producer/transport/worker proof."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


GOLDEN_FIELDS = (
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
    "AUDIO86_GUEST_ASYNC_LIVE_I286_PRODUCER",
    "AUDIO86_GUEST_ASYNC_EVENT_TRANSPORT",
    "AUDIO86_GUEST_ASYNC_PCM_BYTES",
    "AUDIO86_GUEST_ASYNC_GLOBAL_ORDER",
    "AUDIO86_GUEST_ASYNC_RESET_ACK",
    "AUDIO86_GUEST_ASYNC_FINALIZE",
    "AUDIO86_GUEST_ASYNC_DOMAIN_OWNERSHIP",
    "AUDIO86_GUEST_ASYNC_WORKER_TRACE",
    "AUDIO86_GUEST_ASYNC_PRE_RESET_PCM",
    "AUDIO86_GUEST_ASYNC_FULL_PCM",
    "AUDIO86_GUEST_ASYNC_86R3_EXACT",
    "AUDIO86_GUEST_ASYNC_DETERMINISM",
    "AUDIO86_GUEST_ASYNC_INPUT_IMMUTABLE",
    "AUDIO86_GUEST_ASYNC_RESULT",
)

COUNTERS = {
    "AUDIO86_GUEST_ASYNC_ACTIONS_PUBLISHED": "19",
    "AUDIO86_GUEST_ASYNC_ACTIONS_CONSUMED": "19",
    "AUDIO86_GUEST_ASYNC_DATA_RUNS_PUBLISHED": "1",
    "AUDIO86_GUEST_ASYNC_DATA_RUNS_CONSUMED": "1",
    "AUDIO86_GUEST_ASYNC_BYTES_PUBLISHED": "8",
    "AUDIO86_GUEST_ASYNC_BYTES_CONSUMED": "8",
    "AUDIO86_GUEST_ASYNC_RESETS_PUBLISHED": "1",
    "AUDIO86_GUEST_ASYNC_RESETS_ACKNOWLEDGED": "1",
    "AUDIO86_GUEST_ASYNC_FINAL_NEXT_SEQUENCE": "19",
    "AUDIO86_GUEST_ASYNC_EVENT_RESIDUAL": "0",
    "AUDIO86_GUEST_ASYNC_BYTE_RESIDUAL": "0",
}


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in values:
            raise RuntimeError(f"duplicate marker: {key}")
        values[key] = value
    return values


def load_golden(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise RuntimeError("unsupported 86R.3 golden schema")
    values = document.get("values")
    if not isinstance(values, dict):
        raise RuntimeError("86R.3 golden values missing")
    return {field: str(values[field]) for field in GOLDEN_FIELDS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=10)
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    args = parser.parse_args()
    if args.repetitions < 10 or args.timeout_seconds <= 0:
        raise RuntimeError("86R.4A requires >=10 repetitions and positive timeout")
    golden = load_golden(args.golden)
    snapshots: list[dict[str, str]] = []
    for _ in range(args.repetitions):
        completed = subprocess.run(
            [str(args.binary)], check=True, capture_output=True, text=True,
            timeout=args.timeout_seconds,
        )
        values = parse(completed.stdout)
        for key, expected in golden.items():
            if values.get(key) != expected:
                raise RuntimeError(
                    f"86R.3 differential mismatch {key}: "
                    f"got {values.get(key)!r}, expected {expected!r}"
                )
        for marker in MARKERS:
            if values.get(marker) != "PASS":
                raise RuntimeError(f"missing {marker}=PASS")
        for key, expected in COUNTERS.items():
            if values.get(key) != expected:
                raise RuntimeError(f"counter mismatch {key}: {values.get(key)!r}")
        snapshots.append(values)
    if any(snapshot != snapshots[0] for snapshot in snapshots[1:]):
        raise RuntimeError("live async fresh-process output is not identical")
    print(f"AUDIO86_GUEST_ASYNC_REPETITIONS=PASS count={args.repetitions}")
    print("AUDIO86_GUEST_ASYNC_GOLDEN=PASS")
    print("AUDIO86_GUEST_ASYNC_DETERMINISM=PASS")
    print("AUDIO86_GUEST_ASYNC_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
