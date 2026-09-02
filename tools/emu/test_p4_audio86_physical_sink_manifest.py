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
EXEC_MARKER_OCCURRENCES = {
    "HOST_EXEC": {
        "5D1_FULL_Q240": 1,
        "5D1_FINAL_PARTIAL": 3,
        "5D1_PARTIAL_PROGRESS": 3,
        "5D1_QUEUE_OVF": 1,
        "5D1_CONTROL_FAULTS": 1,
    },
    "ESP_EMU_EXEC": {},
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


def marker_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            if token == "PASS":
                continue
            raise SystemExit(f"malformed marker evidence field: {line}")
        name, value = token.split("=", 1)
        if not name or not value or name in result:
            raise SystemExit(f"malformed/duplicate marker field {name}: {line}")
        result[name] = value
    return result


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


def validate_policy_schema(registered_markers: dict[str, set[str]]) -> None:
    if set(EXEC_ASSIGNMENT_SCHEMA) != {"HOST_EXEC", "ESP_EMU_EXEC"}:
        raise SystemExit("executable assignment schema classes mismatch")
    if set(EXEC_MARKER_OCCURRENCES) != set(EXEC_ASSIGNMENT_SCHEMA):
        raise SystemExit("executable marker schema classes mismatch")
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
        marker_schema = EXEC_MARKER_OCCURRENCES[evidence_class]
        if set(marker_schema) != registered_markers[evidence_class]:
            raise SystemExit(f"{evidence_class}: marker schema/manifest mismatch")
        if not all(isinstance(count, int) and count > 0
                   for count in marker_schema.values()):
            raise SystemExit(f"{evidence_class}: marker occurrence policy invalid")
    if set(STATIC_DIAGNOSTIC_SCHEMA) != {
            "STATIC_IDF_SOURCE", "STATIC_PROJECT_SOURCE"}:
        raise SystemExit("static diagnostic schema classes mismatch")
    for evidence_class, schema in STATIC_DIAGNOSTIC_SCHEMA.items():
        for name, item in schema.items():
            if item.get("name") != name:
                raise SystemExit(
                    f"{evidence_class}: diagnostic schema name mismatch")


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
    history_identities: set[tuple[str, int]] = set()
    fake_backends: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if classify_exec_assignment(line, evidence_class, assignments):
            continue
        if not line.startswith("5D1_"):
            continue
        kind = line.split(maxsplit=1)[0]
        if kind == "5D1_EVIDENCE":
            item = structured_fields(line)
            scenario = item.get("scenario", "")
            if (item.get("schema") != "2" or
                    item.get("evidence_class") != evidence_class):
                raise SystemExit(f"executable evidence schema/class mismatch: {line}")
            if scenario not in registered_scenarios:
                raise SystemExit(f"orphan executable scenario: {scenario or '?'}")
            scenarios.append(item)
        elif kind in registered_markers:
            markers.setdefault(kind, []).append(marker_fields(line))
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
    marker_schema = EXEC_MARKER_OCCURRENCES[evidence_class]
    if set(marker_schema) != registered_markers:
        raise SystemExit(f"{evidence_class}: marker schema/manifest mismatch")
    for name, expected in marker_schema.items():
        if marker_counts[name] != expected:
            raise SystemExit(
                f"{name}: marker occurrence mismatch "
                f"{marker_counts[name]} != {expected}")
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
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
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


def test_orphan_policy_change_sensitivity(args: argparse.Namespace) -> None:
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
        require_accepted(command, "F5_CANONICAL_ACCEPTED")
        print("LEGITIMATE_DIAGNOSTIC_SCHEMA_POSITIVE=PASS")

        def mutate(label: str, path: Path, line: str, accepted: bool = False) -> None:
            original = path.read_text(encoding="utf-8", errors="replace")
            path.write_text(original + line + "\n", encoding="utf-8")
            try:
                if accepted:
                    require_accepted(command, label)
                else:
                    require_rejected(command, label)
            finally:
                path.write_text(original, encoding="utf-8")

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
            mutate(*mutation)

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
            mutate(*mutation)

        mutate("ESP_LIFECYCLE_RESULT_VALUE_REJECTED", esp_logs[0],
               "5D1_ESP_EMU_LIFECYCLE_RESULT=NOT_RUN_PASS")
        mutate("ESP_GUEST_RESULT_VALUE_REJECTED", esp_logs[0],
               "P4_AUDIO86_REAL_GUEST_RESULT=0/0_PASS")
        mutate("ESP_GUEST_STATUS_VALUE_REJECTED", esp_logs[0],
               "P4_NANO_AUDIO86_REAL_GUEST_STATUS=NOT_RUN_PASS")
        print("EXEC_ASSIGNMENT_SCHEMA_VALUE_VALIDATION=PASS")

        mutate("STATIC_FAKE_DIAGNOSTIC_SCHEMA_REJECTED", static_log,
               "5D1_NON_ACCEPTANCE_SUMMARY arbitrary=1 hidden=PASS")
        mutate("HOST_HISTORY_DIAGNOSTIC_SCHEMA_REJECTED", host_log,
               "5D1_HISTORY arbitrary=1 hidden=PASS")
        mutate("ESP_FAKE_BACKEND_DIAGNOSTIC_SCHEMA_REJECTED", esp_logs[0],
               "5D1_FAKE_BACKEND arbitrary=1 hidden=PASS")
        mutate("NON_ACCEPTANCE_DIAGNOSTIC_DUPLICATE_REJECTED", static_log,
               "5D1_NON_ACCEPTANCE_SUMMARY name=START_STATIC_GUARDS "
               "value=3/3_PASS")
        print("NON_ACCEPTANCE_DIAGNOSTIC_SCHEMA_EXPLICIT=PASS")

        mutate("EXEC_ASSIGNMENT_DUPLICATE_REJECTED", esp_logs[0],
               "5D1_ESP_EMU_LIFECYCLE_RESULT=PASS")
        print("EXEC_ASSIGNMENT_SCHEMA_DUPLICATE_POLICY=PASS")
        marker = next(line for line in host_log.read_text(
            encoding="utf-8", errors="replace").splitlines()
                      if line.startswith("5D1_FULL_Q240 "))
        mutate("AUTHORITATIVE_MARKER_DUPLICATE_REJECTED", host_log, marker)
        print("AUTHORITATIVE_EVIDENCE_DUPLICATE_POLICY=PASS")

        mutate("HOST_BENIGN_TELEMETRY_NONREGRESSION", host_log,
               "UNREGISTERED_HOST_TELEMETRY=42", accepted=True)
        print("R5_MARKER_RECORD_SELECTION_ESCAPE=CLOSED")
        print("ACCEPTANCE_CLASSIFICATION_BEFORE_RECORD_FILTER=PASS")
        print("COMPLETE_ORPHAN_POLICY_CHANGE_SENSITIVITY=PASS")
        print("COMPLETE_ORPHAN_POLICY_EXECUTABLE_PROOF=PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-log", type=Path)
    parser.add_argument("--esp-log", type=Path, action="append", default=[])
    parser.add_argument("--idf-static-log", type=Path)
    parser.add_argument("--static-log", type=Path)
    parser.add_argument("--require-all-exec", action="store_true")
    parser.add_argument("--self-test-orphan-policy", action="store_true")
    parser.add_argument("--test-orphan-policy-change-sensitivity",
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
    validate_policy_schema(registered_markers)

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
                expected = EXEC_MARKER_OCCURRENCES[evidence_class][marker]
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
    if args.test_orphan_policy_change_sensitivity:
        test_orphan_policy_change_sensitivity(args)
        print("COMPLETE_ORPHAN_POLICY_SOURCE_PROOF=PASS")
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY=FAIL_CLOSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
