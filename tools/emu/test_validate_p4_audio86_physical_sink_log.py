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
    ("evidence", "physical_bytes=960", "physical_bytes=959"),
    ("evidence", "padding_zero=1", "padding_zero=0"),
    ("evidence", "digest_excludes_padding=1", "digest_excludes_padding=0"),
    ("evidence", "semantic_frames=240", "semantic_frames=480"),
    ("evidence", "result=FATAL ring_consumed=0", "result=RETRY ring_consumed=0"),
    ("evidence", "bytes=480 result=FATAL", "bytes=480 result=ACCEPTED"),
    ("evidence", "scenario=retry_before_arm epoch_before=0 epoch_after=1", "scenario=retry_before_arm epoch_before=0 epoch_after=0"),
    ("evidence", "notification_only_ready=0", "notification_only_ready=1"),
    ("evidence", "tail_held=1", "tail_held=0"),
    ("evidence", "accepted_once=1", "accepted_once=0"),
    ("evidence", "scenario=finish_eof_3 eof_snapshot=0 eof_current=3 finish=2", "scenario=finish_eof_3 eof_snapshot=0 eof_current=4 finish=0"),
    ("evidence", "scenario=finish_wrong_generation eof_snapshot=0 eof_current=4", "scenario=finish_wrong_generation eof_snapshot=0 eof_current=5"),
    ("evidence", "scenario=finish_sticky_error eof_snapshot=0 eof_current=0 finish=2 sticky=1", "scenario=finish_sticky_error eof_snapshot=0 eof_current=0 finish=0 sticky=1"),
    ("evidence", "scenario=healthy_stop terminal=STOP first_error=0 forced_abort=0", "scenario=healthy_stop terminal=STOP first_error=0 forced_abort=1"),
    ("evidence", "scenario=healthy_primary_fatal terminal=PRIMARY first_error=86 forced_abort=0", "scenario=healthy_primary_fatal terminal=PRIMARY first_error=86 forced_abort=1"),
    ("evidence", "scenario=physical_fatal terminal=PHYSICAL first_error=2", "scenario=physical_fatal terminal=PHYSICAL first_error=0"),
    ("evidence", "semantic_a=960 k=0", "semantic_a=0 k=960"),
    ("evidence", "discarded_a=960", "discarded_a=0"),
    ("evidence", "preload_units=1 enable_calls=1", "preload_units=1 enable_calls=0"),
    ("evidence", "running=FATAL", "running=IGNORED"),
    ("callback", "entered=1 disarmed=1", "entered=0 disarmed=1"),
    ("callback", "target_touched_safely=1", "target_touched_safely=0"),
    ("callback", "entered=0 target_touched=0", "entered=1 target_touched=0"),
    ("callback", "observed_zero=1 late_entry=1 target_touched=0", "observed_zero=1 late_entry=1 target_touched=1"),
    ("callback", "held=1 abort_while_held=2 unsafe_free=0", "held=1 abort_while_held=2 unsafe_free=1"),
    ("callback", "target_touched=0 eof_credit=0 retry_authorized=0", "target_touched=1 eof_credit=0 retry_authorized=0"),
    ("callback", "finish_credit=0", "finish_credit=1"),
    ("callback", "timeout=1 abort=2 unsafe_free=0", "timeout=1 abort=2 unsafe_free=1"),
    ("history", "scenario=callback_zero_observation sequence=11 generation=1 operation=DELETE_BEGIN", "scenario=callback_zero_observation sequence=11 generation=1 operation=DELETE_END"),
    ("history", "scenario=full_q240_history sequence=30 generation=1 operation=DESTROY", "scenario=full_q240_history sequence=30 generation=1 operation=DELETE_END"),
    ("history", "scenario=full_q240_history sequence=6 generation=1 operation=PRELOAD", "scenario=full_q240_history sequence=6 generation=1 operation=NOOP"),
    ("history", "scenario=retry_before_arm_history sequence=11 generation=1 operation=WRITE", "scenario=retry_before_arm_history sequence=11 generation=1 operation=ABORT"),
    ("history", "scenario=full_q240_history sequence=10 generation=1 operation=TX_EOF", "scenario=full_q240_history sequence=10 generation=1 operation=NOOP"),
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
        counts = {"evidence": 0, "callback": 0, "history": 0}
        for index, (category, old, new) in enumerate(MUTATIONS, 1):
            mutated = canonical.replace(old, new, 1)
            if mutated == canonical:
                print(f"mutation {index} did not apply", file=sys.stderr)
                return 1
            path.write_text(mutated, encoding="utf-8")
            if accepted(path):
                print(f"mutation {index} accepted", file=sys.stderr)
                return 1
            counts[category] += 1
    print(f"5D1_PHYSICAL_EVIDENCE_MUTATIONS={counts['evidence']}_ALL_REJECTED")
    print(f"5D1_CALLBACK_RECORD_MUTATIONS={counts['callback']}_ALL_REJECTED")
    print(f"5D1_OPERATION_HISTORY_MUTATIONS={counts['history']}_ALL_REJECTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
