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
import shlex
import subprocess
from pathlib import Path
from compile_probe import (
    FORBIDDEN_SELECTORS,
    FORBIDDEN_SOURCE_PARTS,
    read_source_map,
    read_sources,
)


SCHEMA_VERSION = 1
SOURCE_PATH_GLOB_CHARS = set("*?[]{}")


class ProbeError(RuntimeError):
    """A structural probe failure."""


def fail(message: str) -> None:
    raise ProbeError(message)


def validate_logical_source(value: str, label: str) -> None:
    path = Path(value)
    if (
        not value
        or "\x00" in value
        or "\\" in value
        or path.is_absolute()
        or path.as_posix() != value
        or not path.parts
        or any(part in {"", ".", ".."} for part in path.parts)
        or any(char in value for char in SOURCE_PATH_GLOB_CHARS)
    ):
        fail(f"unsafe {label}: {value!r}")


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
        f"sources={summary['source_count']}",
        "object_compile="
        f"{summary['object_compile_passed_count']}/"
        f"{summary['source_count']} PASS, "
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
    parser.add_argument("--source-map", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--string-define", action="append", default=[])
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    source_list = args.source_list.resolve()
    source_map = args.source_map.resolve()
    output_dir = args.output_dir.resolve()
    if not source_root.is_dir():
        fail(f"source root is not a directory: {source_root}")
    if not source_list.is_file():
        fail(f"source list is not a file: {source_list}")
    if args.source_map.is_symlink():
        fail(f"source map may not be a symlink: {args.source_map}")
    if not source_map.is_file():
        fail(f"source map is not a file: {source_map}")

    sources, source_stages = read_sources(source_list)
    for source in sources:
        validate_logical_source(source, "source-list entry")
    source_overrides = read_source_map(source_map, sources)
    output_dir.mkdir(parents=True, exist_ok=True)

    repo_root = source_root.parents[2]
    source_map_root = source_map.parent.resolve()
    roots = [
        (output_dir, "<OUTPUT_DIR>"),
        (source_map_root, "<SOURCE_MAP_ROOT>"),
        (source_root, "<SOURCE_ROOT>"),
        (repo_root, "<REPO>"),
    ]

    forbidden_definitions = {
        selector: [item for item in args.define if selector in item]
        for selector in FORBIDDEN_SELECTORS
    }
    forbidden_paths = {
        selector: [item for item in sources if item.startswith(selector)]
        for selector in FORBIDDEN_SOURCE_PARTS
    }
    selected_forbidden = {
        "definitions": {k: v for k, v in forbidden_definitions.items() if v},
        "source_paths": {k: v for k, v in forbidden_paths.items() if v},
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
    compile_cwd = repo_root
    for logical in sources:
        physical = source_overrides.get(logical, source_root / logical).resolve(strict=False)
        object_path = output_dir / "objects" / Path(logical).with_suffix(Path(logical).suffix + ".o")
        object_path.parent.mkdir(parents=True, exist_ok=True)
        object_paths[logical] = object_path
        quote_flags = []
        if logical in source_overrides:
            quote_flags = ["-iquote", str((source_root / logical).parent.resolve())]
        command = [args.compiler, *quote_flags, *common_flags, "-c", str(physical), "-o", str(object_path)]
        object_commands.append(
            (logical, shlex.join([normalize_text(str(item), roots) for item in command]))
        )
        if selected_forbidden["definitions"] or selected_forbidden["source_paths"]:
            object_results.append(
                {
                    "source": logical,
                    "stage": source_stages[logical],
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
        object_results.append(
            {
                "source": logical,
                "stage": source_stages[logical],
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
