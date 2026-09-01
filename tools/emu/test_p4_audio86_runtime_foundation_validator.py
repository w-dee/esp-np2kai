#!/usr/bin/env python3
"""Deterministic fail-closed tests for the 86R.5A esp-emu log validator."""

from __future__ import annotations

from validate_p4_audio86_runtime_foundation_log import SCENARIOS, validate


SHA = "be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991"


def valid_log() -> str:
    lines = [
        "ESP-ROM:esp32p4-eco5-20250430",
        "I main_task: Calling app_main()",
        "AUDIO86_86R5A_PROFILE scope=TEST_ISOLATED_HEADLESS "
        "production_runtime_active=NO i2s=NO es8311=NO a2=NO guest_binding=NO",
        "AUDIO86_86R5A_HEADLESS actions=4 final_sequence=4 data_bytes=16 "
        "resets=1 committed=16 rendered=16 residual_events=0 residual_bytes=0 "
        f"data_sha256={SHA} result=PASS",
    ]
    lines.extend(
        f"AUDIO86_86R5A_SCENARIO name={scenario} result=PASS"
        for scenario in sorted(SCENARIOS)
    )
    lines.extend((
        "P4_AUDIO86_AFFINITY=PASS producer_core=1 worker_core=0 "
        "worker_priority=6 worker_stack=8192",
        "AUDIO86_INTERNAL_MEMORY_ONLY=PASS",
        "TASK_NOTIFICATION_OWNERSHIP=PASS slot=0 owner=TRANSPORT_ONLY",
        "P4_STOP_WAKE_ALL=PASS",
        "P4_FATAL_WAKE_ALL=PASS",
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
        "AUDIO86_86R5A_FINAL residual_events=0 residual_bytes=0 "
        "result=PASS timing=NOT_VALIDATED",
        "AUDIO86_86R5A_RESULT=PASS",
        "P4_NANO_AUDIO86_RUNTIME_FOUNDATION_STATUS=PASS",
        "I main_task: Returned from app_main()",
    ))
    return "\n".join(lines) + "\n"


def expect_failure(name: str, text: str) -> None:
    try:
        validate(text)
    except SystemExit:
        print(f"ESP_EMU_VALIDATOR_NEGATIVE name={name} result=PASS")
        return
    raise SystemExit(f"ERROR: validator accepted negative case: {name}")


def main() -> None:
    complete = valid_log()
    validate(complete)
    print("ESP_EMU_VALIDATOR_POSITIVE name=complete result=PASS")
    expect_failure("guru", complete + "Guru Meditation Error\n")
    expect_failure("panic", complete + "Core 0 panic'ed (Load access fault)\n")
    expect_failure("missing_marker",
                   complete.replace("P4_WAKE_MATRIX=PASS\n", ""))
    expect_failure("truncated_boot", "ESP-ROM:esp32p4-eco5-20250430\n")
    expect_failure("scenario_plus_panic",
                   complete + "Guru Meditation Error after scenario PASS\n")
    print("ESP_EMU_VALIDATOR_FAIL_CLOSED=PASS")


if __name__ == "__main__":
    main()
