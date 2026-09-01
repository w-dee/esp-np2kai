#!/usr/bin/env python3
"""Mutation gates for the fail-closed real-guest log validator."""

from __future__ import annotations

from validate_p4_audio86_real_guest_log import (EXPECTED, validate, validate_failure,
                                                 validate_pressure)


def valid_log() -> str:
    lines = [
        "ESP-ROM:esp32p4-eco5-20250430",
        "I main_task: Calling app_main()",
        "P4_AUDIO86_REAL_GUEST profile=1 producer=p4_nano_pc98 producer_core=1 producer_priority=3 terminal_index=0 worker_core=0 worker_priority=6 producer_index=1 worker_index=0",
        "P4_AUDIO86_REAL_GUEST_RESIDUAL events=0 bytes=0 horizon=0 first_error=0 pcm_fifo=1",
        "P4_AUDIO86_REAL_GUEST_MEMORY psram_fallback=NO free_internal=1 largest_internal=1",
        "PCM86_NOT_EXERCISED_REQUIRES_SUPPLEMENTAL_EXISTING_86H_EVIDENCE",
        "REAL_P4_AUDIO_TIMING=NOT_VALIDATED",
    ]
    for name, (records, serialized, crc, sha) in EXPECTED.items():
        records_key = f"{name}_FRAMES" if name in {"PRE_RESET_PCM", "FULL_PCM"} else f"{name}_RECORDS"
        lines.extend((f"{records_key}={records}",
                      f"{name}_SERIALIZED_BYTES={serialized}",
                      f"{name}_CRC32={crc}", f"{name}_SHA256={sha}"))
    lines.extend((
        "PCM86_BYTES_PAYLOAD_BYTES=8", "PCM86_BYTES_SERIALIZED_BYTES=8",
        "PCM86_BYTES_CRC32=cbf0b66e",
        "PCM86_BYTES_SHA256=a51f32551aae346ed4948a0dba69cf406bdcfd3db57f30c2c9bf0f5d2945f2c4",
        "PCM86_DATA_RUNS_PAYLOAD_BYTES=8",
    ))
    for sequence in range(19):
        frame = 0 if sequence <= 16 else 13
        opcode, action, payload, count = 1, 1, 0, 0
        if sequence == 16:
            opcode, action, count = 3, 5, 8
        elif sequence == 18:
            opcode, action = 0x80000000, 4
        lines.append("P4_AUDIO86_ACTION sequence=%d frame=%d opcode=%d action=%d "
                     "byte_offset=0 byte_count=%d payload=%d" %
                     (sequence, frame, opcode, action, count, payload))
    lines.extend(("P4_AUDIO86_REAL_GUEST_RESULT=PASS",
                  "P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS",
                  "I main_task: Returned from app_main()"))
    return "\n".join(lines) + "\n"


def reject(name: str, text: str) -> None:
    try:
        validate(text)
    except SystemExit:
        print(f"S2_VALIDATOR_MUTATION name={name} result=REJECTED")
        return
    raise SystemExit(f"ERROR: mutation accepted: {name}")


def changed(text: str, before: str, after: str) -> str:
    assert text.count(before) == 1
    return text.replace(before, after)


def pressure_log() -> str:
    return valid_log() + "\n".join((
        "P4_AUDIO86_PRESSURE scenario=EVENT target=EVENT_SEQUENCE_0 cause=EVENT_CAPACITY_ONLY producer=p4_nano_pc98 core=1 priority=3 wait_index=1 phase=6 state=COMPLETE",
        "P4_AUDIO86_PRESSURE_WAIT ip_before=12 ip_after=12 pos_before=14 pos_after=14 snapshot_before=00000002 snapshot_after=00000002 resumes=1",
        "P4_AUDIO86_PRESSURE_LEASES events=0 bytes=0 horizon=0 reset_ack=0",
        "P4_AUDIO86_PRESSURE_RELEASE released=1 index0_isolated=1 ack_published=0",
        "P4_AUDIO86_PRESSURE_RESULT=PASS",
        "",
    ))


def reject_pressure(name: str, text: str) -> None:
    try:
        validate_pressure(text, "event")
    except SystemExit:
        print(f"PRESSURE_VALIDATOR_MUTATION name={name} result=REJECTED")
        return
    raise SystemExit(f"ERROR: pressure mutation accepted: {name}")


def failure_log() -> str:
    return "\n".join((
        "ESP-ROM:esp32p4-eco5-20250430",
        "I main_task: Calling app_main()",
        "P4_AUDIO86_FAILURE kind=FATAL wait=EVENT reason=86 producer_waiting=1 predicate_published=1 producer_wake_index=1 worker_wake_index=0 order=3 lifecycle=Failed first_error=86 later_guest_instructions=0",
        "P4_AUDIO86_FAILURE_CLEANUP worker_quiescent=1 leases_events=0 leases_bytes=0 leases_horizon=0 reset_ack=0 events=0 bytes=0 horizon=0 reset_closed=1 first_error_after_cleanup=86",
        "P4_AUDIO86_FAILURE_RESULT=PASS",
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS",
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS",
        "I main_task: Returned from app_main()",
        "",
    ))


def reject_failure(name: str, text: str) -> None:
    try:
        validate_failure(text, "fatal", "event")
    except SystemExit:
        print(f"FAILURE_VALIDATOR_MUTATION name={name} result=REJECTED")
        return
    raise SystemExit(f"ERROR: failure mutation accepted: {name}")


def byte_extend_failure_log(kind: str) -> str:
    fatal = kind == "fatal"
    return "\n".join((
        "ESP-ROM:esp32p4-eco5-20250430",
        "I main_task: Calling app_main()",
        "P4_AUDIO86_FAILURE kind=%s wait=BYTE_EXTEND reason=%s producer_waiting=1 predicate_published=1 producer_wake_index=1 worker_wake_index=0 order=3 lifecycle=%s first_error=%s later_guest_instructions=0" %
        (kind.upper(), "86" if fatal else "0", "Failed" if fatal else "Stopped",
         "86" if fatal else "0"),
        "P4_AUDIO86_FAILURE_CLEANUP worker_quiescent=1 leases_events=0 leases_bytes=0 leases_horizon=0 reset_ack=0 events=0 bytes=0 horizon=0 reset_closed=1 first_error_after_cleanup=%s" %
        ("86" if fatal else "0"),
        "P4_AUDIO86_FAILURE_RESULT=PASS",
        "P4_AUDIO86_BYTE_EXTEND_WAIT pending_run=1 run_bytes=1 first_byte=10 transport_bytes=1 descriptor_owned=1 horizon_owned=1 rejected_ordinal=2 rejected_byte=20 second_authorized=0 second_mutated=0 second_appended=0 wait_index=1",
        "P4_AUDIO86_BYTE_EXTEND_TERMINAL order=5 semantic_handler_flush=1 sink_bound_run=1 sink_bound_horizon=1 reserve_calls=0 extend_calls=0 control_rechecks=0 run_commits=1 horizon_commits=1 run_count=1 run_byte=10 run_frame=0 run_sequence=16 run_offset=0 rejected_absent=1 cleanup_after_close=1 producer_done_after_close=1 transaction_active=0 join_timeout=0",
        "P4_AUDIO86_BYTE_EXTEND_RESULT=PASS",
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS",
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS",
        "I main_task: Returned from app_main()",
        "",
    ))


def reject_byte_extend(name: str, text: str, kind: str = "fatal") -> None:
    try:
        validate_failure(text, kind, "byte-extend")
    except SystemExit:
        print(f"BYTE_EXTEND_VALIDATOR_MUTATION name={name} result=REJECTED")
        return
    raise SystemExit(f"ERROR: BYTE_EXTEND mutation accepted: {name}")


def main() -> None:
    complete = valid_log()
    validate(complete)
    mutations = {
        "guest_io_sha": changed(complete, EXPECTED["GUEST_IO"][3], "0" * 64),
        "audio_events_crc": changed(complete, "AUDIO_EVENTS_CRC32=3b57b261", "AUDIO_EVENTS_CRC32=00000000"),
        "worker_sha": changed(complete, EXPECTED["WORKER_APPLY_TRACE"][3], "0" * 64),
        "full_pcm_sha": changed(complete, EXPECTED["FULL_PCM"][3], "0" * 64),
        "producer_identity": changed(complete, "producer=p4_nano_pc98", "producer=other"),
        "producer_core": changed(complete, "producer_core=1", "producer_core=0"),
        "producer_priority": changed(complete, "producer_priority=3", "producer_priority=4"),
        "producer_index": changed(complete, "producer_index=1", "producer_index=0"),
        "terminal_index": changed(complete, "terminal_index=0", "terminal_index=1"),
        "worker_core": changed(complete, "worker_core=0", "worker_core=1"),
        "worker_priority": changed(complete, "worker_priority=6", "worker_priority=5"),
        "worker_index": changed(complete, "worker_index=0", "worker_index=1"),
        "event_residual": changed(complete, "events=0", "events=1"),
        "byte_residual": changed(complete, "bytes=0", "bytes=1"),
        "horizon": changed(complete, "horizon=0", "horizon=1"),
        "first_error": changed(complete, "first_error=0", "first_error=1"),
        "data_run_action": changed(complete, "sequence=16 frame=0 opcode=3 action=5", "sequence=16 frame=0 opcode=3 action=1"),
        "reset_action": changed(complete, "sequence=18 frame=13 opcode=2147483648 action=4", "sequence=18 frame=13 opcode=1 action=4"),
        "missing_pcm86_limit": complete.replace("PCM86_NOT_EXERCISED_REQUIRES_SUPPLEMENTAL_EXISTING_86H_EVIDENCE\n", ""),
        "malformed_sha": changed(complete, EXPECTED["GUEST_IO"][3], "f" * 63),
        "duplicate_conflict": complete + "FULL_PCM_SHA256=" + "0" * 64 + "\n",
        "raw_panic": complete + "panic: synthetic\n",
    }
    for name, mutated in mutations.items():
        reject(name, mutated)
    pressure = pressure_log()
    validate_pressure(pressure, "event")
    pressure_mutations = {
        "wrong_scenario": changed(pressure, "scenario=EVENT", "scenario=BYTE"),
        "wrong_target": changed(pressure, "target=EVENT_SEQUENCE_0", "target=BAD"),
        "wrong_cause": changed(pressure, "cause=EVENT_CAPACITY_ONLY", "cause=BAD"),
        "wrong_producer_index": changed(pressure, "wait_index=1", "wait_index=0"),
        "missing_wait": pressure.replace("P4_AUDIO86_PRESSURE_WAIT ip_before=12 ip_after=12 pos_before=14 pos_after=14 snapshot_before=00000002 snapshot_after=00000002 resumes=1\n", ""),
        "changed_ip": changed(pressure, "ip_after=12", "ip_after=13"),
        "changed_position": changed(pressure, "pos_after=14", "pos_after=15"),
        "changed_snapshot": changed(pressure, "snapshot_after=00000002", "snapshot_after=00000003"),
        "missing_release": pressure.replace("P4_AUDIO86_PRESSURE_RELEASE released=1 index0_isolated=1 ack_published=0\n", ""),
        "duplicate_resume": changed(pressure, "resumes=1", "resumes=2"),
        "leaked_event_lease": changed(pressure, "P4_AUDIO86_PRESSURE_LEASES events=0", "P4_AUDIO86_PRESSURE_LEASES events=1"),
        "leaked_byte_lease": changed(pressure, " bytes=0 horizon=0 reset_ack=0", " bytes=1 horizon=0 reset_ack=0"),
        "leaked_horizon_lease": changed(pressure, " horizon=0 reset_ack=0", " horizon=1 reset_ack=0"),
        "wrong_reset_ordering": changed(pressure, "ack_published=0", "ack_published=1"),
        "reset_ack_not_held": changed(pressure, "P4_AUDIO86_PRESSURE_RESULT=PASS", "P4_AUDIO86_PRESSURE_RESULT=FAIL"),
    }
    for name, mutated in pressure_mutations.items():
        reject_pressure(name, mutated)
    failure = failure_log()
    validate_failure(failure, "fatal", "event")
    failure_mutations = {
        "wrong_failure_kind": changed(failure, "kind=FATAL", "kind=STOP"),
        "wrong_wait_kind": changed(failure, "wait=EVENT", "wait=BYTE"),
        "injected_before_wait": changed(failure, "producer_waiting=1", "producer_waiting=0"),
        "producer_index0": changed(failure, "producer_wake_index=1", "producer_wake_index=0"),
        "missing_worker_wake": changed(failure, "worker_wake_index=0", "worker_wake_index=1"),
        "fatal_not_failed": changed(failure, "lifecycle=Failed", "lifecycle=Stopped"),
        "fatal_error_zero": changed(failure, "first_error=86", "first_error=0"),
        "fatal_error_changed": changed(failure, "first_error_after_cleanup=86", "first_error_after_cleanup=87"),
        "later_guest_execution": changed(failure, "later_guest_instructions=0", "later_guest_instructions=1"),
        "worker_not_quiescent": changed(failure, "worker_quiescent=1", "worker_quiescent=0"),
        "event_lease": changed(failure, "leases_events=0", "leases_events=1"),
        "byte_lease": changed(failure, "leases_bytes=0", "leases_bytes=1"),
        "horizon_lease": changed(failure, "leases_horizon=0", "leases_horizon=1"),
        "event_residual": changed(failure, " events=0", " events=1"),
        "byte_residual": changed(failure, " bytes=0", " bytes=1"),
        "horizon_nonempty": changed(failure, " horizon=0", " horizon=1"),
        "reset_dangling": changed(failure, "reset_closed=1", "reset_closed=0"),
        "wrong_wake_order": changed(failure, "order=3", "order=1"),
    }
    for name, mutated in failure_mutations.items():
        reject_failure(name, mutated)
    byte_extend = byte_extend_failure_log("fatal")
    validate_failure(byte_extend, "fatal", "byte-extend")
    stop_byte_extend = byte_extend_failure_log("stop")
    validate_failure(stop_byte_extend, "stop", "byte-extend")
    byte_extend_mutations = {
        "initial_byte_label": changed(byte_extend, "wait=BYTE_EXTEND", "wait=BYTE"),
        "pending_run_absent": changed(byte_extend, "pending_run=1", "pending_run=0"),
        "pending_run_empty": changed(byte_extend, "run_bytes=1", "run_bytes=0"),
        "authorized_byte_missing": changed(byte_extend, "first_byte=10", "first_byte=00"),
        "rejected_byte_present": changed(byte_extend, "rejected_absent=1", "rejected_absent=0"),
        "flush_marker_missing": byte_extend.replace(
            next(line for line in byte_extend.splitlines()
                 if line.startswith("P4_AUDIO86_BYTE_EXTEND_TERMINAL ")) + "\n", ""),
        "flush_after_unbind": changed(byte_extend, "cleanup_after_close=1", "cleanup_after_close=0"),
        "sink_unbound_at_commit": changed(byte_extend, "sink_bound_run=1", "sink_bound_run=0"),
        "wrong_run_count": changed(byte_extend, "run_count=1", "run_count=2"),
        "descriptor_missing": changed(byte_extend, "run_commits=1", "run_commits=0"),
        "horizon_missing": changed(byte_extend, "horizon_commits=1", "horizon_commits=0"),
        "new_reservation": changed(byte_extend, "reserve_calls=0", "reserve_calls=1"),
        "control_recheck": changed(byte_extend, "control_rechecks=0", "control_rechecks=1"),
        "producer_done_early": changed(byte_extend, "producer_done_after_close=1", "producer_done_after_close=0"),
        "byte_residual": changed(byte_extend, " bytes=0 horizon=0", " bytes=1 horizon=0"),
        "worker_join_timeout": changed(byte_extend, "join_timeout=0", "join_timeout=1"),
        "fatal_first_error_changed": changed(byte_extend, "first_error_after_cleanup=86", "first_error_after_cleanup=87"),
    }
    stop_first_error = changed(stop_byte_extend, "first_error=0", "first_error=1")
    for name, mutated in byte_extend_mutations.items():
        reject_byte_extend(name, mutated)
    reject_byte_extend("stop_first_error_nonzero", stop_first_error, "stop")
    print(f"S2_VALIDATOR_MUTATION_COUNT={len(mutations)}")
    print(f"S2_VALIDATOR_REJECTED_COUNT={len(mutations)}")
    print("S2_VALIDATOR_MUTATIONS=ALL_REJECTED")
    print(f"PRESSURE_VALIDATOR_MUTATION_COUNT={len(pressure_mutations)}")
    print(f"PRESSURE_VALIDATOR_REJECTED_COUNT={len(pressure_mutations)}")
    print("PRESSURE_VALIDATOR_MUTATIONS=ALL_REJECTED")
    print(f"FAILURE_VALIDATOR_MUTATION_COUNT={len(failure_mutations)}")
    print(f"FAILURE_VALIDATOR_REJECTED_COUNT={len(failure_mutations)}")
    print("FAILURE_VALIDATOR_MUTATIONS=ALL_REJECTED")
    print(f"BYTE_EXTEND_VALIDATOR_MUTATION_COUNT={len(byte_extend_mutations) + 1}")
    print(f"BYTE_EXTEND_VALIDATOR_REJECTED_COUNT={len(byte_extend_mutations) + 1}")
    print("BYTE_EXTEND_VALIDATOR_MUTATIONS=ALL_REJECTED")


if __name__ == "__main__":
    main()
