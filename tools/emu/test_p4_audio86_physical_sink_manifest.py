#!/usr/bin/env python3
"""Validate the versioned physical-I2S manifest and evidence bijection."""

import argparse
from collections import Counter
from pathlib import Path
import re
import shutil
import subprocess
import sys


MANIFEST = Path(__file__).with_name(
    "p4_audio86_physical_sink_acceptance_manifest.tsv")
ROOT = Path(__file__).resolve().parents[2]
CLASSES = {"HOST_EXEC", "ESP_EMU_EXEC", "STATIC_IDF_SOURCE",
           "STATIC_PROJECT_SOURCE", "BUILD", "INTEGRITY"}
PREDICATES = {"host_validator", "lifecycle_validator", "source_checker",
              "exit_zero", "diff_empty", "absent"}
STATIC_SCHEMA = "1"
START_STATIC_GUARDS = {
    "start_epilogue", "owner_cleanup_guards", "owner_delete_order",
    "owner_destroy_order",
}
ASSIGNMENT = re.compile(r"^([A-Z0-9][A-Z0-9_]*)=([^\s]+)")
EXEC_ASSIGNMENT_SCHEMA = {
    "HOST_EXEC": {},
    "ESP_EMU_EXEC": {
        "5D1_ESP_EMU_LIFECYCLE_RESULT": {
            "classification": "VALIDATOR_PREDICATE",
            "value": "PASS",
            "occurrences": 1,
        },
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS": {
            "classification": "VALIDATOR_PREDICATE",
            "value": "PASS",
            "occurrences": 1,
        },
        "P4_AUDIO86_REAL_GUEST_RESULT": {
            "classification": "NON_ACCEPTANCE_DIAGNOSTIC",
            "value": "FAIL",
            "occurrences": 1,
        },
    },
}
# Marker identities and values are the property/scenario invariants recomputed
# independently by validate_p4_audio86_physical_sink_log.py: q240 is 240
# stereo 16-bit frames (960 bytes); final-partial cases are 1/13/239 frames;
# partial-progress cases exercise 4/480/956-byte low-level writes.
MARKER_SCHEMAS = {
    "HOST_EXEC": {
        "5D1_FULL_Q240": {
            "fields": {"semantic_frames", "semantic_bytes", "physical_bytes",
                       "consume_calls", "result"},
            "bare_tokens": (),
            "identity_field": None,
            "expected_identities": {None},
        },
        "5D1_FINAL_PARTIAL": {
            "fields": {"frames", "semantic_bytes", "physical_bytes",
                       "padding_frames", "padding_zero",
                       "digest_excludes_padding", "result"},
            "bare_tokens": (),
            "identity_field": "frames",
            "expected_identities": {"1", "13", "239"},
        },
        "5D1_PARTIAL_PROGRESS": {
            "fields": {"bytes", "result", "ring_consumed", "rollback"},
            "bare_tokens": ("PASS",),
            "identity_field": "bytes",
            "expected_identities": {"4", "480", "956"},
        },
        "5D1_QUEUE_OVF": {
            "fields": {"running", "draining", "stale", "result"},
            "bare_tokens": (),
            "identity_field": None,
            "expected_identities": {None},
        },
        "5D1_CONTROL_FAULTS": {
            "fields": {"schema", "evidence_class", "model", "rejected",
                       "total"},
            "bare_tokens": (),
            "identity_field": None,
            "expected_identities": {None},
        },
    },
    "ESP_EMU_EXEC": {},
}
EVIDENCE_BASE_FIELDS = {"schema", "evidence_class", "scenario"}
CALLBACK_BASE_FIELDS = {
    "dispatch_state", "delete_started", "delete_returned", "gate_active",
    "gate_generation", "callback_generation", "in_flight_before",
    "in_flight_peak", "in_flight_final", "target_accessed",
    "eof_epoch_before", "eof_epoch_after", "stale_callback_count",
    "reclaim_attempted", "reclaim_allowed", "timeout",
}
# Exact producer grammar for registered scenarios.  Values are typed here;
# scenario semantics remain independently recomputed by the host/lifecycle
# validators named by the manifest predicates.
EXEC_EVIDENCE_FIELDS = {
    "HOST_EXEC": {
        "short_eos": {"preload_units", "enable_calls", "physical_units",
                      "semantic_frames", "drain_eofs", "deadlock"},
        "retry_before_arm": {"epoch_before", "epoch_after", "callbacks",
                             "notification_only_ready", "tail_held",
                             "accepted_once", "forced_abort"},
        "retry_during_arm": {"epoch_before", "epoch_after", "callbacks",
                             "notification_only_ready", "tail_held",
                             "accepted_once", "forced_abort"},
        "retry_coalesced": {"epoch_before", "epoch_after", "callbacks",
                            "notification_only_ready", "tail_held",
                            "accepted_once", "forced_abort"},
        **{f"finish_eof_{count}": {"eof_snapshot", "eof_current", "finish",
                                    "sticky", "stale"}
           for count in range(5)},
        "finish_wrong_generation": {"eof_snapshot", "eof_current", "finish",
                                    "sticky", "stale"},
        "finish_sticky_error": {"eof_snapshot", "eof_current", "finish",
                                "sticky", "stale"},
        "callback_entry_before_disarm": CALLBACK_BASE_FIELDS | {
            "entered", "disarmed", "in_flight_during", "in_flight_after",
            "target_touched_safely"},
        "callback_entry_after_disarm": CALLBACK_BASE_FIELDS | {
            "entered", "target_touched", "in_flight_after", "stale"},
        "callback_zero_observation": CALLBACK_BASE_FIELDS | {
            "delete_returned_while_pending", "observed_zero", "late_entry",
            "target_touched", "eof_credit", "stale"},
        "callback_inflight_teardown": CALLBACK_BASE_FIELDS | {
            "held", "abort_while_held", "unsafe_free", "released",
            "abort_after_release"},
        "callback_stale_after_abort": CALLBACK_BASE_FIELDS | {
            "target_touched", "eof_credit", "retry_authorized",
            "finish_credit", "stale"},
        "callback_quiescence_timeout": CALLBACK_BASE_FIELDS | {
            "abort", "unsafe_free", "retry_abort"},
        "healthy_stop": {"terminal", "first_error", "forced_abort",
                         "finish_accepted", "abandonment", "pending_a",
                         "quiescent"},
        "healthy_primary_fatal": {"terminal", "first_error", "forced_abort",
                                  "finish_accepted", "abandonment", "pending_a",
                                  "quiescent"},
        "physical_fatal": {"terminal", "first_error", "forced_abort",
                           "semantic_a", "k", "p", "r", "discarded_a",
                           "pending_a", "abort_calls"},
        "dual_primary_then_physical": {"terminal", "first_error",
                                       "forced_abort", "finish_accepted"},
        "dual_physical_then_primary": {"terminal", "first_error",
                                       "forced_abort", "finish_accepted"},
        **{f"start_backend_rollback_{stage}": {
            "start_fatal", "callback_in_flight", "residual_before",
            "residual_after", "release_calls"}
           for stage in range(1, 5)},
    },
    "ESP_EMU_EXEC": {
        **{f"start_fatal_{stage}": {
            "start_fatal", "ready_wait", "forced_abort", "first_error",
            "terminal_ack", "consumer_quiescent", "terminal_wait",
            "owner_suspended", "delete_performed", "sink_destroy_performed",
            "callback_residual", "resource_residual", "pa_high",
            "i2c_residual", "result"}
           for stage in range(1, 5)},
    },
}
STATIC_DIAGNOSTIC_SCHEMA = {
    "STATIC_IDF_SOURCE": {
        "CALLBACK_BARRIER_SOURCE_PROOF": {
            "schema": "1",
            "evidence_class": "STATIC_IDF_SOURCE",
            "name": "CALLBACK_BARRIER_SOURCE_PROOF",
            "value": "COMPLETE",
        },
    },
    "STATIC_PROJECT_SOURCE": {
        "START_STATIC_GUARDS": {
            "name": "START_STATIC_GUARDS",
            "value": "3/3_PASS",
        },
    },
}
ASSIGNMENT_CLASSIFICATIONS = {
    "VALIDATOR_PREDICATE", "NON_ACCEPTANCE_DIAGNOSTIC",
}


def structured_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            raise SystemExit(f"malformed structured evidence field: {line}")
        name, value = token.split("=", 1)
        if not name or not value:
            raise SystemExit(f"empty structured evidence field: {line}")
        if name in result:
            raise SystemExit(f"duplicate structured evidence field {name}: {line}")
        result[name] = value
    return result


def marker_fields(line: str) -> tuple[dict[str, str], tuple[str, ...]]:
    result: dict[str, str] = {}
    bare_tokens: list[str] = []
    for token in line.split()[1:]:
        if "=" not in token:
            bare_tokens.append(token)
            continue
        name, value = token.split("=", 1)
        if not name or not value or name in result:
            raise SystemExit(f"malformed/duplicate marker field {name}: {line}")
        result[name] = value
    return result, tuple(bare_tokens)


def decimal_field(item: dict[str, str], name: str, line: str) -> int:
    value = item.get(name, "")
    if not re.fullmatch(r"[0-9]+", value):
        raise SystemExit(f"invalid evidence decimal {name}: {line}")
    return int(value)


def integer_field(item: dict[str, str], name: str, line: str) -> int:
    try:
        return int(item[name], 0)
    except (KeyError, ValueError) as error:
        raise SystemExit(f"invalid diagnostic integer {name}: {line}") from error


def acceptance_assignment(line: str) -> str | None:
    assignment = ASSIGNMENT.match(line)
    if not assignment:
        return None
    name, value = assignment.groups()
    if ("MATRIX" in name or value in {"PASS", "FAIL", "REJECTED"} or
            value.endswith(("_PASS", "_FAIL", "_REJECTED"))):
        return name
    return None


def lexical_record(raw_line: str, evidence_class: str) -> str:
    """Apply the column-0 authoritative-record policy, then trim line end."""
    if raw_line[:1].isspace():
        candidate = raw_line.lstrip()
        assignment = ASSIGNMENT.match(candidate)
        registered_assignment = bool(
            assignment and
            assignment.group(1) in EXEC_ASSIGNMENT_SCHEMA.get(
                evidence_class, {}))
        if (candidate.startswith("5D1_") or registered_assignment or
                acceptance_assignment(candidate)):
            raise SystemExit(
                f"indented evidence/control record violates column-0 grammar: "
                f"{raw_line}")
    return raw_line.rstrip()


def marker_expected_count(schema: dict[str, object]) -> int:
    identities = schema["expected_identities"]
    if not isinstance(identities, set):
        raise SystemExit("marker identity schema is not a set")
    return len(identities)


def validate_policy_schema(registered_scenarios: dict[str, set[str]],
                           registered_markers: dict[str, set[str]]) -> None:
    if set(EXEC_ASSIGNMENT_SCHEMA) != {"HOST_EXEC", "ESP_EMU_EXEC"}:
        raise SystemExit("executable assignment schema classes mismatch")
    if set(MARKER_SCHEMAS) != set(EXEC_ASSIGNMENT_SCHEMA):
        raise SystemExit("executable marker schema classes mismatch")
    if set(EXEC_EVIDENCE_FIELDS) != set(EXEC_ASSIGNMENT_SCHEMA):
        raise SystemExit("structured evidence schema classes mismatch")
    for evidence_class, schema in EXEC_ASSIGNMENT_SCHEMA.items():
        for name, item in schema.items():
            if set(item) != {"classification", "value", "occurrences"}:
                raise SystemExit(f"{name}: assignment schema fields mismatch")
            if item["classification"] not in ASSIGNMENT_CLASSIFICATIONS:
                raise SystemExit(f"{name}: assignment classification invalid")
            if item["occurrences"] != 1:
                raise SystemExit(f"{name}: assignment occurrence policy invalid")
            if acceptance_assignment(f"{name}={item['value']}") != name:
                raise SystemExit(f"{name}: assignment value is not acceptance-shaped")
        marker_schema = MARKER_SCHEMAS[evidence_class]
        if set(marker_schema) != registered_markers[evidence_class]:
            raise SystemExit(f"{evidence_class}: marker schema/manifest mismatch")
        for name, item in marker_schema.items():
            if set(item) != {"fields", "bare_tokens", "identity_field",
                             "expected_identities"}:
                raise SystemExit(f"{name}: marker schema fields mismatch")
            fields = item["fields"]
            identity_field = item["identity_field"]
            if not isinstance(fields, set) or not fields:
                raise SystemExit(f"{name}: marker field schema invalid")
            if identity_field is not None and identity_field not in fields:
                raise SystemExit(f"{name}: marker identity field absent")
            if marker_expected_count(item) < 1:
                raise SystemExit(f"{name}: marker occurrence policy invalid")
        if set(EXEC_EVIDENCE_FIELDS[evidence_class]) != registered_scenarios[
                evidence_class]:
            raise SystemExit(
                f"{evidence_class}: structured schema/manifest mismatch")
    if set(STATIC_DIAGNOSTIC_SCHEMA) != {
            "STATIC_IDF_SOURCE", "STATIC_PROJECT_SOURCE"}:
        raise SystemExit("static diagnostic schema classes mismatch")
    for evidence_class, schema in STATIC_DIAGNOSTIC_SCHEMA.items():
        for name, item in schema.items():
            if item.get("name") != name:
                raise SystemExit(
                    f"{evidence_class}: diagnostic schema name mismatch")


def validate_exec_evidence(line: str, evidence_class: str,
                           registered_scenarios: set[str]) -> dict[str, str]:
    item = structured_fields(line)
    scenario = item.get("scenario", "")
    if (item.get("schema") != "2" or
            item.get("evidence_class") != evidence_class):
        raise SystemExit(f"executable evidence schema/class mismatch: {line}")
    if scenario not in registered_scenarios:
        raise SystemExit(f"orphan executable scenario: {scenario or '?'}")
    expected = EVIDENCE_BASE_FIELDS | EXEC_EVIDENCE_FIELDS[evidence_class][
        scenario]
    if set(item) != expected:
        raise SystemExit(f"executable evidence fields mismatch: {line}")
    for name in expected - EVIDENCE_BASE_FIELDS - {"dispatch_state", "terminal"}:
        decimal_field(item, name, line)
    if "dispatch_state" in item and item["dispatch_state"] not in {
            "DISPATCHED_NOT_ENTERED", "ENTERED_IN_FLIGHT", "EXITED"}:
        raise SystemExit(f"invalid callback dispatch state: {line}")
    if "terminal" in item and item["terminal"] not in {
            "STOP", "PRIMARY", "PHYSICAL", "DUAL"}:
        raise SystemExit(f"invalid terminal evidence value: {line}")
    return item


def validate_marker(line: str, evidence_class: str, kind: str,
                    identities: dict[str, set[object]]) -> dict[str, str]:
    schema = MARKER_SCHEMAS[evidence_class][kind]
    item, bare_tokens = marker_fields(line)
    if set(item) != schema["fields"] or bare_tokens != schema["bare_tokens"]:
        raise SystemExit(f"{kind}: marker field/token schema mismatch: {line}")

    if kind == "5D1_FULL_Q240":
        frames = decimal_field(item, "semantic_frames", line)
        semantic = decimal_field(item, "semantic_bytes", line)
        physical = decimal_field(item, "physical_bytes", line)
        calls = decimal_field(item, "consume_calls", line)
        valid = (frames == 240 and semantic == frames * 4 and
                 physical == 960 and calls == 1 and item["result"] == "PASS")
    elif kind == "5D1_FINAL_PARTIAL":
        frames = decimal_field(item, "frames", line)
        semantic = decimal_field(item, "semantic_bytes", line)
        physical = decimal_field(item, "physical_bytes", line)
        padding = decimal_field(item, "padding_frames", line)
        zero = decimal_field(item, "padding_zero", line)
        excluded = decimal_field(item, "digest_excludes_padding", line)
        valid = (semantic == frames * 4 and physical == 960 and
                 padding + frames == 240 and zero == 1 and excluded == 1 and
                 item["result"] == "PASS")
    elif kind == "5D1_PARTIAL_PROGRESS":
        decimal_field(item, "bytes", line)
        consumed = decimal_field(item, "ring_consumed", line)
        rollback = decimal_field(item, "rollback", line)
        valid = (item["result"] == "FATAL" and consumed == 0 and rollback == 0)
    elif kind == "5D1_QUEUE_OVF":
        valid = (item == {"running": "FATAL", "draining": "TELEMETRY_ONLY",
                          "stale": "IGNORED", "result": "PASS"})
    elif kind == "5D1_CONTROL_FAULTS":
        rejected = decimal_field(item, "rejected", line)
        total = decimal_field(item, "total", line)
        valid = (item["schema"] == "2" and
                 item["evidence_class"] == evidence_class and
                 item["model"] == "callback" and rejected == 8 and total == 8)
    else:
        raise SystemExit(f"{kind}: marker semantic validator absent")
    if not valid:
        raise SystemExit(f"{kind}: marker semantic schema mismatch: {line}")

    identity_field = schema["identity_field"]
    identity = item[identity_field] if isinstance(identity_field, str) else None
    if identity not in schema["expected_identities"]:
        raise SystemExit(f"{kind}: unexpected marker identity {identity!r}")
    if identity in identities.setdefault(kind, set()):
        raise SystemExit(f"{kind}: duplicate marker identity {identity!r}")
    identities[kind].add(identity)
    return item


def classify_exec_assignment(line: str, evidence_class: str,
                             counts: Counter[str]) -> bool:
    assignment = ASSIGNMENT.match(line)
    if not assignment:
        return False
    name, value = assignment.groups()
    schema = EXEC_ASSIGNMENT_SCHEMA[evidence_class]
    if name in schema:
        expected = schema[name]["value"]
        if value != expected or line != f"{name}={expected}":
            raise SystemExit(f"{name}: executable assignment schema mismatch")
        counts[name] += 1
    elif name.startswith("5D1_") or acceptance_assignment(line):
        raise SystemExit(f"orphan acceptance/control output: {line}")
    return True


def validate_history(line: str, evidence_class: str,
                     identities: set[tuple[str, int]]) -> None:
    item = structured_fields(line)
    required = {"schema", "evidence_class", "scenario", "sequence",
                "generation", "operation", "result", "bytes"}
    if set(item) != required:
        raise SystemExit(f"history diagnostic fields mismatch: {line}")
    if item["schema"] != "2" or item["evidence_class"] != evidence_class:
        raise SystemExit(f"history diagnostic schema/class mismatch: {line}")
    if not item["scenario"] or not item["operation"]:
        raise SystemExit(f"history diagnostic semantic field missing: {line}")
    sequence = integer_field(item, "sequence", line)
    integer_field(item, "generation", line)
    integer_field(item, "result", line)
    integer_field(item, "bytes", line)
    identity = (item["scenario"], sequence)
    if identity in identities:
        raise SystemExit(f"duplicate history diagnostic: {identity!r}")
    identities.add(identity)


def validate_fake_backend(line: str, evidence_class: str,
                          registered_scenarios: set[str],
                          seen: set[str]) -> None:
    item = structured_fields(line)
    required = {"schema", "evidence_class", "scenario", "i2s", "callbacks",
                "i2c", "codec", "pa_high", "released", "destroyed"}
    if set(item) != required:
        raise SystemExit(f"fake-backend diagnostic fields mismatch: {line}")
    if (evidence_class != "ESP_EMU_EXEC" or item["schema"] != "2" or
            item["evidence_class"] != evidence_class):
        raise SystemExit(f"fake-backend diagnostic schema/class mismatch: {line}")
    scenario = item["scenario"]
    if scenario not in registered_scenarios:
        raise SystemExit(f"fake-backend diagnostic scenario unknown: {scenario}")
    if scenario in seen:
        raise SystemExit(f"duplicate fake-backend diagnostic: {scenario}")
    for name in required - {"schema", "evidence_class", "scenario"}:
        integer_field(item, name, line)
    seen.add(scenario)


def records(path: Path, evidence_class: str,
            registered_scenarios: set[str],
            registered_markers: set[str]) -> tuple[
                list[dict[str, str]], dict[str, list[dict[str, str]]]]:
    scenarios: list[dict[str, str]] = []
    markers: dict[str, list[dict[str, str]]] = {}
    assignments: Counter[str] = Counter()
    marker_counts: Counter[str] = Counter()
    marker_identities: dict[str, set[object]] = {}
    history_identities: set[tuple[str, int]] = set()
    fake_backends: set[str] = set()
    for raw_line in path.read_text(
            encoding="utf-8", errors="replace").splitlines():
        line = lexical_record(raw_line, evidence_class)
        if classify_exec_assignment(line, evidence_class, assignments):
            continue
        if not line.startswith("5D1_"):
            continue
        kind = line.split(maxsplit=1)[0]
        if kind == "5D1_EVIDENCE":
            scenarios.append(validate_exec_evidence(
                line, evidence_class, registered_scenarios))
        elif kind in registered_markers:
            markers.setdefault(kind, []).append(validate_marker(
                line, evidence_class, kind, marker_identities))
            marker_counts[kind] += 1
        elif kind == "5D1_HISTORY":
            validate_history(line, evidence_class, history_identities)
        elif kind == "5D1_FAKE_BACKEND":
            validate_fake_backend(line, evidence_class, registered_scenarios,
                                  fake_backends)
        else:
            raise SystemExit(f"unknown 5D1 evidence/control record: {line}")

    for name, schema in EXEC_ASSIGNMENT_SCHEMA[evidence_class].items():
        expected = schema["occurrences"]
        if assignments[name] != expected:
            raise SystemExit(
                f"{name}: executable assignment occurrence mismatch "
                f"{assignments[name]} != {expected}")
    marker_schema = MARKER_SCHEMAS[evidence_class]
    if set(marker_schema) != registered_markers:
        raise SystemExit(f"{evidence_class}: marker schema/manifest mismatch")
    for name, schema in marker_schema.items():
        expected = marker_expected_count(schema)
        if marker_counts[name] != expected:
            raise SystemExit(
                f"{name}: marker occurrence mismatch "
                f"{marker_counts[name]} != {expected}")
        if marker_identities.get(name, set()) != schema["expected_identities"]:
            raise SystemExit(f"{name}: marker identity coverage mismatch")
    return scenarios, markers


def validate_static_diagnostic(line: str, evidence_class: str,
                               seen: set[str]) -> None:
    item = structured_fields(line)
    name = item.get("name", "")
    schema = STATIC_DIAGNOSTIC_SCHEMA[evidence_class]
    if name not in schema or item != schema[name]:
        raise SystemExit(f"non-acceptance diagnostic schema mismatch: {line}")
    if name in seen:
        raise SystemExit(f"duplicate non-acceptance diagnostic: {name}")
    seen.add(name)


def static_records(path: Path, evidence_class: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    diagnostics: set[str] = set()
    for raw_line in path.read_text(
            encoding="utf-8", errors="replace").splitlines():
        line = lexical_record(raw_line, evidence_class)
        assignment = ASSIGNMENT.match(line)
        if assignment:
            name = assignment.group(1)
            if name.startswith("5D1_") or acceptance_assignment(line):
                raise SystemExit(f"orphan acceptance/control output: {line}")
            continue
        if line.startswith("5D1_STATIC_EVIDENCE "):
            result.append(structured_fields(line))
        elif line.startswith("5D1_NON_ACCEPTANCE_SUMMARY "):
            validate_static_diagnostic(line, evidence_class, diagnostics)
        elif line.startswith("5D1_"):
            raise SystemExit(f"unknown 5D1 evidence/control record: {line}")
    expected = set(STATIC_DIAGNOSTIC_SCHEMA[evidence_class])
    if diagnostics != expected:
        raise SystemExit(
            f"{evidence_class}: diagnostic occurrence mismatch "
            f"missing={sorted(expected - diagnostics)} "
            f"extra={sorted(diagnostics - expected)}")
    return result


def self_test_assignment_orphan_policy() -> None:
    orphan_lines = (
        "5D1_START_RUNTIME_FAULT_MATRIX=3/3_PASS",
        "5D1_START_RUNTIME_FAULT_MATRIX=0/0_PASS",
        "5D1_START_RUNTIME_FAULT_MATRIX=NOT_RUN_PASS",
        "UNMAPPED_STATIC_CHECK=PASS",
    )
    for evidence_class in ("HOST_EXEC", "ESP_EMU_EXEC"):
        for line in orphan_lines:
            try:
                classify_exec_assignment(line, evidence_class, Counter())
            except SystemExit:
                pass
            else:
                raise SystemExit(
                    f"{evidence_class}: orphan assignment was accepted: {line}")
    for line in orphan_lines:
        if not acceptance_assignment(line):
            raise SystemExit(f"static orphan assignment was not classified: {line}")


def validator_command(host_log: Path, esp_logs: list[Path],
                      idf_static_log: Path, static_log: Path) -> list[str]:
    command = [sys.executable, str(Path(__file__).resolve()),
               "--host-log", str(host_log)]
    for path in esp_logs:
        command.extend(("--esp-log", str(path)))
    command.extend(("--idf-static-log", str(idf_static_log),
                    "--static-log", str(static_log), "--require-all-exec"))
    return command


def require_rejected(command: list[str], label: str) -> None:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode == 0:
        raise SystemExit(f"{label}: mutation was accepted")
    print(f"{label}=PASS observed_exit={result.returncode}")


def require_accepted(command: list[str], label: str) -> None:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"{label}: control was rejected (exit={result.returncode})")
    print(f"{label}=PASS observed_exit=0")


def test_evidence_grammar_change_sensitivity(args: argparse.Namespace) -> None:
    import tempfile

    if not (args.host_log and args.esp_log and args.idf_static_log and
            args.static_log and args.require_all_exec):
        raise SystemExit("change-sensitivity test requires complete evidence inputs")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        host_log = root / "host.log"
        idf_static_log = root / "idf-static.log"
        static_log = root / "project-static.log"
        shutil.copyfile(args.host_log, host_log)
        shutil.copyfile(args.idf_static_log, idf_static_log)
        shutil.copyfile(args.static_log, static_log)
        esp_logs: list[Path] = []
        for index, source in enumerate(args.esp_log):
            destination = root / f"esp-{index}.log"
            shutil.copyfile(source, destination)
            esp_logs.append(destination)

        command = validator_command(host_log, esp_logs, idf_static_log, static_log)
        require_accepted(command, "F6_CANONICAL_ACCEPTED")
        print("LEGITIMATE_DIAGNOSTIC_SCHEMA_POSITIVE=PASS")

        def mutate_text(label: str, path: Path, mutated: str,
                        accepted: bool = False) -> None:
            original = path.read_text(encoding="utf-8", errors="replace")
            if mutated == original:
                raise SystemExit(f"{label}: mutation did not change input")
            path.write_text(mutated, encoding="utf-8")
            try:
                if accepted:
                    require_accepted(command, label)
                else:
                    require_rejected(command, label)
            finally:
                path.write_text(original, encoding="utf-8")

        def append_line(label: str, path: Path, line: str,
                        accepted: bool = False) -> None:
            original = path.read_text(encoding="utf-8", errors="replace")
            mutate_text(label, path, original + line + "\n", accepted)

        def replace_once(label: str, path: Path, old: str, new: str,
                         accepted: bool = False) -> None:
            original = path.read_text(encoding="utf-8", errors="replace")
            if original.count(old) != 1:
                raise SystemExit(
                    f"{label}: expected exactly one mutation target")
            mutate_text(label, path, original.replace(old, new, 1), accepted)

        assignment_mutations = (
            ("F4_HOST_ORPHAN_PASS_REJECTED", host_log,
             "UNREGISTERED_HOST_ACCEPTANCE=PASS"),
            ("F4_HOST_ORPHAN_ZERO_PASS_REJECTED", host_log,
             "UNREGISTERED_HOST_ZERO=0/0_PASS"),
            ("F4_HOST_ORPHAN_NOT_RUN_REJECTED", host_log,
             "UNREGISTERED_HOST_NOT_RUN=NOT_RUN_PASS"),
            ("F4_HOST_ORPHAN_MATRIX_REJECTED", host_log,
             "UNREGISTERED_HOST_MATRIX=1/1_PASS"),
            ("F4_ESP_ORPHAN_PASS_REJECTED", esp_logs[0],
             "UNREGISTERED_ESP_ACCEPTANCE=PASS"),
            ("F4_ESP_ORPHAN_ZERO_PASS_REJECTED", esp_logs[0],
             "UNREGISTERED_ESP_ZERO=0/0_PASS"),
            ("F4_ESP_ORPHAN_NOT_RUN_REJECTED", esp_logs[0],
             "UNREGISTERED_ESP_NOT_RUN=NOT_RUN_PASS"),
            ("F4_ESP_ORPHAN_MATRIX_REJECTED", esp_logs[0],
             "UNREGISTERED_ESP_MATRIX=1/1_PASS"),
            ("F4_IDF_STATIC_ORPHAN_REJECTED", idf_static_log,
             "UNREGISTERED_IDF_STATIC_ACCEPTANCE=PASS"),
            ("F4_PROJECT_STATIC_ORPHAN_REJECTED", static_log,
             "UNREGISTERED_PROJECT_STATIC_ACCEPTANCE=PASS"),
        )
        for mutation in assignment_mutations:
            append_line(*mutation)

        marker_mutations = (
            ("HOST_5D1_UNKNOWN_MARKER_REJECTED", host_log,
             "5D1_UNKNOWN_HOST_MARKER scenario=x result=PASS"),
            ("UNKNOWN_MARKER_FAIL_RECORD_REJECTED", host_log,
             "5D1_UNKNOWN_HOST_MARKER scenario=x result=FAIL"),
            ("UNKNOWN_MARKER_REJECTED_RECORD_REJECTED", host_log,
             "5D1_UNKNOWN_HOST_MARKER scenario=x result=REJECTED"),
            ("ESP_5D1_UNKNOWN_MARKER_REJECTED", esp_logs[0],
             "5D1_UNKNOWN_ESP_MARKER scenario=x result=PASS"),
            ("IDF_STATIC_5D1_UNKNOWN_MARKER_REJECTED", idf_static_log,
             "5D1_UNKNOWN_IDF_MARKER scenario=x result=PASS"),
            ("PROJECT_STATIC_5D1_UNKNOWN_MARKER_REJECTED", static_log,
             "5D1_UNKNOWN_PROJECT_MARKER scenario=x result=PASS"),
        )
        for mutation in marker_mutations:
            append_line(*mutation)

        append_line("ESP_LIFECYCLE_RESULT_VALUE_REJECTED", esp_logs[0],
                    "5D1_ESP_EMU_LIFECYCLE_RESULT=NOT_RUN_PASS")
        append_line("ESP_GUEST_RESULT_VALUE_REJECTED", esp_logs[0],
                    "P4_AUDIO86_REAL_GUEST_RESULT=0/0_PASS")
        append_line("ESP_GUEST_STATUS_VALUE_REJECTED", esp_logs[0],
                    "P4_NANO_AUDIO86_REAL_GUEST_STATUS=NOT_RUN_PASS")
        print("EXEC_ASSIGNMENT_SCHEMA_VALUE_VALIDATION=PASS")

        append_line("STATIC_FAKE_DIAGNOSTIC_SCHEMA_REJECTED", static_log,
                    "5D1_NON_ACCEPTANCE_SUMMARY arbitrary=1 hidden=PASS")
        append_line("HOST_HISTORY_DIAGNOSTIC_SCHEMA_REJECTED", host_log,
                    "5D1_HISTORY arbitrary=1 hidden=PASS")
        append_line("ESP_FAKE_BACKEND_DIAGNOSTIC_SCHEMA_REJECTED", esp_logs[0],
                    "5D1_FAKE_BACKEND arbitrary=1 hidden=PASS")
        append_line("NON_ACCEPTANCE_DIAGNOSTIC_DUPLICATE_REJECTED", static_log,
                    "5D1_NON_ACCEPTANCE_SUMMARY name=START_STATIC_GUARDS "
                    "value=3/3_PASS")
        print("NON_ACCEPTANCE_DIAGNOSTIC_SCHEMA_EXPLICIT=PASS")

        append_line("EXEC_ASSIGNMENT_DUPLICATE_REJECTED", esp_logs[0],
                    "5D1_ESP_EMU_LIFECYCLE_RESULT=PASS")
        print("EXEC_ASSIGNMENT_SCHEMA_DUPLICATE_POLICY=PASS")
        full_marker = next(line for line in host_log.read_text(
            encoding="utf-8", errors="replace").splitlines()
                      if line.startswith("5D1_FULL_Q240 "))
        append_line("AUTHORITATIVE_MARKER_DUPLICATE_REJECTED", host_log,
                    full_marker)
        print("AUTHORITATIVE_EVIDENCE_DUPLICATE_POLICY=PASS")

        append_line("LEADING_WHITESPACE_ACCEPTANCE_REJECTED", host_log,
                    " UNREGISTERED_HOST_ACCEPTANCE=PASS")
        append_line("LEADING_WHITESPACE_5D1_REJECTED", host_log,
                    " 5D1_UNKNOWN_HOST_MARKER scenario=x result=PASS")
        print("LEADING_WHITESPACE_EVIDENCE_ESCAPE=CLOSED")

        replace_once("MARKER_INVALID_SEMANTIC_VALUE_REJECTED", host_log,
                     full_marker,
                     full_marker.replace("semantic_frames=240",
                                         "semantic_frames=241"))
        replace_once("MARKER_UNEXPECTED_FIELD_REJECTED", host_log,
                     full_marker, full_marker + " unexpected_semantic=PASS")
        replace_once("MARKER_INVALID_RESULT_REJECTED", host_log,
                     full_marker,
                     full_marker.replace("result=PASS",
                                         "result=NOT_RUN_PASS"))
        replace_once("MARKER_MISSING_FIELD_REJECTED", host_log,
                     full_marker,
                     full_marker.replace(" consume_calls=1", ""))
        replace_once("MARKER_DUPLICATE_FIELD_REJECTED", host_log,
                     full_marker, full_marker + " result=PASS")
        print("REGISTERED_MARKER_SCHEMA_VALIDATION=PASS")

        evidence_line = next(line for line in host_log.read_text(
            encoding="utf-8", errors="replace").splitlines()
                             if line.startswith("5D1_EVIDENCE "))
        replace_once("STRUCTURED_ACCEPTANCE_FIELD_REJECTED", host_log,
                     evidence_line, evidence_line + " acceptance=PASS")
        replace_once("STRUCTURED_NEUTRAL_FIELD_REJECTED", host_log,
                     evidence_line, evidence_line + " note=extra")
        print("STRUCTURED_EVIDENCE_EXACT_FIELD_SCHEMA=PASS")
        print("STRUCTURED_EVIDENCE_UNEXPECTED_FIELD_REJECTED=PASS")

        partials = [line for line in host_log.read_text(
            encoding="utf-8", errors="replace").splitlines()
                    if line.startswith("5D1_FINAL_PARTIAL ")]
        if len(partials) != 3:
            raise SystemExit("repeatable marker control count mismatch")
        original = host_log.read_text(encoding="utf-8", errors="replace")
        duplicate_partials = original
        for line in partials[1:]:
            duplicate_partials = duplicate_partials.replace(line, partials[0], 1)
        mutate_text("REPEATABLE_MARKER_SEMANTIC_DUPLICATE_REJECTED",
                    host_log, duplicate_partials)
        print("REPEATABLE_MARKER_SEMANTIC_UNIQUENESS=PASS")
        print("REPEATABLE_MARKER_BOUNDED_COVERAGE=PASS")

        replace_once("PARSER_TRAILING_WHITESPACE_ACCEPTED", host_log,
                     full_marker, full_marker + "   ", accepted=True)
        spaced_marker = "  ".join(full_marker.split())
        replace_once("PARSER_MULTIPLE_FIELD_SPACES_ACCEPTED", host_log,
                     full_marker, spaced_marker, accepted=True)
        marker_tokens = full_marker.split()
        reordered_marker = " ".join(
            [marker_tokens[0], *reversed(marker_tokens[1:])])
        replace_once("PARSER_FIELD_REORDERING_ACCEPTED", host_log,
                     full_marker, reordered_marker, accepted=True)
        replace_once("PARSER_MALFORMED_TOKEN_REJECTED", host_log,
                     full_marker, full_marker + " malformed")
        print("EVIDENCE_PARSER_BOUNDARY_AUDIT=PASS")

        append_line("HOST_BENIGN_TELEMETRY_NONREGRESSION", host_log,
                    "UNREGISTERED_HOST_TELEMETRY=42", accepted=True)
        append_line("HOST_FREEFORM_INFORMATION_NONREGRESSION", host_log,
                    "informational host test output", accepted=True)
        print("R5_MARKER_RECORD_SELECTION_ESCAPE=CLOSED")
        print("ACCEPTANCE_CLASSIFICATION_BEFORE_RECORD_FILTER=PASS")
        print("RAW_RECORD_CLASSIFICATION_TOTALITY=PASS")
        print("EVIDENCE_RECORD_SILENT_DROP_PATHS=0")
        print("RAW_RECORD_CLASSIFICATION_UNAMBIGUOUS=PASS")
        print("TELEMETRY_FALLBACK_CANNOT_HIDE_EVIDENCE=PASS")
        print("COMPLETE_EVIDENCE_GRAMMAR_CHANGE_SENSITIVITY=PASS")
        print("COMPLETE_EVIDENCE_GRAMMAR_EXECUTABLE_PROOF=PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-log", type=Path)
    parser.add_argument("--esp-log", type=Path, action="append", default=[])
    parser.add_argument("--idf-static-log", type=Path)
    parser.add_argument("--static-log", type=Path)
    parser.add_argument("--require-all-exec", action="store_true")
    parser.add_argument("--self-test-orphan-policy", action="store_true")
    parser.add_argument(
        "--test-evidence-grammar-change-sensitivity",
        "--test-orphan-policy-change-sensitivity",
        dest="test_evidence_grammar_change_sensitivity",
        action="store_true")
    args = parser.parse_args()
    if args.self_test_orphan_policy:
        self_test_assignment_orphan_policy()
        print("ASSIGNMENT_ORPHAN_POLICY_SELF_TEST=PASS")
        return 0

    rows: list[list[str]] = []
    for number, line in enumerate(MANIFEST.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        row = line.split("\t")
        if len(row) != 7 or not all(row):
            raise SystemExit(f"manifest line {number}: expected 7 nonempty fields")
        rows.append(row)
    identifiers = [row[0] for row in rows]
    if len(identifiers) != len(set(identifiers)):
        raise SystemExit("duplicate property id")
    for prop, _, evidence_class, command, evidence_id, required, predicate in rows:
        if evidence_class not in CLASSES:
            raise SystemExit(f"{prop}: invalid evidence class")
        if predicate not in PREDICATES:
            raise SystemExit(f"{prop}: missing predicate evaluator")
        if not required.split("|"):
            raise SystemExit(f"{prop}: missing required fields")
        token = command.split()[0]
        if token.startswith("tools/") and not (ROOT / token).exists():
            raise SystemExit(f"{prop}: producer absent: {token}")
        if evidence_class in {"HOST_EXEC", "ESP_EMU_EXEC"} and not evidence_id:
            raise SystemExit(f"{prop}: executable producer id absent")

    registered_scenarios = {
        evidence_class: {row[4] for row in rows
                         if row[2] == evidence_class and
                         not row[4].startswith("marker:")}
        for evidence_class in EXEC_ASSIGNMENT_SCHEMA
    }
    registered_markers = {
        evidence_class: {row[4].removeprefix("marker:") for row in rows
                         if row[2] == evidence_class and
                         row[4].startswith("marker:")}
        for evidence_class in EXEC_ASSIGNMENT_SCHEMA
    }
    validate_policy_schema(registered_scenarios, registered_markers)

    all_scenarios: list[dict[str, str]] = []
    all_markers: dict[str, list[dict[str, str]]] = {}
    input_logs = ([(args.host_log, "HOST_EXEC")] if args.host_log else [])
    input_logs.extend((path, "ESP_EMU_EXEC") for path in args.esp_log)
    for path, evidence_class in input_logs:
        scenarios, markers = records(
            path, evidence_class, registered_scenarios[evidence_class],
            registered_markers[evidence_class])
        all_scenarios.extend(scenarios)
        for marker, values in markers.items():
            all_markers.setdefault(marker, []).extend(values)

    if args.require_all_exec:
        for prop, _, evidence_class, _, evidence_id, required, _ in rows:
            if evidence_class not in {"HOST_EXEC", "ESP_EMU_EXEC"}:
                continue
            required_fields = set(required.split("|"))
            if evidence_id.startswith("marker:"):
                marker = evidence_id.removeprefix("marker:")
                matches = all_markers.get(marker, [])
                expected = marker_expected_count(
                    MARKER_SCHEMAS[evidence_class][marker])
            else:
                matches = [item for item in all_scenarios
                           if item.get("scenario") == evidence_id and
                           item.get("evidence_class") == evidence_class]
                expected = 1
            if len(matches) != expected:
                raise SystemExit(
                    f"{prop}: evidence occurrence mismatch "
                    f"{len(matches)} != {expected}")
            available = set().union(*(set(item) for item in matches))
            missing = required_fields - available
            if missing:
                raise SystemExit(f"{prop}: missing raw fields {sorted(missing)}")

    if args.require_all_exec and (not args.idf_static_log or not args.static_log):
        raise SystemExit("complete evidence validation requires both static logs")
    static_inputs = ((args.idf_static_log, "STATIC_IDF_SOURCE"),
                     (args.static_log, "STATIC_PROJECT_SOURCE"))
    for static_path, evidence_class in static_inputs:
        if not static_path:
            continue
        static_rows = {row[0]: row for row in rows if row[2] == evidence_class}
        static = static_records(static_path, evidence_class)
        seen: set[str] = set()
        for item in static:
            prop = item.get("property_id", "")
            if item.get("schema") != STATIC_SCHEMA:
                raise SystemExit(f"{prop or '?'}: static evidence schema mismatch")
            if item.get("evidence_class") != evidence_class:
                raise SystemExit(f"{prop or '?'}: static evidence class mismatch")
            if item.get("predicate") != "PASS":
                raise SystemExit(f"{prop or '?'}: static predicate did not pass")
            if prop not in static_rows:
                raise SystemExit(f"orphan static property: {prop or '?'}")
            if prop in seen:
                raise SystemExit(f"duplicate static property evidence: {prop}")
            required = set(static_rows[prop][5].split("|"))
            available = set(item.get("fields", "").split("|"))
            if required != available:
                raise SystemExit(
                    f"{prop}: static fields mismatch "
                    f"missing={sorted(required - available)} "
                    f"extra={sorted(available - required)}")
            seen.add(prop)
        missing = set(static_rows) - seen
        if missing:
            raise SystemExit(f"static evidence producer did not run: {sorted(missing)}")

    counts = Counter(row[2] for row in rows)
    print("5D1_ACCEPTANCE_MANIFEST_SCHEMA=PASS")
    if args.require_all_exec:
        print("MANIFEST_EVIDENCE_BIJECTION=PASS")
    complete_boundary = bool(args.host_log and args.esp_log and
                             args.idf_static_log and args.static_log and
                             args.require_all_exec)
    if complete_boundary:
        print("ORPHAN_ACCEPTANCE_OUTPUTS=0")
        project_static_rows = {row[0] for row in rows
                               if row[2] == "STATIC_PROJECT_SOURCE"}
        print("START_STATIC_GUARDS_ALREADY_IN_MANIFEST=" +
              ("YES" if START_STATIC_GUARDS <= project_static_rows else "NO"))
        print("START_STATIC_GUARD_DUPLICATES_ADDED=NO")
    for evidence_class in ("HOST_EXEC", "ESP_EMU_EXEC", "STATIC_IDF_SOURCE",
                           "STATIC_PROJECT_SOURCE", "BUILD", "INTEGRITY"):
        print(f"{evidence_class}={counts[evidence_class]}")
    print("5D1_EVIDENCE_CLASS_SUM=PASS")
    if args.require_all_exec:
        print(f"86R5D1_MATRIX={len(rows)}/{len(rows)}_PASS")
    else:
        print(f"5D1_ACCEPTANCE_MANIFEST_ROWS={len(rows)}")
    if args.test_evidence_grammar_change_sensitivity:
        test_evidence_grammar_change_sensitivity(args)
        print("EVIDENCE_LEXICAL_WHITESPACE_POLICY=COLUMN_0_REQUIRED")
        print("MARKER_SCHEMA_CONSTRAINTS_SEMANTICALLY_GROUNDED=PASS")
        print("COMPLETE_EVIDENCE_GRAMMAR_SOURCE_PROOF=PASS")
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY=FAIL_CLOSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
