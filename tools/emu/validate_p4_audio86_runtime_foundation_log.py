#!/usr/bin/env python3
"""Validate the semantic/lifecycle contract of the isolated 86R.5A profile."""

import argparse
import re
from pathlib import Path


SCENARIOS = {
    "normal",
    "fatal_worker_wait",
    "fatal_producer_wait",
    "stop_event_wait",
    "event_wait_normal",
    "stop_reset_wait",
    "fatal_reset_wait",
    "stop_worker_wait",
    "cross_index_isolation",
    "byte_wake_before_recheck",
    "byte_spurious_then_normal",
    "byte_wait_stop",
    "byte_wait_fatal",
    "horizon_wake_before_recheck",
    "horizon_spurious_then_normal",
    "horizon_wait_stop",
    "horizon_wait_fatal",
    "transport_before_horizon",
    "worker_only_partial_create",
    "producer_only_partial_create",
    "producer_ready_timeout",
    "worker_ready_timeout",
    "completion_recheck_event_only",
    "completion_recheck_horizon_only",
    "completion_recheck_combined",
    "completion_recheck_empty",
    "completion_recheck_stop",
    "completion_recheck_producer_fatal",
    "completion_recheck_worker_fatal",
    "completion_recheck_reset",
}
EXPECTED_SHA = "be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991"
FATAL_PATTERNS = (
    re.compile(r"Guru Meditation Error", re.IGNORECASE),
    re.compile(r"panic'ed", re.IGNORECASE),
    re.compile(r"^\s*panic\s*:", re.IGNORECASE | re.MULTILINE),
    re.compile(r"assert failed", re.IGNORECASE),
    re.compile(r"abort\(\) was called", re.IGNORECASE),
    re.compile(r"Stack (?:overflow|smashing)", re.IGNORECASE),
    re.compile(r"(?:Task watchdog got triggered|Interrupt wdt timeout)",
               re.IGNORECASE),
    re.compile(r"ESP_ERROR_CHECK failed", re.IGNORECASE),
    re.compile(r"unhandled (?:fatal )?exception", re.IGNORECASE),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def validate(text: str) -> None:
    for pattern in FATAL_PATTERNS:
        require(pattern.search(text) is None,
                f"runtime-fatal pattern present: {pattern.pattern}")
    require("ESP-ROM:esp32p4" in text, "boot ROM marker missing")
    require("main_task: Calling app_main()" in text,
            "app_main start marker missing")
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
        "AUDIO86_NOTIFICATION_CONFIG=PASS notification_entries=2 "
        "producer_notification_index=1 worker_notification_index=0 "
        "static_task_size=344 worker_context=117152",
        "TASK_NOTIFICATION_OWNERSHIP=PASS producer_slot=1 "
        "owner=AUDIO_TRANSPORT worker_slot=0 owner=AUDIO_TRANSPORT",
        "AUDIO_NOTIFICATION_INDEX_COMPILE_GUARD=PASS",
        "AUDIO_NOTIFICATION_ISR_PATH=NONE",
        "CROSS_INDEX_WAKE_STEALING=NONE",
        "CROSS_INDEX_STALE_VALUE_INTERFERENCE=NONE",
        "RECREATED_TASK_AUDIO_NOTIFICATION_CLEAN=PASS",
        "P4_STOP_WAKE_ALL=PASS",
        "P4_FATAL_WAKE_ALL=PASS",
        "AUDIO_STOP_WAKE_FANOUT=PASS",
        "AUDIO_FATAL_WAKE_FANOUT=PASS",
        "BYTE_CAPACITY_WAIT_RETRY=PASS",
        "BYTE_RETIRE_WAKE=PASS",
        "BYTE_WAIT_LOST_WAKEUP_PROOF=PASS",
        "BYTE_WAIT_STOP_WAKE=PASS",
        "BYTE_WAIT_FATAL_WAKE=PASS",
        "EVENT_WAIT_STOP_WAKE=PASS",
        "EVENT_WAIT_FATAL_WAKE=PASS",
        "FREERTOS_WAIT_PROTOCOL=PASS",
        "LOST_WAKEUP_PROOF=PASS",
        "P4_WAKE_MATRIX=PASS",
        "EVENT_WAIT_INDEX1=PASS",
        "BYTE_WAIT_INDEX1=PASS",
        "HORIZON_WAIT_INDEX1=PASS",
        "RESET_ACK_WAIT_INDEX1=PASS",
        "WORKER_WAIT_INDEX0=PASS",
        "PARTIAL_CREATE_INDEXED_NOTIFICATION=PASS",
        "READY_TIMEOUT_INDEXED_NOTIFICATION=PASS",
        "TASK_REUSE_ENTRIES2=PASS",
        "TASK_QUIESCENCE_ENTRIES2=PASS",
        "HORIZON_MAILBOX_C11_PROOF=PASS",
        "HORIZON_INDEFINITE_PUBLICATION=PASS",
        "HORIZON_FULL_WAIT_RETRY=PASS",
        "HORIZON_WAIT_PROTOCOL=PASS",
        "TRANSPORT_BEFORE_HORIZON=PASS",
        "STATIC_TASK_QUIESCENCE_PROTOCOL=PASS",
        "PARTIAL_CREATE_QUIESCENCE=PASS",
        "READY_TIMEOUT_QUIESCENCE=PASS",
        "TERMINAL_PATHS_UNIFIED=PASS",
        "TERMINAL_TIMEOUT_NO_REUSE=PASS",
        "STATIC_STORAGE_REUSE_SAFE=PASS",
        "P4_POST_DONE_EVENT_RECHECK=PASS",
        "P4_POST_DONE_HORIZON_ONLY=PASS",
        "P4_POST_DONE_COMBINED_RECHECK=PASS",
        "P4_POST_DONE_EMPTY_FINISH=PASS",
        "FINAL_COMPLETION_LEVEL_PREDICATE=PASS",
        "TERMINAL_ERROR_PRECEDENCE=PASS",
        "RESET_FINALIZE_NONREGRESSION=PASS",
        "POST_DONE_HORIZON_RECHECK=PASS",
        "P4_POST_DONE_C11_PROOF=PASS",
        "SINGLE_P4_COMPLETION_RULE=PASS",
        "BYTE_POST_DONE_RECHECK_NOT_REQUIRED_BY_PROTOCOL=PASS",
        "SUCCESSFUL_FINAL_RESIDUALS_ZERO=PASS",
        "P4_POST_DONE_COMPLETION_STRESS=PASS repetitions=1000",
        "AUDIO86_86R5A_FINAL residual_events=0 residual_bytes=0 "
        "horizon_pending=0 reset_ack=1 first_error=0 "
        "result=PASS timing=NOT_VALIDATED",
        "AUDIO86_86R5A_RESULT=PASS",
        "P4_NANO_AUDIO86_RUNTIME_FOUNDATION_STATUS=PASS",
    ):
        require(marker in text, f"missing marker: {marker}")
    require("main_task: Returned from app_main()" in text,
            "app_main terminal marker missing")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    validate(text)
    print("AUDIO86_86R5A_ESP_EMU_SEMANTIC_LIFECYCLE=PASS")
    print("AUDIO86_86R5A_REAL_TIMING=NOT_VALIDATED")


if __name__ == "__main__":
    main()
