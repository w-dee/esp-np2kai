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
    "AUDIO86_GUEST_REAL_IO_PATH",
    "AUDIO86_GUEST_TIMER_PIC",
    "AUDIO86_GUEST_PCM86_ACCOUNTING",
    "AUDIO86_GUEST_TIMESTAMPING",
    "AUDIO86_GUEST_EVENT_ORACLE",
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
    traces = document.get("traces", {})
    result: dict[str, str] = {}
    for name in TRACE_NAMES:
        item = traces.get(name)
        if not isinstance(item, dict):
            raise RuntimeError(f"golden missing {name}")
        result[f"{name}_COUNT"] = str(item["count"])
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
        for marker in FIXTURE_MARKERS:
            if values.get(marker) != "PASS":
                raise RuntimeError(f"runtime fixture did not report {marker}=PASS")
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
