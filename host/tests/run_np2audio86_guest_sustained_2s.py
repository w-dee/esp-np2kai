#!/usr/bin/env python3
"""Freeze and validate the two-second real-i286 guest PCM oracle."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


FIELD_NAMES = (
    "WORKLOAD_ID",
    "SEMANTIC_DURATION_MS",
    "SUSTAINED_Q240_UNITS",
    "ORIGINAL_GUEST_PROGRAM_SERIALIZED_BYTES",
    "ORIGINAL_GUEST_PROGRAM_CRC32",
    "ORIGINAL_GUEST_PROGRAM_SHA256",
    "SUSTAINED_GUEST_PROGRAM_SERIALIZED_BYTES",
    "SUSTAINED_GUEST_PROGRAM_CRC32",
    "SUSTAINED_GUEST_PROGRAM_SHA256",
    "SUSTAINED_LOOP_COUNT",
    "SUSTAINED_LOOP_INNER_COUNT",
    "SUSTAINED_LOOP_CYCLES",
    "FIRST_GUEST_IO_FRAME",
    "LAST_GUEST_IO_FRAME",
    "FIRST_GUEST_IO_CYCLE",
    "LAST_GUEST_IO_CYCLE",
    "FINAL_GUEST_CYCLE",
    "FINAL_GUEST_FRAME",
    "GUEST_CYCLE_GUARD_MARGIN",
    "STATUS_POLL_PORT",
    "STATUS_POLL_RESULT",
    "FINAL_EVENT_FRAME",
    "GUEST_TERMINATION",
    "GUEST_TERMINATION_IP",
    "GUEST_IO_SEMANTIC_COUNT",
    "GUEST_IO_SERIALIZED_BYTES",
    "GUEST_IO_CRC32",
    "GUEST_IO_SHA256",
    "TIMER_PIC_SEMANTIC_COUNT",
    "TIMER_PIC_SERIALIZED_BYTES",
    "TIMER_PIC_CRC32",
    "TIMER_PIC_SHA256",
    "AUDIO_EVENTS_SEMANTIC_COUNT",
    "AUDIO_EVENTS_SERIALIZED_BYTES",
    "AUDIO_EVENTS_CRC32",
    "AUDIO_EVENTS_SHA256",
    "PCM86_DATA_RUNS_SEMANTIC_COUNT",
    "PCM86_DATA_RUNS_PAYLOAD_BYTES",
    "PCM86_DATA_RUNS_SERIALIZED_BYTES",
    "PCM86_DATA_RUNS_CRC32",
    "PCM86_DATA_RUNS_SHA256",
    "WORKER_APPLY_TRACE_SEMANTIC_COUNT",
    "WORKER_APPLY_TRACE_SERIALIZED_BYTES",
    "WORKER_APPLY_TRACE_CRC32",
    "WORKER_APPLY_TRACE_SHA256",
    "FINAL_G_STATE_SEMANTIC_COUNT",
    "FINAL_G_STATE_SERIALIZED_BYTES",
    "FINAL_G_STATE_CRC32",
    "FINAL_G_STATE_SHA256",
    "RESET_SEQUENCE",
    "RESET_OPCODE",
    "RENDER_HORIZON_FRAMES",
    "FULL_REPLAY_PCM_FRAMES",
    "FULL_REPLAY_PCM_BYTES",
    "FULL_REPLAY_PCM_CRC32",
    "FULL_REPLAY_PCM_SHA256",
    "FULL_REPLAY_PCM_PEAK",
    "FULL_REPLAY_PCM_NONZERO",
    "FULL_REPLAY_PCM_FIRST_NONZERO",
    "FULL_REPLAY_PCM_LAST_NONZERO",
    "FULL_REPLAY_PCM_CLAMP",
    "FINAL_Q240_NONZERO_FRAMES",
    "FM_COVERAGE",
    "PSG_COVERAGE",
    "RHYTHM_COVERAGE",
    "PCM86_SEMANTIC_WAVEFORM",
)

MARKERS = (
    "ORIGINAL_FULL_REPLAY_PCM_UNCHANGED",
    "SUSTAINED_FIXTURE_REAL_I286_EXECUTION",
    "SUSTAINED_FIXTURE_REAL_BOARD86_IO",
    "SUSTAINED_STATUS_POLL_SIDE_EFFECT_AUDIT",
    "SUSTAINED_LOOP_COUNT_SOURCE_GROUNDED",
    "SUSTAINED_GUEST_CYCLE_GUARD_UNCHANGED",
    "SUSTAINED_GUEST_ACTIVITY_DISTRIBUTED",
    "SUSTAINED_FINAL_Q240_AUDIO_ACTIVITY",
    "SUSTAINED_RENDER_QUANTUM_EQUIVALENCE",
    "AUDIO86_GUEST_SYNC_RESULT",
)


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    accepted = set(FIELD_NAMES) | set(MARKERS)
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in accepted:
            if key in values:
                raise RuntimeError(f"duplicate sustained oracle field: {key}")
            values[key] = value
    missing = accepted - values.keys()
    if missing:
        raise RuntimeError(f"missing sustained oracle fields: {sorted(missing)}")
    return values


def load_golden(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise RuntimeError("unsupported sustained golden schema")
    if document.get("workload_id") != "FULL_REPLAY_PCM_SUSTAINED_2S_V1":
        raise RuntimeError("unexpected sustained workload identity")
    values = document.get("values")
    if not isinstance(values, dict) or set(values) != set(FIELD_NAMES):
        raise RuntimeError("sustained golden field set mismatch")
    return {name: str(values[name]) for name in FIELD_NAMES}


def assert_markers(values: dict[str, str]) -> None:
    for marker in MARKERS:
        expected = (
            "120_240_PASS"
            if marker == "SUSTAINED_RENDER_QUANTUM_EQUIVALENCE"
            else "PASS"
        )
        if values[marker] != expected:
            raise RuntimeError(f"{marker} mismatch: {values[marker]!r}")


def assert_expected(values: dict[str, str], expected: dict[str, str]) -> None:
    for key, expected_value in expected.items():
        if values[key] != expected_value:
            raise RuntimeError(
                f"{key} mismatch: got {values[key]!r}, expected {expected_value!r}"
            )


def run_fresh(binary: Path, repetitions: int) -> list[dict[str, str]]:
    snapshots: list[dict[str, str]] = []
    for _ in range(repetitions):
        completed = subprocess.run(
            [str(binary)], check=True, capture_output=True, text=True
        )
        values = parse(completed.stdout)
        assert_markers(values)
        snapshots.append(values)
    if any(snapshot != snapshots[0] for snapshot in snapshots[1:]):
        raise RuntimeError("fresh-process sustained identities are not identical")
    return snapshots


def change_sensitivity_self_test(values: dict[str, str]) -> None:
    try:
        parse("WORKLOAD_ID=x\nWORKLOAD_ID=y\n")
    except RuntimeError as error:
        if "duplicate" not in str(error):
            raise
    else:
        raise RuntimeError("duplicate-field check is not sensitive")
    try:
        parse("WORKLOAD_ID=x\n")
    except RuntimeError as error:
        if "missing" not in str(error):
            raise
    else:
        raise RuntimeError("missing-field check is not sensitive")
    mutated = dict(values)
    mutated["FULL_REPLAY_PCM_SHA256"] = "0" * 64
    try:
        assert_expected(values, mutated)
    except RuntimeError as error:
        if "FULL_REPLAY_PCM_SHA256 mismatch" not in str(error):
            raise
    else:
        raise RuntimeError("identity mutation was not detected")


def candidate_document(values: dict[str, str], repetitions: int) -> str:
    fields = {name: values[name] for name in FIELD_NAMES}
    document = {
        "schema_version": 1,
        "workload_id": "FULL_REPLAY_PCM_SUSTAINED_2S_V1",
        "generation": {
            "fresh_processes": repetitions,
            "command": "python3 host/tests/run_np2audio86_guest_sustained_2s.py --binary host/build/phase2/tests/np2audio86_guest_sustained_2s --print-candidate --repetitions 3",
        },
        "values": fields,
    }
    return json.dumps(document, indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--golden", type=Path)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--print-candidate", action="store_true")
    args = parser.parse_args()
    if args.repetitions < 3:
        raise RuntimeError("sustained oracle requires at least three fresh processes")
    if not args.print_candidate and args.golden is None:
        parser.error("--golden is required unless --print-candidate is used")

    snapshots = run_fresh(args.binary, args.repetitions)
    values = snapshots[0]
    change_sensitivity_self_test(values)
    if args.print_candidate:
        print(candidate_document(values, args.repetitions), end="")
        return 0

    golden = load_golden(args.golden)
    assert_expected(values, golden)
    print(f"SUSTAINED_HOST_FRESH_PROCESS_DETERMINISM={args.repetitions}/{args.repetitions}_PASS")
    print("SUSTAINED_GOLDEN_CHANGE_SENSITIVITY=PASS")
    print("SUSTAINED_GOLDEN_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
