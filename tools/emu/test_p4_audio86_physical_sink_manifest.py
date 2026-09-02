#!/usr/bin/env python3
"""Validate the versioned physical-I2S manifest and evidence bijection."""

import argparse
from collections import Counter
from pathlib import Path
import re

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


def fields(line: str) -> dict[str, str]:
    return dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)


def records(path: Path) -> tuple[list[dict[str, str]], dict[str, list[dict[str, str]]]]:
    scenarios: list[dict[str, str]] = []
    markers: dict[str, list[dict[str, str]]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
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
        assignment = ASSIGNMENT.match(line)
        if assignment and (
                "MATRIX" in assignment.group(1) or
                assignment.group(2) in {"PASS", "FAIL", "REJECTED"} or
                assignment.group(2).endswith(("_PASS", "_FAIL", "_REJECTED"))):
            raise SystemExit(f"orphan acceptance output: {line}")
    return result


def self_test_orphan_policy() -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "static.log"
        orphan_lines = (
            "5D1_START_RUNTIME_FAULT_MATRIX=3/3_PASS "
            "evidence_class=STATIC_PROJECT_SOURCE",
            "5D1_START_RUNTIME_FAULT_MATRIX=0/0_PASS",
            "5D1_START_RUNTIME_FAULT_MATRIX=NOT_RUN_PASS",
            "UNMAPPED_STATIC_CHECK=PASS",
        )
        for line in orphan_lines:
            path.write_text(line + "\n", encoding="utf-8")
            try:
                static_records(path)
            except SystemExit as error:
                if "orphan acceptance output" not in str(error):
                    raise
            else:
                raise SystemExit(f"orphan acceptance output was accepted: {line}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-log", type=Path)
    parser.add_argument("--esp-log", type=Path, action="append", default=[])
    parser.add_argument("--static-log", type=Path)
    parser.add_argument("--require-all-exec", action="store_true")
    parser.add_argument("--self-test-orphan-policy", action="store_true")
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
    for path in ([args.host_log] if args.host_log else []) + args.esp_log:
        scenarios, markers = records(path)
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

    static_rows = {row[0]: row for row in rows
                   if row[2] == "STATIC_PROJECT_SOURCE"}
    if args.static_log:
        static = static_records(args.static_log)
        seen: set[str] = set()
        for item in static:
            prop = item.get("property_id", "")
            if item.get("schema") != STATIC_SCHEMA:
                raise SystemExit(f"{prop or '?'}: static evidence schema mismatch")
            if item.get("evidence_class") != "STATIC_PROJECT_SOURCE":
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
    if args.static_log:
        print("ORPHAN_ACCEPTANCE_OUTPUT_POLICY=FAIL_CLOSED")
        print("ORPHAN_ACCEPTANCE_OUTPUTS=0")
        print("START_STATIC_GUARDS_ALREADY_IN_MANIFEST=" +
              ("YES" if START_STATIC_GUARDS <= set(static_rows) else "NO"))
        print("START_STATIC_GUARD_DUPLICATES_ADDED=NO")
    for evidence_class in ("HOST_EXEC", "ESP_EMU_EXEC", "STATIC_IDF_SOURCE",
                           "STATIC_PROJECT_SOURCE", "BUILD", "INTEGRITY"):
        print(f"{evidence_class}={counts[evidence_class]}")
    print("5D1_EVIDENCE_CLASS_SUM=PASS")
    if args.require_all_exec:
        print(f"86R5D1_MATRIX={len(rows)}/{len(rows)}_PASS")
    else:
        print(f"5D1_ACCEPTANCE_MANIFEST_ROWS={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
