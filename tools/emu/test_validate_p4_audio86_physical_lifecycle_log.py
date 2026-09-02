#!/usr/bin/env python3
"""Record-level mutation tests for esp-emu start-failure evidence."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_physical_lifecycle_log.py"
MUTATIONS = (
    ("ready_wait=1", "ready_wait=0"),
    ("forced_abort=1", "forced_abort=0"),
    ("first_error=2 terminal_ack=1", "first_error=86 terminal_ack=1"),
    ("terminal_ack=1", "terminal_ack=0"),
    ("consumer_quiescent=1", "consumer_quiescent=0"),
    ("terminal_wait=1", "terminal_wait=0"),
    ("owner_suspended=1", "owner_suspended=0"),
    ("delete_performed=1", "delete_performed=0"),
    ("sink_destroy_performed=1", "sink_destroy_performed=0"),
    ("callback_residual=0", "callback_residual=1"),
    ("resource_residual=0", "resource_residual=1"),
    ("pa_high=0", "pa_high=1"),
    ("i2c_residual=0", "i2c_residual=1"),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=int, choices=range(1, 5), required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    canonical = args.log.read_text(encoding="utf-8", errors="replace")
    evidence_lines = [
        line for line in canonical.splitlines()
        if line.startswith("5D1_EVIDENCE ")
        and f"scenario=start_fatal_{args.stage} " in line
    ]
    if len(evidence_lines) != 1:
        raise SystemExit(
            f"expected one start_fatal_{args.stage} evidence record, "
            f"found {len(evidence_lines)}")
    evidence = evidence_lines[0]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "mutated.log"
        for index, (old, new) in enumerate(MUTATIONS, 1):
            mutated_evidence = evidence.replace(old, new, 1)
            if mutated_evidence == evidence:
                raise SystemExit(f"mutation {index} did not apply")
            mutated = canonical.replace(evidence, mutated_evidence, 1)
            path.write_text(mutated, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(VALIDATOR), "--stage", str(args.stage),
                 "--log", str(path)], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
            if result.returncode == 0:
                raise SystemExit(f"mutation {index} accepted")
    print(f"START_RECORD_MUTATIONS={len(MUTATIONS)}_ALL_REJECTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
