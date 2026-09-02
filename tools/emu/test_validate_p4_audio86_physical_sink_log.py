#!/usr/bin/env python3
"""Mutation coverage for the 86R.5D.1 evidence validator."""

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_physical_sink_log.py"
TEST = ROOT / "host/build/phase2/tests/p4_nano_audio86_physical_sink_test"
MUTATIONS = (
    ("physical_bytes=960", "physical_bytes=959"),
    ("padding_zero=1", "padding_zero=0"),
    ("digest_excludes_padding=1", "digest_excludes_padding=0"),
    ("semantic_frames=240", "semantic_frames=480"),
    ("timeout_bytes=0", "timeout_bytes=960"),
    ("result=FATAL ring_consumed=0", "result=RETRY ring_consumed=0"),
    ("result=FATAL ring_consumed=0", "result=ACCEPTED ring_consumed=0"),
    ("tail_held=1", "tail_held=0"),
    ("notification_hint_only=1", "notification_hint_only=0"),
    ("wrong_generation=IGNORED", "wrong_generation=COUNTED"),
    ("retry_authorized=0", "retry_authorized=1"),
    ("5D1_PHYSICAL_STOP first_error=0 forced_abort=0", "5D1_PHYSICAL_STOP first_error=0 forced_abort=1"),
    ("5D1_PHYSICAL_PRIMARY_FATAL first_error=86 forced_abort=0", "5D1_PHYSICAL_PRIMARY_FATAL first_error=86 forced_abort=1"),
    ("semantic_A=240 K=0", "semantic_A=0 K=240"),
    ("discarded_A=240", "discarded_A=0"),
    ("eof3=WAIT", "eof3=PASS"),
    ("sticky_error=FATAL", "sticky_error=PASS"),
    ("timeout=FAIL_CLOSED", "timeout=RECLAIM"),
    ("preload_units=1 enable=1", "preload_units=1 enable=0"),
    ("running=FATAL", "running=IGNORED"),
    ("stale=IGNORED", "stale=COUNTED"),
    ("unsafe_free=0", "unsafe_free=1"),
)


def accepted(path: Path) -> bool:
    return subprocess.run([sys.executable, str(VALIDATOR), str(path)],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def main() -> int:
    canonical = subprocess.check_output([str(TEST)], text=True)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "evidence.log"
        path.write_text(canonical, encoding="utf-8")
        if not accepted(path):
            print("canonical evidence rejected", file=sys.stderr)
            return 1
        for index, (old, new) in enumerate(MUTATIONS, 1):
            mutated = canonical.replace(old, new, 1)
            if mutated == canonical:
                print(f"mutation {index} did not apply", file=sys.stderr)
                return 1
            path.write_text(mutated, encoding="utf-8")
            if accepted(path):
                print(f"mutation {index} accepted", file=sys.stderr)
                return 1
    print(f"5D1_VALIDATOR_MUTATIONS={len(MUTATIONS)}_ALL_REJECTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
