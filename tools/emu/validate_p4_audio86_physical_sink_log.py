#!/usr/bin/env python3
"""Fail-closed validator for the deterministic 86R.5D.1 host evidence."""

import argparse
import re
import sys
from pathlib import Path


REQUIRED = (
    "5D1_FULL_Q240 semantic_frames=240 semantic_bytes=960 physical_bytes=960 consume_calls=1 result=PASS",
    "5D1_FINAL_PARTIAL frames=1 semantic_bytes=4 physical_bytes=960 padding_frames=239 padding_zero=1 digest_excludes_padding=1 result=PASS",
    "5D1_FINAL_PARTIAL frames=13 semantic_bytes=52 physical_bytes=960 padding_frames=227 padding_zero=1 digest_excludes_padding=1 result=PASS",
    "5D1_FINAL_PARTIAL frames=239 semantic_bytes=956 physical_bytes=960 padding_frames=1 padding_zero=1 digest_excludes_padding=1 result=PASS",
    "5D1_ZERO_PROGRESS_RETRY timeout_bytes=0 tail_held=1 accepted_once=1 forced_abort=0 result=PASS",
    "5D1_RETRY_LOST_WAKE before_arm=PASS during_arm=PASS coalesced=PASS notification_hint_only=1 result=PASS",
    "5D1_FINISH_DRAIN eof0=WAIT eof1=WAIT eof2=WAIT eof3=WAIT eof4=PASS wrong_generation=IGNORED sticky_error=FATAL result=PASS",
    "5D1_STALE_CALLBACK_AFTER_ABORT live_state_corruption=0 retry_authorized=0 false_finish=0 result=PASS",
    "5D1_QUEUE_OVF running=FATAL draining=TELEMETRY_ONLY stale=IGNORED result=PASS",
    "5D1_CALLBACK_QUIESCENCE held=WAIT released=RECLAIM timeout=FAIL_CLOSED unsafe_free=0 result=PASS",
    "5D1_PHYSICAL_FATAL first_error=PHYSICAL forced_abort=1 semantic_A=240 K=0 P=0 R=0 discarded_A=240 result=PASS",
    "5D1_DUAL_FAILURE order=PRIMARY_THEN_PHYSICAL first_error=86 forced_abort=1 result=PASS",
    "5D1_DUAL_FAILURE order=PHYSICAL_THEN_PRIMARY first_error=2 forced_abort=1 result=PASS",
    "5D1_PHYSICAL_STOP first_error=0 forced_abort=0 drained=1 abandonment=0 finish=PASS result=PASS",
    "5D1_PHYSICAL_PRIMARY_FATAL first_error=86 forced_abort=0 drained=1 abandonment=0 finish=PASS result=PASS",
    "5D1_SHORT_EOS preload_units=1 enable=1 physical_units=1 semantic_frames=13 drain_eofs=4 deadlock=0 result=PASS",
    "5D1_PHYSICAL_SINK_RESULT=PASS",
)


def validate(text: str) -> list[str]:
    lines = text.splitlines()
    errors = [f"missing exact evidence: {line}" for line in REQUIRED
              if lines.count(line) != 1]
    partials = re.findall(r"^5D1_PARTIAL_PROGRESS bytes=(\d+) result=FATAL ring_consumed=0 rollback=0 PASS$", text, re.M)
    if partials != ["4", "480", "956"]:
        errors.append(f"partial-progress matrix mismatch: {partials!r}")
    if any("result=FAIL" in line for line in lines):
        errors.append("failure marker present")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    errors = validate(args.log.read_text(encoding="utf-8"))
    if errors:
        for error in errors:
            print(f"5D1_PHYSICAL_VALIDATOR error={error}", file=sys.stderr)
        return 1
    print("5D1_PHYSICAL_VALIDATOR=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
