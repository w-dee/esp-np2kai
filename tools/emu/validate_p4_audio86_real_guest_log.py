#!/usr/bin/env python3
"""Fail-closed validator for the 86R.5B-S2 real-guest ESP-EMU profile."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FATAL_PATTERNS = (
    re.compile(r"Guru Meditation Error", re.I),
    re.compile(r"panic'ed", re.I),
    re.compile(r"^\s*panic\s*:", re.I | re.M),
    re.compile(r"assert failed", re.I),
    re.compile(r"abort\(\) was called", re.I),
    re.compile(r"Stack (?:overflow|smashing)", re.I),
    re.compile(r"(?:Task watchdog got triggered|Interrupt wdt timeout)", re.I),
    re.compile(r"ESP_ERROR_CHECK failed", re.I),
    re.compile(r"unhandled (?:fatal )?exception", re.I),
)

EXPECTED = {
    "GUEST_IO": ("59", "1416", "6be759ce",
                 "070c5b5aaed07e325fc79393b0f8eec6063ec58d5f576af8b83370ed64f09ffa"),
    "AUDIO_EVENTS": ("18", "432", "3b57b261",
                     "b703aca526bed52f75ffce46e1370209bd74e2f09f6bc9fc0f37b9f4406a119c"),
    "PCM86_DATA_RUNS": ("1", "32", "b5843125",
                          "e0f01b24e16944090c6297afe48183c06116004df104f6f4ffeb81832809f3b9"),
    "TIMER_PIC": ("20", "560", "540799b1",
                  "d605283cabea5fdc2d86643a7ff669bb56c6404435245a2ba8b5e93a26e30488"),
    "FINAL_G_STATE": ("1", "114", "8baa6a57",
                      "b9f134cc6cf9bb1b84faefa601ba330ca3495fbba2857b6029888e05b52eb91a"),
    "WORKER_APPLY_TRACE": ("19", "760", "764a2932",
                           "51e5e2f51b1718c7761a8ec85ec0be022b2b6754ac8cc27308ff45651a9659bb"),
    "PRE_RESET_PCM": ("13", "52", "f1b8c4c5",
                      "d51e85a3e8d63ecd763988f02521ef38e754f914ad84fc728375c8d84b8bf9a7"),
    "FULL_PCM": ("2400", "9600", "b518c3c9",
                 "176ea419f153382039e143163ff8476c5461abfddd055cd801003ef89c04a18a"),
}

PRESSURE = {
    "event": ("EVENT", "EVENT_SEQUENCE_0", "EVENT_CAPACITY_ONLY", "1", "0"),
    "byte": ("BYTE", "DATA_RUN_SEQUENCE_16", "BYTE_CAPACITY_ONLY", "0", "0"),
    "byte-extend": ("BYTE_EXTEND", "DATA_RUN_SEQUENCE_16_BYTE_2",
                    "BYTE_CAPACITY_ONLY", "0", "0"),
    "horizon": ("HORIZON", "HORIZON_EVENT_SEQUENCE_0", "HORIZON_ONLY", "0", "0"),
    "reset-ack": ("RESET_ACK", "RESET_ORDINAL_1", "POSTCOMMIT_ACK", "0", "1"),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def one(text: str, key: str) -> str:
    matches = re.findall(rf"(?m)^{re.escape(key)}=([^\r\n]+)$", text)
    require(len(matches) == 1, f"{key}: expected exactly one marker, got {len(matches)}")
    return matches[0].strip()


def line_after(text: str, prefix: str) -> str:
    matches = re.findall(rf"(?m)^{re.escape(prefix)}([^\r\n]+)$", text)
    require(len(matches) == 1, f"{prefix}: expected exactly one marker, got {len(matches)}")
    return matches[0].strip()


def validate_digest(text: str, name: str, records_key: str, expected: tuple[str, str, str, str]) -> None:
    records, serialized, crc, sha = expected
    require(one(text, records_key) == records, f"{records_key} mismatch")
    require(one(text, f"{name}_SERIALIZED_BYTES") == serialized,
            f"{name} serialized byte count mismatch")
    require(one(text, f"{name}_CRC32") == crc, f"{name} CRC mismatch")
    actual_sha = one(text, f"{name}_SHA256")
    require(re.fullmatch(r"[0-9a-f]{64}", actual_sha) is not None,
            f"{name} malformed SHA256")
    require(actual_sha == sha, f"{name} SHA256 mismatch")


def validate(text: str) -> None:
    for pattern in FATAL_PATTERNS:
        require(pattern.search(text) is None, f"raw fatal signature: {pattern.pattern}")
    require("ESP-ROM:esp32p4" in text, "boot ROM marker missing")
    require("main_task: Calling app_main()" in text, "app_main start missing")
    require("main_task: Returned from app_main()" in text, "app_main completion missing")
    profile = one(text, "P4_AUDIO86_REAL_GUEST profile")
    require(profile == "1 producer=p4_nano_pc98 producer_core=1 producer_priority=3 "
                       "terminal_index=0 worker_core=0 worker_priority=6 "
                       "producer_index=1 worker_index=0", "topology marker mismatch")
    require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS", "real guest result")
    require(one(text, "P4_NANO_AUDIO86_REAL_GUEST_STATUS") == "PASS", "main status")
    residual = line_after(text, "P4_AUDIO86_REAL_GUEST_RESIDUAL ")
    require(residual.startswith("events=0 bytes=0 horizon=0 first_error=0 "),
            "final residual predicate mismatch")
    memory = line_after(text, "P4_AUDIO86_REAL_GUEST_MEMORY ")
    require(memory.startswith("psram_fallback=NO free_internal="), "PSRAM fallback")
    require("PCM86_NOT_EXERCISED_REQUIRES_SUPPLEMENTAL_EXISTING_86H_EVIDENCE" in text,
            "PCM86 limitation missing")
    require(one(text, "REAL_P4_AUDIO_TIMING") == "NOT_VALIDATED", "timing claim mismatch")

    validate_digest(text, "GUEST_IO", "GUEST_IO_RECORDS", EXPECTED["GUEST_IO"])
    validate_digest(text, "AUDIO_EVENTS", "AUDIO_EVENTS_RECORDS", EXPECTED["AUDIO_EVENTS"])
    require(one(text, "PCM86_BYTES_PAYLOAD_BYTES") == "8", "PCM86 byte payload mismatch")
    require(one(text, "PCM86_BYTES_SERIALIZED_BYTES") == "8", "PCM86 byte size mismatch")
    require(one(text, "PCM86_BYTES_CRC32") == "cbf0b66e", "PCM86 byte CRC mismatch")
    require(one(text, "PCM86_BYTES_SHA256") ==
            "a51f32551aae346ed4948a0dba69cf406bdcfd3db57f30c2c9bf0f5d2945f2c4",
            "PCM86 byte SHA mismatch")
    validate_digest(text, "PCM86_DATA_RUNS", "PCM86_DATA_RUNS_RECORDS", EXPECTED["PCM86_DATA_RUNS"])
    require(one(text, "PCM86_DATA_RUNS_PAYLOAD_BYTES") == "8", "DATA_RUN payload mismatch")
    validate_digest(text, "TIMER_PIC", "TIMER_PIC_RECORDS", EXPECTED["TIMER_PIC"])
    validate_digest(text, "FINAL_G_STATE", "FINAL_G_STATE_RECORDS", EXPECTED["FINAL_G_STATE"])
    validate_digest(text, "WORKER_APPLY_TRACE", "WORKER_APPLY_TRACE_RECORDS", EXPECTED["WORKER_APPLY_TRACE"])
    validate_digest(text, "PRE_RESET_PCM", "PRE_RESET_PCM_FRAMES", EXPECTED["PRE_RESET_PCM"])
    validate_digest(text, "FULL_PCM", "FULL_PCM_FRAMES", EXPECTED["FULL_PCM"])

    actions = re.findall(
        r"(?m)^P4_AUDIO86_ACTION sequence=(\d+) frame=(\d+) opcode=(\d+) action=(\d+) "
        r"byte_offset=(\d+) byte_count=(\d+) payload=(\d+)$", text)
    require(len(actions) == 19, f"canonical action count: {len(actions)}")
    for sequence, action in enumerate(actions):
        require(int(action[0]) == sequence, "non-canonical action sequence")
        expected_frame = 0 if sequence <= 16 else 13
        require(int(action[1]) == expected_frame, "non-canonical action frame")
    require(actions[16][3] == "5" and actions[16][5] == "8", "DATA_RUN action mismatch")
    require(actions[17][3] == "1", "timer-clear action mismatch")
    require(actions[18][2] == str(0x80000000) and actions[18][3] == "4" and
            actions[18][6] == "0", "RESET action mismatch")


def pressure_fields(text: str, prefix: str) -> dict[str, str]:
    return dict(field.split("=", 1) for field in line_after(text, prefix).split())


def validate_byte_extend_wait(text: str) -> None:
    wait = pressure_fields(text, "P4_AUDIO86_BYTE_EXTEND_WAIT ")
    require(wait == {
        "pending_run": "1", "run_bytes": "1", "first_byte": "10",
        "transport_bytes": "1", "descriptor_owned": "1", "horizon_owned": "1",
        "rejected_ordinal": "2", "rejected_byte": "20", "second_authorized": "0",
        "second_mutated": "0", "second_appended": "0", "wait_index": "1",
    }, "BYTE_EXTEND cutpoint mismatch")


def validate_byte_extend_stale_wake(text: str) -> None:
    stale = pressure_fields(text, "P4_AUDIO86_BYTE_EXTEND_STALE_WAKE ")
    require(set(stale) == {
        "notifications", "consumed", "wake_returns", "phase_advance",
        "guest_progress", "second_authorized",
    }, "BYTE_EXTEND stale-wake fields mismatch")
    require(stale["notifications"] == "3" and int(stale["consumed"]) >= 3 and
            int(stale["wake_returns"]) >= 1 and stale["phase_advance"] == "0" and
            stale["guest_progress"] == "0" and stale["second_authorized"] == "0",
            "BYTE_EXTEND stale-wake level recheck mismatch")
    release = pressure_fields(text, "P4_AUDIO86_BYTE_EXTEND_RELEASE ")
    require(release == {
        "signalled": "1", "observed": "1", "lease": "0",
        "second_authorized": "1", "second_mutated": "1", "second_appended": "1",
    }, "BYTE_EXTEND authoritative release mismatch")


def validate_byte_extend_terminal(text: str) -> None:
    terminal = pressure_fields(text, "P4_AUDIO86_BYTE_EXTEND_TERMINAL ")
    require(terminal == {
        "order": "5", "semantic_handler_flush": "1", "sink_bound_run": "1",
        "sink_bound_horizon": "1", "reserve_calls": "0", "extend_calls": "0",
        "control_rechecks": "0", "run_commits": "1", "horizon_commits": "1",
        "run_count": "1", "run_byte": "10", "run_frame": "0",
        "run_sequence": "16", "run_offset": "0", "rejected_absent": "1",
        "cleanup_after_close": "1", "producer_done_after_close": "1",
        "transaction_active": "0", "join_timeout": "0",
    }, "BYTE_EXTEND terminal close mismatch")
    require(one(text, "P4_AUDIO86_BYTE_EXTEND_RESULT") == "PASS",
            "BYTE_EXTEND terminal result")


def validate_pressure(text: str, scenario: str) -> None:
    """Validate out-of-band pressure evidence without weakening 86R2/86R3."""
    require(scenario in PRESSURE, f"unknown pressure scenario: {scenario}")
    validate(text)
    name, target, cause, expected_index0, expected_ack = PRESSURE[scenario]
    marker = pressure_fields(text, "P4_AUDIO86_PRESSURE ")
    require(marker == {
        "scenario": name, "target": target, "cause": cause,
        "producer": "p4_nano_pc98", "core": "1", "priority": "3",
        "wait_index": "1", "phase": "6", "state": "COMPLETE",
    }, "pressure scenario/topology/state mismatch")
    wait = pressure_fields(text, "P4_AUDIO86_PRESSURE_WAIT ")
    require(set(wait) == {"ip_before", "ip_after", "pos_before", "pos_after",
                          "snapshot_before", "snapshot_after", "resumes"},
            "pressure wait fields mismatch")
    require(wait["ip_before"] == wait["ip_after"], "pressure IP changed while waiting")
    require(wait["pos_before"] == wait["pos_after"], "pressure execution position changed")
    require(wait["snapshot_before"] == wait["snapshot_after"],
            "pressure guest snapshot changed")
    require(re.fullmatch(r"[0-9a-f]{8}", wait["snapshot_before"]) is not None,
            "pressure snapshot malformed")
    require(wait["resumes"] == "1", "pressure resume count")
    leases = pressure_fields(text, "P4_AUDIO86_PRESSURE_LEASES ")
    require(leases == {"events": "0", "bytes": "0", "horizon": "0", "reset_ack": "0"},
            "pressure lease residual")
    release = pressure_fields(text, "P4_AUDIO86_PRESSURE_RELEASE ")
    require(release == {"released": "1", "index0_isolated": expected_index0,
                        "ack_published": expected_ack}, "pressure release evidence")
    require(one(text, "P4_AUDIO86_PRESSURE_RESULT") == "PASS", "pressure result")
    if scenario == "byte-extend":
        validate_byte_extend_wait(text)
        validate_byte_extend_stale_wake(text)


def validate_failure(text: str, kind: str, scenario: str, pcm_output: bool = False) -> None:
    """Validate C3's real C2-rendezvous STOP/FATAL result, fail closed."""
    require(kind in {"stop", "fatal"}, f"unknown failure kind: {kind}")
    require(scenario in PRESSURE, f"unknown failure scenario: {scenario}")
    for pattern in FATAL_PATTERNS:
        require(pattern.search(text) is None, f"uncontrolled raw fatal: {pattern.pattern}")
    require("ESP-ROM:esp32p4" in text, "boot ROM marker missing")
    require("main_task: Calling app_main()" in text, "app_main start missing")
    require("main_task: Returned from app_main()" in text, "app_main completion missing")
    expected_kind = kind.upper()
    expected_wait = PRESSURE[scenario][0]
    marker = pressure_fields(text, "P4_AUDIO86_FAILURE ")
    expected_reason = "0" if kind == "stop" else "86"
    expected_lifecycle = "Stopped" if kind == "stop" else "Failed"
    require(marker == {
        "kind": expected_kind, "wait": expected_wait, "reason": expected_reason,
        "producer_waiting": "1", "predicate_published": "1",
        "producer_wake_index": "1", "worker_wake_index": "0", "order": "3",
        "lifecycle": expected_lifecycle,
        "first_error": "0" if kind == "stop" else "86",
        "later_guest_instructions": "0",
    }, "failure marker mismatch")
    cleanup = pressure_fields(text, "P4_AUDIO86_FAILURE_CLEANUP ")
    require(cleanup == {
        "worker_quiescent": "1", "leases_events": "0", "leases_bytes": "0",
        "leases_horizon": "0", "reset_ack": "0", "events": "0", "bytes": "0",
        "horizon": "0", "reset_closed": "1",
        "first_error_after_cleanup": "0" if kind == "stop" else "86",
    }, "failure cleanup mismatch")
    require(one(text, "P4_AUDIO86_FAILURE_RESULT") == "PASS", "failure result")
    require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS", "real guest result")
    require(one(text, "P4_NANO_AUDIO86_REAL_GUEST_STATUS") == "PASS", "main status")
    if pcm_output:
        pcm = pressure_fields(text, "P4_AUDIO86_PCM_FAILURE ")
        produced_frames = pcm.get("produced_frames")
        consumed_frames = pcm.get("consumed_frames")
        require(produced_frames == consumed_frames,
                "PCM failure produced/consumed mismatch")
        expected_pcm = {
            "ring_finished": "1", "pcm_done": "1", "occupancy": "0",
            "partial": "0", "produced_frames": produced_frames,
            "consumed_frames": consumed_frames, "consumer_ack": "1",
            "consumer_quiescent": "1", "sink_finished": "1",
            "forced_abort": "0", "worker_suspended": "1",
            "consumer_suspended": "1", "worker_deleted_after_suspended": "1",
            "consumer_deleted_after_suspended": "1", "worker_join_timeout": "0",
            "consumer_join_timeout": "0", "abandoned_published": "0",
            "abandoned_partial": "0", "abandoned_rendered": "0",
        }
        require(pcm == expected_pcm, "PCM failure drain/quiescence mismatch")
    if scenario == "byte-extend":
        validate_byte_extend_wait(text)
        validate_byte_extend_terminal(text)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--pressure-scenario", choices=sorted(PRESSURE))
    parser.add_argument("--failure-kind", choices=("stop", "fatal"))
    parser.add_argument("--pcm-output", action="store_true")
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    require((args.failure_kind is None) or (args.pressure_scenario is not None),
            "failure validation requires a pressure scenario")
    if args.failure_kind:
        validate_failure(text, args.failure_kind, args.pressure_scenario,
                         args.pcm_output)
    elif args.pressure_scenario:
        validate_pressure(text, args.pressure_scenario)
    else:
        validate(text)
    print("P4_86R2_EXACT=PASS")
    print("P4_86R3_EXACT=PASS")
    print("S2_VALIDATOR=PASS")
    print("RAW_FATAL_SCAN=PASS")
    if args.pressure_scenario:
        print("PRESSURE_VALIDATOR=PASS")
    if args.failure_kind:
        print("FAILURE_VALIDATOR=PASS")


if __name__ == "__main__":
    main()
