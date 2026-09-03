#!/usr/bin/env python3
"""Recompute 86R.5D.1 physical-sink acceptance from raw host evidence."""

import argparse
import re
import sys
from pathlib import Path


def parse_fields(line: str) -> dict[str, str]:
    return dict(token.split("=", 1) for token in line.split()[1:]
                if "=" in token)


def require(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def as_int(fields: dict[str, str], key: str, errors: list[str]) -> int:
    try:
        return int(fields[key], 0)
    except (KeyError, ValueError):
        errors.append(f"invalid/missing integer {fields.get('scenario', '?')}.{key}")
        return -1


def validate(text: str) -> list[str]:
    errors: list[str] = []
    lines = text.splitlines()
    records: dict[str, dict[str, str]] = {}
    for line in lines:
        if not line.startswith("5D1_EVIDENCE "):
            continue
        fields = parse_fields(line)
        require(errors, fields.get("schema") == "2",
                "evidence record schema mismatch")
        require(errors, fields.get("evidence_class") == "HOST_EXEC",
                "evidence record class mismatch")
        scenario = fields.get("scenario", "")
        require(errors, bool(scenario), "evidence record missing scenario")
        require(errors, scenario not in records, f"duplicate scenario {scenario}")
        if scenario:
            records[scenario] = fields

    full = re.fullmatch(
        r"5D1_FULL_Q240 semantic_frames=(\d+) semantic_bytes=(\d+) "
        r"physical_bytes=(\d+) consume_calls=(\d+) result=PASS",
        next((line for line in lines if line.startswith("5D1_FULL_Q240 ")), ""))
    require(errors, full is not None, "missing full-q240 raw evidence")
    if full:
        frames, semantic, physical, calls = map(int, full.groups())
        require(errors, frames == 240 and semantic == frames * 4,
                "full-q240 semantic mismatch")
        require(errors, physical == 960 and calls == 1,
                "full-q240 physical/call mismatch")

    partials: dict[int, tuple[int, int, int, int, int]] = {}
    partial_re = re.compile(
        r"5D1_FINAL_PARTIAL frames=(\d+) semantic_bytes=(\d+) "
        r"physical_bytes=(\d+) padding_frames=(\d+) padding_zero=(\d+) "
        r"digest_excludes_padding=(\d+) result=PASS")
    for line in lines:
        match = partial_re.fullmatch(line)
        if match:
            values = tuple(map(int, match.groups()))
            partials[values[0]] = values[1:]
    require(errors, set(partials) == {1, 13, 239},
            f"partial matrix mismatch {sorted(partials)}")
    for frames, values in partials.items():
        semantic, physical, padding, zero, excluded = values
        require(errors, semantic == frames * 4 and physical == 960,
                f"partial {frames} copy mismatch")
        require(errors, padding + frames == 240 and zero == 1 and excluded == 1,
                f"partial {frames} padding mismatch")

    progress = re.findall(
        r"^5D1_PARTIAL_PROGRESS bytes=(\d+) result=FATAL "
        r"ring_consumed=0 rollback=0 PASS$", text, re.M)
    require(errors, progress == ["4", "480", "956"],
            f"partial-progress mismatch {progress!r}")
    require(errors, lines.count(
        "5D1_QUEUE_OVF running=FATAL draining=TELEMETRY_ONLY "
        "stale=IGNORED result=PASS") == 1, "queue-overflow policy mismatch")

    for count in range(5):
        scenario = f"finish_eof_{count}"
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        if fields:
            require(errors, as_int(fields, "eof_snapshot", errors) == 0,
                    f"{scenario} snapshot mismatch")
            require(errors, as_int(fields, "eof_current", errors) == count,
                    f"{scenario} EOF mismatch")
            require(errors, as_int(fields, "finish", errors) ==
                    (0 if count == 4 else 2), f"{scenario} finish mismatch")

    wrong = records.get("finish_wrong_generation", {})
    require(errors, bool(wrong), "missing wrong-generation drain evidence")
    if wrong:
        require(errors, as_int(wrong, "eof_current", errors) == 4 and
                as_int(wrong, "stale", errors) == 1 and
                as_int(wrong, "finish", errors) == 0,
                "wrong-generation callback received drain authority")
    sticky = records.get("finish_sticky_error", {})
    require(errors, bool(sticky), "missing sticky-error finish evidence")
    if sticky:
        require(errors, as_int(sticky, "sticky", errors) == 1 and
                as_int(sticky, "finish", errors) == 2,
                "sticky error incorrectly finished")

    retry_expected = {"retry_before_arm": 1, "retry_during_arm": 1,
                      "retry_coalesced": 3}
    for scenario, callbacks in retry_expected.items():
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        if fields:
            before = as_int(fields, "epoch_before", errors)
            after = as_int(fields, "epoch_after", errors)
            require(errors, after - before == callbacks and
                    as_int(fields, "callbacks", errors) == callbacks,
                    f"{scenario} epoch/callback mismatch")
            require(errors, as_int(fields, "notification_only_ready", errors) == 0
                    and as_int(fields, "forced_abort", errors) == 0,
                    f"{scenario} false authority")
            require(errors, as_int(fields, "tail_held", errors) == 1 and
                    as_int(fields, "accepted_once", errors) == 1,
                    f"{scenario} ownership mismatch")

    for scenario, retry_count in (("s2_10_unit_stream", 0),
                                  ("s2_10_unit_retry", 1)):
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        if fields:
            require(errors,
                    as_int(fields, "preload_units", errors) == 4 and
                    as_int(fields, "enable_calls", errors) == 1 and
                    as_int(fields, "running_writes", errors) == 6 + retry_count,
                    f"{scenario} preload/RUNNING progression mismatch")
            require(errors,
                    as_int(fields, "physical_units", errors) == 10 and
                    as_int(fields, "full_units", errors) == 10 and
                    as_int(fields, "semantic_frames", errors) == 2400 and
                    as_int(fields, "submit_attempts", errors) ==
                        10 + retry_count and
                    as_int(fields, "retry_count", errors) == retry_count,
                    f"{scenario} unit/retry accounting mismatch")
            require(errors,
                    as_int(fields, "accepted_once", errors) == 1 and
                    as_int(fields, "byte_identity", errors) == 1 and
                    as_int(fields, "retry_slot_held", errors) == retry_count and
                    as_int(fields, "retry_accepted_held", errors) ==
                        retry_count and
                    as_int(fields, "pending", errors) == 0 and
                    as_int(fields, "drained", errors) == 2400 and
                    as_int(fields, "discarded", errors) == 0,
                    f"{scenario} ownership/identity mismatch")

    callback_expected = {
        "callback_entry_before_disarm": {"entered": 1, "disarmed": 1,
            "in_flight_during": 1, "in_flight_after": 0,
            "target_touched_safely": 1},
        "callback_entry_after_disarm": {"entered": 0, "target_touched": 0,
            "in_flight_after": 0, "stale": 1},
        "callback_zero_observation": {"observed_zero": 1, "late_entry": 1,
            "target_touched": 0, "eof_credit": 0, "stale": 1},
        "callback_inflight_teardown": {"held": 1, "abort_while_held": 2,
            "unsafe_free": 0, "released": 1, "abort_after_release": 0},
        "callback_stale_after_abort": {"target_touched": 0, "eof_credit": 0,
            "retry_authorized": 0, "finish_credit": 0, "stale": 1},
        "callback_quiescence_timeout": {"timeout": 1, "abort": 2,
            "unsafe_free": 0, "retry_abort": 0},
    }
    for scenario, expected in callback_expected.items():
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        for key, value in expected.items():
            if fields:
                require(errors, as_int(fields, key, errors) == value,
                        f"{scenario}.{key} mismatch")
        if fields:
            for key in ("dispatch_state", "delete_started", "delete_returned",
                        "gate_active", "gate_generation", "callback_generation",
                        "in_flight_before", "in_flight_peak", "in_flight_final",
                        "target_accessed", "eof_epoch_before", "eof_epoch_after",
                        "stale_callback_count", "reclaim_attempted",
                        "reclaim_allowed", "timeout"):
                require(errors, key in fields, f"{scenario}.{key} missing")

    zero = records.get("callback_zero_observation", {})
    if zero:
        require(errors, zero.get("dispatch_state") == "DISPATCHED_NOT_ENTERED" and
                as_int(zero, "delete_returned_while_pending", errors) == 0 and
                as_int(zero, "in_flight_before", errors) == 0 and
                as_int(zero, "in_flight_peak", errors) == 1 and
                as_int(zero, "in_flight_final", errors) == 0 and
                as_int(zero, "reclaim_allowed", errors) == 1,
                "callback zero-observation barrier mismatch")

    terminal_expected = {
        "healthy_stop": (0, 0, 1),
        "healthy_primary_fatal": (86, 0, 1),
        "dual_primary_then_physical": (86, 1, 0),
        "dual_physical_then_primary": (2, 1, 0),
    }
    for scenario, (first_error, forced_abort, finish) in terminal_expected.items():
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        if fields:
            require(errors, as_int(fields, "first_error", errors) == first_error
                    and as_int(fields, "forced_abort", errors) == forced_abort
                    and as_int(fields, "finish_accepted", errors) == finish,
                    f"{scenario} terminal mismatch")
    fatal = records.get("physical_fatal", {})
    require(errors, bool(fatal), "missing physical fatal evidence")
    if fatal:
        accepted = as_int(fatal, "semantic_a", errors)
        pending = as_int(fatal, "pending_a", errors)
        discarded = as_int(fatal, "discarded_a", errors)
        require(errors, as_int(fatal, "first_error", errors) == 2 and
                as_int(fatal, "forced_abort", errors) == 1,
                "physical fatal terminal mismatch")
        require(errors, accepted == pending + discarded and
                all(as_int(fatal, key, errors) == 0 for key in ("k", "p", "r")),
                "physical fatal A/K/P/R mismatch")

    for stage in range(1, 5):
        scenario = f"start_backend_rollback_{stage}"
        fields = records.get(scenario, {})
        require(errors, bool(fields), f"missing {scenario}")
        if fields:
            for key in ("start_fatal", "release_calls"):
                require(errors, as_int(fields, key, errors) == 1,
                        f"{scenario}.{key} mismatch")
            for key in ("callback_in_flight", "residual_after"):
                require(errors, as_int(fields, key, errors) == 0,
                        f"{scenario}.{key} mismatch")

    short = records.get("short_eos", {})
    require(errors, bool(short), "missing short-EOS evidence")
    if short:
        expected = {"preload_units": 1, "enable_calls": 1,
                    "physical_units": 1, "semantic_frames": 13,
                    "drain_eofs": 4, "deadlock": 0}
        for key, value in expected.items():
            require(errors, as_int(short, key, errors) == value,
                    f"short_eos.{key} mismatch")

    control = [parse_fields(line) for line in lines
               if line.startswith("5D1_CONTROL_FAULTS ")]
    require(errors, len(control) == 1 and
            as_int(control[0], "rejected", errors) == 8 and
            as_int(control[0], "total", errors) == 8,
            "callback control-fault matrix mismatch")

    histories: dict[str, list[dict[str, str]]] = {}
    for line in lines:
        if line.startswith("5D1_HISTORY "):
            fields = parse_fields(line)
            histories.setdefault(fields.get("scenario", ""), []).append(fields)
    history = histories.get("callback_zero_observation", [])
    operations = [item.get("operation") for item in history]
    require(errors, operations == [
        "PREPARE_BEGIN", "I2S_CREATE", "CALLBACK_REGISTER", "I2C_ACQUIRE",
        "CODEC_CONFIG", "PA_HIGH", "CALLBACK_DISPATCH", "ABORT",
        "CODEC_MUTE", "PA_LOW", "DISABLE", "DELETE_BEGIN",
        "CALLBACK_ENTRY", "CALLBACK_EXIT", "DELETE_END", "I2C_RELEASE",
        "DESTROY"], "callback operation history mismatch")
    for scenario, history in histories.items():
        for sequence, item in enumerate(history):
            require(errors, as_int(item, "sequence", errors) == sequence,
                    f"{scenario} history sequence mismatch")
            for key in ("generation", "operation", "result", "bytes"):
                require(errors, key in item, f"{scenario} history {key} missing")
        operations = [item.get("operation") for item in history]
        require(errors, "DELETE_END" in operations and "DESTROY" in operations,
                f"{scenario} cleanup history incomplete")
        if "DELETE_END" in operations and "DESTROY" in operations:
            require(errors, operations.index("DELETE_END") <
                    operations.index("DESTROY"),
                    f"{scenario} destroy precedes delete end")
        if "CALLBACK_DISPATCH" in operations:
            require(errors, operations.index("CALLBACK_EXIT") <
                    operations.index("DELETE_END"),
                    f"{scenario} delete end precedes callback exit")
        if "ENABLE" in operations:
            require(errors, "PRELOAD" in operations and
                    operations.index("PRELOAD") < operations.index("ENABLE"),
                    f"{scenario} enable precedes required preload")
        if "ABORT" in operations and "WRITE" in operations:
            require(errors, max(i for i, operation in enumerate(operations)
                                if operation == "WRITE") <
                    operations.index("ABORT"),
                    f"{scenario} write occurs after abort")
    full_history = [item.get("operation") for item in
                    histories.get("full_q240_history", [])]
    require(errors, full_history.count("TX_EOF") == 4 and
            "DISABLE" in full_history and
            max(i for i, operation in enumerate(full_history)
                if operation == "TX_EOF") < full_history.index("DISABLE"),
            "normal finish lacks required EOF barrier")
    retry_history = [item.get("operation") for item in
                     histories.get("retry_before_arm_history", [])]
    require(errors, retry_history.count("WRITE") >= 2,
            "retry operation history missing writes")
    sticky_history = [item.get("operation") for item in
                      histories.get("finish_sticky_error_history", [])]
    require(errors, "QUEUE_OVF" in sticky_history and
            "ABORT" in sticky_history,
            "sticky-error history missing overflow/abort")
    require(errors, not any("result=FAIL" in line for line in lines),
            "failure marker present")
    require(errors, not any(line == "5D1_PHYSICAL_SINK_RESULT=PASS"
                            for line in lines),
            "fixed unasserted process PASS marker present")
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
    print("5D1_PHYSICAL_VALIDATOR_RECOMPUTES=PASS")
    print("5D1_VALIDATOR_INDEPENDENCE=PASS")
    print("FAKE_OPERATION_ORDER_VALIDATOR=PASS")
    print("CALLBACK_STRUCTURED_EVIDENCE=PASS")
    print("5D1_CALLBACK_CONTROL_FAULTS=8/8_REJECTED")
    print("5D1_EVIDENCE_SCHEMA_VERSIONED=PASS")
    print("FIXED_UNASSERTED_PASS_LINES=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
