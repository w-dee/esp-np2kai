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
        "STATIC_STORAGE_REUSE_SAFE=PASS generation=29",
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
    harmless = complete + "I audit: panic handling prose is harmless\n"
    validate(harmless)
    print("ESP_EMU_VALIDATOR_POSITIVE name=harmless_panic_prose result=PASS")
    expect_failure("guru", complete + "Guru Meditation Error\n")
    expect_failure("panic_ed", complete + "Core 0 panic'ed (Load access fault)\n")
    expect_failure("bare_panic", complete + "  PaNiC : synthetic fatal\n")
    expect_failure("assert", complete + "assert failed: synthetic\n")
    expect_failure("abort", complete + "abort() was called\n")
    expect_failure("stack_overflow", complete + "Stack overflow\n")
    expect_failure("stack_smashing", complete + "Stack smashing\n")
    expect_failure("task_wdt", complete + "Task watchdog got triggered\n")
    expect_failure("interrupt_wdt", complete + "Interrupt wdt timeout\n")
    expect_failure("esp_error_check", complete + "ESP_ERROR_CHECK failed\n")
    expect_failure("unhandled_exception",
                   complete + "Unhandled fatal exception\n")
    expect_failure("missing_marker",
                   complete.replace("P4_WAKE_MATRIX=PASS\n", ""))
    expect_failure("truncated_boot", "ESP-ROM:esp32p4-eco5-20250430\n")
    expect_failure("scenario_plus_panic",
                   complete + "Guru Meditation Error after scenario PASS\n")
    print("VALIDATOR_MATRIX=16/16_PASS")
    print("BARE_PANIC_REJECTED=PASS")
    print("HARMLESS_PANIC_PROSE_ACCEPTED=PASS")
    print("ESP_EMU_VALIDATOR_FAIL_CLOSED=PASS")


if __name__ == "__main__":
    main()
