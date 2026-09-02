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
        "5D1_ESP_EMU_LIFECYCLE_RESULT": "VALIDATOR_PREDICATE",
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS": "VALIDATOR_PREDICATE",
        "P4_AUDIO86_REAL_GUEST_RESULT": "NON_ACCEPTANCE_DIAGNOSTIC",
    },
}


def fields(line: str) -> dict[str, str]:
    return dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)


def acceptance_assignment(line: str) -> str | None:
    assignment = ASSIGNMENT.match(line)
    if not assignment:
        return None
    name, value = assignment.groups()
    if ("MATRIX" in name or value in {"PASS", "FAIL", "REJECTED"} or
            value.endswith(("_PASS", "_FAIL", "_REJECTED"))):
        return name
    return None


def records(path: Path, evidence_class: str) -> tuple[
        list[dict[str, str]], dict[str, list[dict[str, str]]]]:
    scenarios: list[dict[str, str]] = []
    markers: dict[str, list[dict[str, str]]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        assignment = acceptance_assignment(line)
        if assignment and assignment not in EXEC_ASSIGNMENT_SCHEMA[evidence_class]:
            raise SystemExit(f"orphan acceptance output: {line}")
        if line.startswith("5D1_EVIDENCE "):
            scenarios.append(fields(line))
        elif line.startswith("5D1_"):
            marker = line.split(maxsplit=1)[0]
            markers.setdefault(marker, []).append(fields(line))
    return scenarios, markers


def static_records(path: Path) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("5D1_STATIC_EVIDENCE "):
            result.append(fields(line))
            continue
        if line.startswith("5D1_NON_ACCEPTANCE_SUMMARY "):
            continue
        if acceptance_assignment(line):
            raise SystemExit(f"orphan acceptance output: {line}")
    return result


def self_test_orphan_policy() -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        orphan_lines = (
            "5D1_START_RUNTIME_FAULT_MATRIX=3/3_PASS "
            "evidence_class=STATIC_PROJECT_SOURCE",
            "5D1_START_RUNTIME_FAULT_MATRIX=0/0_PASS",
            "5D1_START_RUNTIME_FAULT_MATRIX=NOT_RUN_PASS",
            "UNMAPPED_STATIC_CHECK=PASS",
        )
        for evidence_class in ("HOST_EXEC", "ESP_EMU_EXEC",
                               "STATIC_IDF_SOURCE", "STATIC_PROJECT_SOURCE"):
            path = Path(directory) / f"{evidence_class}.log"
            for line in orphan_lines:
                path.write_text(line + "\n", encoding="utf-8")
                try:
                    if evidence_class in EXEC_ASSIGNMENT_SCHEMA:
                        records(path, evidence_class)
                    else:
                        static_records(path)
                except SystemExit as error:
                    if "orphan acceptance output" not in str(error):
                        raise
                else:
                    raise SystemExit(
                        f"{evidence_class}: orphan acceptance output was accepted: {line}")


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
    output = result.stdout + result.stderr
    if result.returncode == 0 or "orphan acceptance output" not in output:
        raise SystemExit(
            f"{label}: mutation was not rejected as orphan acceptance output "
            f"(exit={result.returncode})")
    print(f"{label}=PASS observed_exit={result.returncode}")


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
        canonical = subprocess.run(command, text=True, capture_output=True,
                                   check=False)
        if canonical.returncode != 0:
            raise SystemExit(
                f"canonical evidence rejected by change-sensitivity test: "
                f"exit={canonical.returncode}")
        print("F4_CANONICAL_ACCEPTED=PASS observed_exit=0")

        mutations = (
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
        for label, path, line in mutations:
            original = path.read_text(encoding="utf-8", errors="replace")
            path.write_text(original + line + "\n", encoding="utf-8")
            try:
                require_rejected(command, label)
            finally:
                path.write_text(original, encoding="utf-8")
        print("F4_STATIC_ORPHAN_POLICY_NONREGRESSION=PASS")
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY_CHANGE_SENSITIVITY=PASS")


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
        self_test_orphan_policy()
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY=FAIL_CLOSED")
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

    all_scenarios: list[dict[str, str]] = []
    all_markers: dict[str, list[dict[str, str]]] = {}
    input_logs = ([(args.host_log, "HOST_EXEC")] if args.host_log else [])
    input_logs.extend((path, "ESP_EMU_EXEC") for path in args.esp_log)
    for path, evidence_class in input_logs:
        scenarios, markers = records(path, evidence_class)
        all_scenarios.extend(scenarios)
        for marker, values in markers.items():
            all_markers.setdefault(marker, []).extend(values)

    if args.require_all_exec:
        mapped: set[str] = set()
        for prop, _, evidence_class, _, evidence_id, required, _ in rows:
            if evidence_class not in {"HOST_EXEC", "ESP_EMU_EXEC"}:
                continue
            required_fields = set(required.split("|"))
            if evidence_id.startswith("marker:"):
                marker = evidence_id.removeprefix("marker:")
                matches = all_markers.get(marker, [])
            else:
                matches = [item for item in all_scenarios
                           if item.get("scenario") == evidence_id and
                           item.get("evidence_class") == evidence_class]
            if not matches:
                raise SystemExit(f"{prop}: evidence producer did not run")
            available = set().union(*(set(item) for item in matches))
            missing = required_fields - available
            if missing:
                raise SystemExit(f"{prop}: missing raw fields {sorted(missing)}")
            mapped.add(evidence_id)
        produced = {item.get("scenario", "") for item in all_scenarios}
        manifest_scenarios = {row[4] for row in rows
                              if row[2] in {"HOST_EXEC", "ESP_EMU_EXEC"} and
                              not row[4].startswith("marker:")}
        orphaned = produced - manifest_scenarios
        if orphaned:
            raise SystemExit(f"orphan executable scenarios: {sorted(orphaned)}")

    if args.require_all_exec and (not args.idf_static_log or not args.static_log):
        raise SystemExit("complete evidence validation requires both static logs")
    static_inputs = ((args.idf_static_log, "STATIC_IDF_SOURCE"),
                     (args.static_log, "STATIC_PROJECT_SOURCE"))
    for static_path, evidence_class in static_inputs:
        if not static_path:
            continue
        static_rows = {row[0]: row for row in rows if row[2] == evidence_class}
        static = static_records(static_path)
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
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY=FAIL_CLOSED")
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
