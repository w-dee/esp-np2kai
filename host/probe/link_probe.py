#!/usr/bin/env python3
"""Collect deterministic mapped relocatable link-closure evidence.

This probe compiles only the explicit source inventory, links those objects
with ``-r``, and records the undefined symbols that remain.  It is an
evidence collector, not a dependency resolver: unresolved externals are
reported and do not by themselves make a successful probe fail.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import shlex
import subprocess
from pathlib import Path
from compile_probe import (
    FORBIDDEN_SELECTORS,
    FORBIDDEN_SOURCE_PARTS,
    forbidden_source_paths,
    parse_host_inputs,
    read_source_map,
    read_source_overlay,
    read_sources,
    validate_host_sources,
    validate_logical_source,
)


SCHEMA_VERSION = 2
SOURCE_PATH_GLOB_CHARS = set("*?[]{}")
MANAGED_REPORT_NAMES = (
    "combined.o",
    "link-results.json",
    "link-results.txt",
    "object-commands.txt",
    "undefined-symbols.txt",
    "symbol-references.tsv",
)


class ProbeError(RuntimeError):
    """A structural probe failure."""


def fail(message: str) -> None:
    raise ProbeError(message)


def reject_symlink_components(path: Path, label: str) -> None:
    """Reject an output path that would traverse an unexpected symlink."""

    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        if current.is_symlink():
            fail(f"unexpected symlink in {label}: {current}")


def remove_file_artifact(path: Path, label: str) -> None:
    """Remove one managed file, never recursively removing an unexpected dir."""

    if not os.path.lexists(path):
        return
    if path.is_symlink():
        path.unlink()
        return
    if path.is_dir():
        fail(f"unexpected directory for managed {label}: {path}")
    path.unlink()


def clean_managed_outputs(output_dir: Path) -> None:
    """Clear only artifacts owned by this probe from one output directory."""

    reject_symlink_components(output_dir, "link-probe output")
    if os.path.lexists(output_dir):
        if output_dir.is_symlink():
            fail(f"link-probe output directory may not be a symlink: {output_dir}")
        if not output_dir.is_dir():
            fail(f"link-probe output is not a directory: {output_dir}")
    else:
        output_dir.mkdir(parents=True, exist_ok=True)

    objects = output_dir / "objects"
    if os.path.lexists(objects):
        if objects.is_symlink():
            objects.unlink()
        elif objects.is_dir():
            shutil.rmtree(objects)
        else:
            fail(f"unexpected non-directory for managed objects: {objects}")
    for name in MANAGED_REPORT_NAMES:
        remove_file_artifact(output_dir / name, name)


def stable_path(path: Path, roots: list[tuple[Path, str]]) -> str:
    candidate = path.resolve(strict=False)
    for root, label in roots:
        try:
            relative = candidate.relative_to(root)
        except ValueError:
            continue
        suffix = relative.as_posix()
        return label if not suffix or suffix == "." else f"{label}/{suffix}"
    return candidate.as_posix()


def normalize_text(value: str, roots: list[tuple[Path, str]]) -> str:
    normalized = value
    for root, label in sorted(roots, key=lambda item: len(str(item[0])), reverse=True):
        normalized = normalized.replace(str(root), label)
    return normalized.replace("\\", "/")


def parse_nm_undefined(output: str) -> list[str]:
    symbols: set[str] = set()
    for raw in output.splitlines():
        fields = raw.split()
        if not fields:
            continue
        symbol = fields[-1]
        if symbol in {"U", "u"}:
            continue
        symbols.add(symbol)
    return sorted(symbols)


def make_common_flags(args: argparse.Namespace) -> list[str]:
    flags: list[str] = []
    for include in args.include:
        flags.append(f"-I{Path(include).resolve()}")
    for definition in args.define:
        flags.append(f"-D{definition}")
    for definition in args.string_define:
        name, separator, value = definition.partition("=")
        if not separator or not name:
            fail(f"--string-define requires NAME=VALUE: {definition}")
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        flags.append(f'-D{name}="{escaped}"')
    flags.extend(args.cflag)
    return flags


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_reports(
    output_dir: Path,
    summary: dict,
    object_commands: list[tuple[str, str]],
    undefined_symbols: list[str],
    symbol_references: dict[str, list[str]],
    failure_text: str = "",
) -> None:
    write_text(
        output_dir / "object-commands.txt",
        "".join(f"{logical}\t{command}\n" for logical, command in object_commands),
    )
    write_text(
        output_dir / "undefined-symbols.txt",
        "".join(f"{symbol}\n" for symbol in undefined_symbols),
    )
    tsv_lines = ["symbol\tlogical_sources\n"]
    for symbol in sorted(symbol_references):
        tsv_lines.append(f"{symbol}\t{','.join(symbol_references[symbol])}\n")
    write_text(output_dir / "symbol-references.tsv", "".join(tsv_lines))
    text_lines = [
        "Mapped relocatable link-closure probe",
        f"vendored_sources={summary['vendored_source_count']}",
        f"host_sources={summary['host_source_count']}",
        f"total_sources={summary['total_source_count']}",
        f"sources={summary['source_count']} (total)",
        "object_compile="
        f"{summary['object_compile_passed_count']}/"
        f"{summary['total_source_count']} PASS, "
        f"{summary['object_compile_failed_count']} FAIL",
        f"relocatable_link_attempted={summary['relocatable_link_attempted']}",
        f"relocatable_link_returncode={summary['relocatable_link_returncode']}",
        f"undefined_count={summary['undefined_symbol_count']}",
        "",
        "Undefined symbols:",
    ]
    text_lines.extend(undefined_symbols)
    if failure_text:
        text_lines.extend(["", "Failure:", failure_text])
    write_text(output_dir / "link-results.txt", "\n".join(text_lines) + "\n")
    write_text(
        output_dir / "link-results.json",
        json.dumps(summary, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-list", type=Path, required=True)
    parser.add_argument("--host-source-root", type=Path)
    parser.add_argument("--host-source-list", type=Path)
    parser.add_argument("--source-map", type=Path, required=True)
    parser.add_argument("--source-overlay", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--string-define", action="append", default=[])
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()

    output_argument = args.output_dir.absolute()
    clean_managed_outputs(output_argument)

    source_root = args.source_root.resolve()
    source_list = args.source_list.resolve()
    source_map = args.source_map.resolve()
    output_dir = output_argument.resolve()
    if not source_root.is_dir():
        fail(f"source root is not a directory: {source_root}")
    if not source_list.is_file():
        fail(f"source list is not a file: {source_list}")
    if args.source_map.is_symlink():
        fail(f"source map may not be a symlink: {args.source_map}")
    if not source_map.is_file():
        fail(f"source map is not a file: {source_map}")

    host_source_root, host_source_list = parse_host_inputs(args)
    vendor_sources, vendor_stages = read_sources(source_list)
    host_sources: list[str] = []
    host_stages: dict[str, str] = {}
    if host_source_root is not None and host_source_list is not None:
        host_sources, host_stages = read_sources(host_source_list, allow_empty=True)
        host_paths = validate_host_sources(host_source_root, host_sources)
    else:
        host_paths = {}
    overlap = sorted(set(vendor_sources) & set(host_sources))
    if overlap:
        fail(f"vendor/host logical source collision: {overlap}")
    sources = vendor_sources + host_sources
    source_stages = {**vendor_stages, **host_stages}
    for source in sources:
        validate_logical_source(source, "source-list entry")
    source_overrides = read_source_map(source_map, vendor_sources)
    overlay_overrides = read_source_overlay(args.source_overlay, vendor_sources)
    source_overrides = {**source_overrides, **overlay_overrides}
    source_paths = {
        **{source: (source_root / source).resolve(strict=False) for source in vendor_sources},
        **host_paths,
    }

    repo_root = source_root.parents[2]
    source_map_root = source_map.parent.resolve()
    roots = [
        (output_dir, "<OUTPUT_DIR>"),
        (source_map_root, "<SOURCE_MAP_ROOT>"),
        (source_root, "<SOURCE_ROOT>"),
        (repo_root, "<REPO>"),
    ]
    if host_source_root is not None and host_source_root != repo_root:
        roots.insert(3, (host_source_root, "<HOST_SOURCE_ROOT>"))

    forbidden_definitions = {
        selector: [item for item in args.define if selector in item]
        for selector in FORBIDDEN_SELECTORS
    }
    selected_forbidden = {
        "definitions": {k: v for k, v in forbidden_definitions.items() if v},
        "source_paths": forbidden_source_paths(sources),
    }
    forbidden_audit = {
        "selectors_checked": list(FORBIDDEN_SELECTORS),
        "selected": selected_forbidden,
        "status": "FAIL" if selected_forbidden["definitions"] or selected_forbidden["source_paths"] else "PASS",
    }

    common_flags = make_common_flags(args)
    object_commands: list[tuple[str, str]] = []
    object_results: list[dict] = []
    object_paths: dict[str, Path] = {}
    seen_object_paths: dict[Path, str] = {}
    compile_cwd = repo_root
    for logical in sources:
        physical = source_overrides.get(logical, source_paths[logical]).resolve(strict=False)
        relative_object = Path(logical).with_suffix(Path(logical).suffix + ".o")
        if logical in host_sources:
            object_path = output_dir / "objects" / "host-owned" / relative_object
            ownership = "project-host"
        elif logical in overlay_overrides:
            object_path = output_dir / "objects" / relative_object
            ownership = "vendor-overlay"
        elif logical in source_overrides:
            object_path = output_dir / "objects" / relative_object
            ownership = "vendor-mapped"
        else:
            object_path = output_dir / "objects" / relative_object
            ownership = "vendor-pristine"
        prior = seen_object_paths.get(object_path)
        if prior is not None:
            fail(f"vendor/host object-path collision: {prior} and {logical} -> {object_path}")
        seen_object_paths[object_path] = logical
        object_path.parent.mkdir(parents=True, exist_ok=True)
        object_paths[logical] = object_path
        quote_flags = []
        overlay_flags = []
        if logical in overlay_overrides:
            quote_flags = ["-iquote", str(physical.parent)]
            overlay_flags = ["-I", str(physical.parent)]
        elif logical in source_overrides:
            quote_flags = ["-iquote", str((source_root / logical).parent.resolve())]
        command = [
            args.compiler,
            *quote_flags,
            *overlay_flags,
            *common_flags,
            "-c",
            str(physical),
            "-o",
            str(object_path),
        ]
        object_commands.append(
            (logical, shlex.join([normalize_text(str(item), roots) for item in command]))
        )
        if selected_forbidden["definitions"] or selected_forbidden["source_paths"]:
            object_results.append(
                {
                    "source": logical,
                    "stage": source_stages[logical],
                    "ownership": ownership,
                    "physical_source": stable_path(physical, roots),
                    "object": stable_path(object_path, roots),
                    "exists": physical.is_file(),
                    "returncode": None,
                    "command": [normalize_text(str(item), roots) for item in command],
                    "diagnostics": "not run: forbidden selector or source path",
                }
            )
            continue
        completed = subprocess.run(
            command,
            cwd=compile_cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode:
            remove_file_artifact(object_path, f"failed object {logical}")
        object_results.append(
            {
                "source": logical,
                "stage": source_stages[logical],
                "ownership": ownership,
                "physical_source": stable_path(physical, roots),
                "object": stable_path(object_path, roots),
                "exists": physical.is_file(),
                "returncode": completed.returncode,
                "command": [normalize_text(str(item), roots) for item in command],
                "diagnostics": normalize_text(completed.stdout, roots),
            }
        )

    passed = sum(item["returncode"] == 0 for item in object_results)
    failed = len(object_results) - passed
    base_summary = {
        "schema_version": SCHEMA_VERSION,
        "compiler": normalize_text(args.compiler, roots),
        "nm": normalize_text(args.nm, roots),
        "source_root": stable_path(source_root, roots),
        "source_list": stable_path(source_list, roots),
        "source_map": stable_path(source_map, roots),
        "include_dirs": [stable_path(Path(item).resolve(), roots) for item in args.include],
        "defines": list(args.define),
        "string_defines": list(args.string_define),
        "cflags": list(args.cflag),
        "stages": {
            stage: [source for source in sources if source_stages[source] == stage]
            for stage in dict.fromkeys(source_stages.values())
        },
        "vendored_source_count": len(vendor_sources),
        "host_source_count": len(host_sources),
        "total_source_count": len(sources),
        "source_count": len(sources),
        "object_compile_passed_count": passed,
        "object_compile_failed_count": failed,
        "relocatable_link_attempted": False,
        "relocatable_link_returncode": None,
        "undefined_symbol_count": 0,
        "undefined_symbols": [],
        "symbol_references": {},
        "forbidden_selector_audit": forbidden_audit,
        "object_results": object_results,
    }
    if host_source_root is not None and host_source_list is not None:
        base_summary["host_source_root"] = stable_path(host_source_root, roots)
        base_summary["host_source_list"] = stable_path(host_source_list, roots)
    if args.source_overlay is not None:
        base_summary["source_overlay"] = stable_path(args.source_overlay.resolve(), roots)

    if selected_forbidden["definitions"] or selected_forbidden["source_paths"]:
        message = "forbidden selector or source path appeared in probe inputs"
        write_reports(output_dir, base_summary, object_commands, [], {}, message)
        return 1

    if failed:
        message = "one or more object compilations failed; relocatable link not attempted"
        write_reports(output_dir, base_summary, object_commands, [], {}, message)
        return 1

    combined = output_dir / "combined.o"
    link_command = [args.compiler, "-r", "-o", str(combined)] + [
        str(object_paths[source]) for source in sources
    ]
    remove_file_artifact(combined, "combined.o")
    linked = subprocess.run(
        link_command,
        cwd=compile_cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    base_summary["relocatable_link_attempted"] = True
    base_summary["relocatable_link_returncode"] = linked.returncode
    base_summary["relocatable_link_command"] = [
        normalize_text(str(item), roots) for item in link_command
    ]
    base_summary["relocatable_link_diagnostics"] = normalize_text(linked.stdout, roots)
    if linked.returncode:
        remove_file_artifact(combined, "combined.o")
        message = "relocatable link failed"
        write_reports(output_dir, base_summary, object_commands, [], {}, message)
        return 1

    nm_combined = subprocess.run(
        [args.nm, "-u", str(combined)],
        cwd=compile_cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if nm_combined.returncode:
        base_summary["nm_diagnostics"] = normalize_text(nm_combined.stdout, roots)
        write_reports(output_dir, base_summary, object_commands, [], {}, "combined nm failed")
        return 1
    undefined_symbols = parse_nm_undefined(nm_combined.stdout)

    references: dict[str, set[str]] = {}
    for logical in sources:
        nm_object = subprocess.run(
            [args.nm, "-u", str(object_paths[logical])],
            cwd=compile_cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if nm_object.returncode:
            base_summary["nm_diagnostics"] = normalize_text(nm_object.stdout, roots)
            write_reports(output_dir, base_summary, object_commands, [], {}, "object nm failed")
            return 1
        for symbol in parse_nm_undefined(nm_object.stdout):
            references.setdefault(symbol, set()).add(logical)

    symbol_references = {
        symbol: sorted(references.get(symbol, set())) for symbol in undefined_symbols
    }
    base_summary["undefined_symbol_count"] = len(undefined_symbols)
    base_summary["undefined_symbols"] = undefined_symbols
    base_summary["symbol_references"] = symbol_references
    write_reports(output_dir, base_summary, object_commands, undefined_symbols, symbol_references)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ProbeError, OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
