#!/usr/bin/env python3
"""Run the 86R.2 guest oracle in fresh processes and check its frozen digest."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


TRACE_NAMES = (
    "GUEST_IO",
    "AUDIO_EVENTS",
    "PCM86_BYTES",
    "PCM86_DATA_RUNS",
    "TIMER_PIC",
    "FINAL_G_STATE",
)

FIXTURE_MARKERS = (
    "ACTUAL_PIC_AUTHORITY",
    "SHARED_IRQ_SEMANTICS",
    "A46C_GUEST_COUNTER_SEMANTICS",
    "PCM86_NEVENT_SEMANTICS",
    "A466_SEMANTICS",
    "A468_SEMANTICS",
    "A46A_SEMANTICS",
    "A46A_INVALID_NEVENT_PRESERVATION",
    "A46A_GUEST_TIME_SYNC",
    "A46A_VALID_REQIRQ_RESCHEDULE",
    "DOMAIN_G_CONSUMER_INDEPENDENCE",
    "AUDIO86_GUEST_A46C_LOGICAL_FULL_WAIT",
    "AUDIO86_GUEST_PCM86_SETNEXTINTR_SELECTION",
    "AUDIO86_GUEST_PCM86_NEVENT_IMMEDIATE",
    "AUDIO86_GUEST_PCM86_NEVENT_FALLBACK",
    "AUDIO86_GUEST_TIMER_B_ACTUAL_PIC",
    "A466_END_TO_END",
    "A468_IRQ_CLEAR_BRANCH",
    "A468_FORCED_IRQ_BRANCH",
    "A468_RESCUE",
    "A468_NEVENT_CONTROL",
    "A46A_RESCUE",
    "A46A_INVALID_FORMAT_PRESERVATION",
    "A46A_REQIRQ_RESCHEDULE",
    "AUDIO86_GUEST_REAL_IO_PATH",
    "AUDIO86_GUEST_TIMER_PIC",
    "AUDIO86_GUEST_PCM86_ACCOUNTING",
    "AUDIO86_GUEST_TIMESTAMPING",
    "AUDIO86_GUEST_EVENT_ORACLE",
    "AUDIO86_GUEST_BOUNDARY_TESTS",
    "AUDIO86_GUEST_ACTUAL_PIC",
    "AUDIO86_GUEST_PCM86_NEVENT",
)


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in values:
            raise RuntimeError(f"duplicate oracle marker: {key}")
        values[key] = value
    return values


def expected(golden: Path) -> dict[str, str]:
    document = json.loads(golden.read_text(encoding="utf-8"))
    if document.get("schema_version") != 2:
        raise RuntimeError("unsupported audio86 golden schema")
    traces = document.get("traces", {})
    result: dict[str, str] = {}
    for name in TRACE_NAMES:
        item = traces.get(name)
        if not isinstance(item, dict):
            raise RuntimeError(f"golden missing {name}")
        if name == "PCM86_BYTES":
            result[f"{name}_PAYLOAD_BYTES"] = str(item["payload_bytes"])
            result[f"{name}_SERIALIZED_BYTES"] = str(item["serialized_bytes"])
        elif name == "PCM86_DATA_RUNS":
            result[f"{name}_SEMANTIC_COUNT"] = str(item["semantic_count"])
            result[f"{name}_PAYLOAD_BYTES"] = str(item["payload_bytes"])
            result[f"{name}_SERIALIZED_BYTES"] = str(item["serialized_bytes"])
        else:
            result[f"{name}_SEMANTIC_COUNT"] = str(item["semantic_count"])
            result[f"{name}_SERIALIZED_BYTES"] = str(item["serialized_bytes"])
        result[f"{name}_COUNT"] = str(item.get("count", item["serialized_bytes"]))
        result[f"{name}_CRC32"] = str(item["crc32"])
        result[f"{name}_SHA256"] = str(item["sha256"])
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
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
        if values.get("AUDIO86_GUEST_RUNTIME_RESULT") != "PASS":
            raise RuntimeError("runtime fixture did not report PASS")
        if values.get("INVALID_A46A_AUDIO_EVENT_EMITTED") != "NO":
            raise RuntimeError(
                "runtime fixture reported an invalid A46A audio event"
            )
        for marker in FIXTURE_MARKERS:
            if values.get(marker) != "PASS":
                raise RuntimeError(f"runtime fixture did not report {marker}=PASS")
        if values.get("AUDIO86_GUEST_PCM86_UNDERFLOW_HISTORY_BOUNDARY") != "ZERO_HISTORY_MVP":
            raise RuntimeError("runtime fixture did not report the zero-history boundary")
        snapshots.append(values)
    if any(snapshot != snapshots[0] for snapshot in snapshots[1:]):
        raise RuntimeError("fresh-process oracle output is not byte-identical")
    print(f"AUDIO86_GUEST_RUNTIME_REPETITIONS=PASS count={args.repetitions}")
    print("AUDIO86_GUEST_RUNTIME_GOLDEN=PASS")
    print("AUDIO86_GUEST_DETERMINISM=PASS")
    print("AUDIO86_GUEST_RUNTIME_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
