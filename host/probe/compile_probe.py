#!/usr/bin/env python3
"""Run explicit, non-linking Phase 1 compile probes.

This is an evidence collector, not a dependency resolver. It deliberately
does not synthesize headers, declarations, compatibility functions, or source
files. Every translation unit is probed independently so the first missing
contract for each candidate is retained in the ignored build output.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from pathlib import Path


MISSING_HEADER_RE = re.compile(
    r"(?:fatal error|error): ['\"]?([^:'\"\n]+)['\"]?: No such file or directory"
)
IMPLICIT_FUNCTION_RE = re.compile(
    r"implicit declaration of function ['`]([^'`]+)['`]"
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"\n]+)[>\"]")
GLOB_CHARS = set("*?[]{}")
FORBIDDEN_SELECTORS = (
    "CPUCORE_IA32",
    "SUPPORT_IA32_HAXM",
    "USE_SDL",
    "i386",
    "i286x",
)
FORBIDDEN_SOURCE_PARTS = ("i386c/", "i386hax/", "i286x/")


def read_sources(path: Path) -> tuple[list[str], dict[str, str]]:
    sources: list[str] = []
    stages: dict[str, str] = {}
    stage = "unlabeled"
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        marker = raw.strip()
        if marker.lower().startswith("# stage:"):
            stage = marker.split(":", 1)[1].strip() or "unlabeled"
            continue
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if any(char in line for char in GLOB_CHARS):
            raise SystemExit(f"source list must not contain glob syntax: {path}:{lineno}: {line}")
        sources.append(line)
        stages[line] = stage
    if not sources:
        raise SystemExit(f"source list is empty: {path}")
    return sources, stages


def split_missing_headers(stderr: str) -> list[str]:
    found = []
    for match in MISSING_HEADER_RE.finditer(stderr):
        header = match.group(1).strip()
        if header not in found:
            found.append(header)
    return found


def safe_source_map_path(value: object, label: str) -> Path:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise SystemExit(f"unsafe {label}: {value!r}")
    if "\\" in value:
        raise SystemExit(f"backslash in {label}: {value!r}")
    path = Path(value)
    if path.is_absolute() or path.as_posix() != value:
        raise SystemExit(f"unsafe {label}: {value!r}")
    if not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise SystemExit(f"unsafe {label}: {value!r}")
    return path


def read_source_map(path: Path, sources: list[str]) -> dict[str, Path]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SystemExit(f"cannot read source map {path}: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise SystemExit(f"unsupported source-map schema: {path}")
    entries = value.get("entries")
    if not isinstance(entries, list):
        raise SystemExit(f"source-map entries must be a list: {path}")
    source_set = set(sources)
    mapped: dict[str, Path] = {}
    map_root = path.parent.resolve()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {"logical_source", "generated_path"}:
            raise SystemExit(f"invalid source-map entry {index}: {path}")
        logical = entry["logical_source"]
        if not isinstance(logical, str) or logical not in source_set:
            raise SystemExit(f"source-map logical source is not in the inventory: {logical!r}")
        if logical in mapped:
            raise SystemExit(f"duplicate source-map logical source: {logical}")
        generated_relative = safe_source_map_path(entry["generated_path"], "generated_path")
        generated = map_root / generated_relative
        current = map_root
        for part in generated_relative.parts:
            current /= part
            if current.is_symlink():
                raise SystemExit(f"source-map generated path contains a symlink: {current}")
        resolved = generated.resolve(strict=False)
        try:
            resolved.relative_to(map_root)
        except ValueError as exc:
            raise SystemExit(f"source-map generated path escapes its directory: {generated}") from exc
        if not generated.is_file():
            raise SystemExit(f"source-map generated file is missing: {generated}")
        mapped[logical] = generated
    return mapped


def include_inventory(
    source_root: Path,
    include_dirs: list[Path],
    sources: list[str],
    source_overrides: dict[str, Path],
) -> tuple[list[dict], list[dict]]:
    references = []
    unresolved = []
    for relative in sources:
        source = source_overrides.get(relative, (source_root / relative).resolve())
        original_source = (source_root / relative).resolve()
        if not source.is_file():
            continue
        for lineno, raw in enumerate(source.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = INCLUDE_RE.match(raw)
            if not match:
                continue
            delimiter, include = match.groups()
            candidates = []
            if delimiter == '"':
                candidates.append(original_source.parent / include)
            candidates.extend(include_dir / include for include_dir in include_dirs)
            resolved = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            item = {
                "source": relative,
                "line": lineno,
                "include": include,
                "delimiter": delimiter,
                "resolved": str(resolved) if resolved else None,
            }
            references.append(item)
            if resolved is None:
                unresolved.append(item)
    return references, unresolved


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-list", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-map", type=Path)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--string-define", action="append", default=[])
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    sources, source_stages = read_sources(args.source_list)
    source_map_argument = args.source_map.absolute() if args.source_map else None
    if source_map_argument and source_map_argument.is_symlink():
        raise SystemExit(f"source map may not be a symlink: {source_map_argument}")
    source_overrides = (
        read_source_map(source_map_argument, sources) if source_map_argument else {}
    )
    forbidden_definitions = {
        selector: [item for item in args.define if selector in item]
        for selector in FORBIDDEN_SELECTORS
    }
    forbidden_paths = {
        selector: [item for item in sources if item.startswith(selector)]
        for selector in FORBIDDEN_SOURCE_PARTS
    }
    selected_forbidden = {
        "definitions": {key: value for key, value in forbidden_definitions.items() if value},
        "source_paths": {key: value for key, value in forbidden_paths.items() if value},
    }
    if selected_forbidden["definitions"] or selected_forbidden["source_paths"]:
        raise SystemExit(
            "forbidden IA-32/SDL selector appeared in Phase 1 probe inputs: "
            + json.dumps(selected_forbidden, sort_keys=True)
        )
    include_dirs = [Path(item).resolve() for item in args.include]
    include_references, unresolved_includes = include_inventory(
        source_root, include_dirs, sources, source_overrides
    )

    common_flags = []
    for include in args.include:
        common_flags.append(f"-I{Path(include).resolve()}")
    for definition in args.define:
        common_flags.append(f"-D{definition}")
    for definition in args.string_define:
        name, separator, value = definition.partition("=")
        if not separator:
            raise SystemExit(f"--string-define requires NAME=VALUE: {definition}")
        escaped = value.replace('\\', '\\\\').replace('"', '\\"')
        common_flags.append(f'-D{name}="{escaped}"')
    common_flags.extend(args.cflag)

    results = []
    command_lines = []
    for relative in sources:
        source = source_overrides.get(relative, (source_root / relative).resolve())
        quote_flags = []
        if relative in source_overrides:
            quote_flags = ["-iquote", str((source_root / relative).resolve().parent)]
        command = [args.compiler, *quote_flags, *common_flags, "-fsyntax-only", str(source)]
        command_lines.append(shlex.join(command))
        completed = subprocess.run(
            command,
            cwd=source_root.parent.parent,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        output = completed.stdout
        results.append(
            {
                "source": relative,
                "stage": source_stages[relative],
                "exists": source.is_file(),
                "returncode": completed.returncode,
                "command": command,
                "missing_headers": split_missing_headers(output),
                "implicit_function_diagnostics": sorted(
                    set(IMPLICIT_FUNCTION_RE.findall(output))
                ),
                "diagnostics": output,
            }
        )

    passed = sum(item["returncode"] == 0 for item in results)
    summary = {
        "compiler": args.compiler,
        "source_root": str(source_root),
        "source_list": str(args.source_list.resolve()),
        "include_dirs": [str(item) for item in include_dirs],
        "defines": args.define,
        "string_defines": args.string_define,
        "cflags": args.cflag,
        "stages": {
            stage: [source for source in sources if source_stages[source] == stage]
            for stage in dict.fromkeys(source_stages.values())
        },
        "forbidden_selector_audit": {
            "selectors_checked": list(FORBIDDEN_SELECTORS),
            "selected": selected_forbidden,
            "status": "PASS",
        },
        "compile_only": True,
        "link_probe": "not-run; compile inventory only",
        "link_unresolved_symbols": [],
        "unresolved_symbols": sorted(
            {
                symbol
                for item in results
                for symbol in item["implicit_function_diagnostics"]
            }
        ),
        "include_reference_count": len(include_references),
        "unresolved_include_count": len(unresolved_includes),
        "unresolved_includes": unresolved_includes,
        "source_count": len(results),
        "passed_count": passed,
        "failed_count": len(results) - passed,
        "results": results,
    }
    if source_map_argument:
        summary["source_map"] = str(source_map_argument.resolve())
    (output_dir / "compile_commands.txt").write_text(
        "\n".join(command_lines) + "\n", encoding="utf-8"
    )
    (output_dir / "compile-results.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    text_lines = [
        "Phase 1 compile-only inventory",
        f"sources={len(results)} passed={passed} failed={len(results) - passed}",
        "link_probe=not-run; compile inventory only",
        "forbidden_selector_audit=PASS (CPUCORE_IA32 SUPPORT_IA32_HAXM USE_SDL i386 i286x)",
        f"include_references={len(include_references)} unresolved_includes={len(unresolved_includes)}",
        "",
    ]
    for item in results:
        status = "PASS" if item["returncode"] == 0 else f"FAIL({item['returncode']})"
        missing = ", ".join(item["missing_headers"]) or "-"
        text_lines.append(
            f"{status}\tstage={item['stage']}\t{item['source']}\tmissing_headers={missing}"
        )
    text_lines.extend(["", "Unresolved textual includes:"])
    for item in unresolved_includes:
        text_lines.append(
            f"{item['source']}:{item['line']}\t{item['delimiter']}{item['include']}"
        )
    (output_dir / "compile-results.txt").write_text(
        "\n".join(text_lines) + "\n", encoding="utf-8"
    )
    # Inventory probes intentionally return success so all translation units
    # are attempted and their evidence is available for the review gate.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
