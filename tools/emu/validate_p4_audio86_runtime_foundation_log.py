#!/usr/bin/env python3
"""Validate the semantic/lifecycle contract of the isolated 86R.5A profile."""

import argparse
import re
from pathlib import Path


SCENARIOS = {
    "normal",
    "fatal_worker_wait",
    "fatal_producer_wait",
    "stop_reset_wait",
    "fatal_reset_wait",
    "stop_worker_wait",
    "invalid_horizon",
}
EXPECTED_SHA = "be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")

    require("AUDIO86_86R5A_PROFILE scope=TEST_ISOLATED_HEADLESS "
            "production_runtime_active=NO i2s=NO es8311=NO a2=NO "
            "guest_binding=NO" in text, "isolated scope marker missing")
    seen = set(re.findall(
        r"AUDIO86_86R5A_SCENARIO name=([^ ]+) result=PASS", text))
    require(seen == SCENARIOS, f"scenario set mismatch: {seen!r}")
    headless = re.search(
        r"AUDIO86_86R5A_HEADLESS actions=(\d+) final_sequence=(\d+) "
        r"data_bytes=(\d+) resets=(\d+) committed=(\d+) rendered=(\d+) "
        r"residual_events=(\d+) residual_bytes=(\d+) data_sha256=([0-9a-f]{64}) "
        r"result=PASS", text)
    require(headless is not None, "headless evidence missing")
    require(headless.groups() == (
        "4", "4", "16", "1", "16", "16", "0", "0", EXPECTED_SHA),
        f"headless evidence mismatch: {headless.groups()!r}")
    for marker in (
        "P4_AUDIO86_AFFINITY=PASS producer_core=1 worker_core=0 "
        "worker_priority=6 worker_stack=8192",
        "AUDIO86_INTERNAL_MEMORY_ONLY=PASS",
        "TASK_NOTIFICATION_OWNERSHIP=PASS slot=0 owner=TRANSPORT_ONLY",
        "P4_STOP_WAKE_ALL=PASS",
        "P4_FATAL_WAKE_ALL=PASS",
        "AUDIO86_86R5A_FINAL residual_events=0 residual_bytes=0 "
        "result=PASS timing=NOT_VALIDATED",
        "AUDIO86_86R5A_RESULT=PASS",
        "P4_NANO_AUDIO86_RUNTIME_FOUNDATION_STATUS=PASS",
    ):
        require(marker in text, f"missing marker: {marker}")
    print("AUDIO86_86R5A_ESP_EMU_SEMANTIC_LIFECYCLE=PASS")
    print("AUDIO86_86R5A_REAL_TIMING=NOT_VALIDATED")


if __name__ == "__main__":
    main()
