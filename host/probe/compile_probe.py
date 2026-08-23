#!/usr/bin/env python3
"""Run explicit, non-linking compile probes for vendor and host sources."""

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
        or any(char in value for char in GLOB_CHARS)
    ):
        raise SystemExit(f"unsafe {label}: {value!r}")


def read_sources(
    path: Path, *, allow_empty: bool = False
) -> tuple[list[str], dict[str, str]]:
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
        validate_logical_source(line, f"source-list entry {path}:{lineno}")
        if line in stages:
            raise SystemExit(f"duplicate source-list entry: {path}:{lineno}: {line}")
        sources.append(line)
        stages[line] = stage
    if not sources and not allow_empty:
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


def read_source_overlay(path: Path | None, vendor_sources: list[str]) -> dict[str, Path]:
    """Map I286/V30 vendor units to the prepared host overlay, if supplied."""

    if path is None:
        return {}
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        if current.is_symlink():
            raise SystemExit(f"source overlay contains an unexpected symlink: {current}")
    if not absolute.is_dir():
        raise SystemExit(f"source overlay is not a directory: {absolute}")
    root = absolute.resolve()
    mapped: dict[str, Path] = {}
    for logical in vendor_sources:
        if not logical.startswith("i286c/"):
            continue
        candidate = root / logical
        current = root
        for part in Path(logical).parts:
            current /= part
            if current.is_symlink():
                raise SystemExit(f"source overlay contains an unexpected symlink: {current}")
        resolved = candidate.resolve(strict=False)
        try:
            resolved.relative_to(root)
        except ValueError as exc:
            raise SystemExit(f"source overlay escapes its directory: {candidate}") from exc
        if not candidate.is_file():
            raise SystemExit(f"source overlay is missing regular file: {logical}")
        mapped[logical] = candidate
    if not mapped:
        raise SystemExit("source overlay contains no I286/V30 vendor units")
    return mapped


def include_inventory(
    include_dirs: list[Path],
    sources: list[str],
    source_roots: dict[str, Path],
    source_overrides: dict[str, Path],
    overlay_sources: set[str],
) -> tuple[list[dict], list[dict]]:
    references = []
    unresolved = []
    for relative in sources:
        source_root = source_roots[relative]
        source = source_overrides.get(relative, (source_root / relative).resolve())
        original_source = (source_root / relative).resolve()
        include_source = source.resolve() if relative in overlay_sources else original_source
        if not source.is_file():
            continue
        for lineno, raw in enumerate(
            source.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            match = INCLUDE_RE.match(raw)
            if not match:
                continue
            delimiter, include = match.groups()
            candidates = []
            if delimiter == '"':
                candidates.append(include_source.parent / include)
            if relative in overlay_sources:
                candidates.append(include_source.parent / include)
            candidates.extend(include_dir / include for include_dir in include_dirs)
            resolved = next(
                (candidate.resolve() for candidate in candidates if candidate.is_file()),
                None,
            )
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


def parse_host_inputs(args: argparse.Namespace) -> tuple[Path | None, Path | None]:
    has_root = args.host_source_root is not None
    has_list = args.host_source_list is not None
    if has_root != has_list:
        raise SystemExit(
            "--host-source-root and --host-source-list must be provided together"
        )
    if not has_root:
        return None, None
    host_root = args.host_source_root.resolve()
    host_list = args.host_source_list.resolve()
    if not host_root.is_dir():
        raise SystemExit(f"host source root is not a directory: {host_root}")
    if not host_list.is_file():
        raise SystemExit(f"host source list is not a file: {host_list}")
    return host_root, host_list


def validate_host_sources(host_root: Path, sources: list[str]) -> dict[str, Path]:
    resolved: dict[str, Path] = {}
    for logical in sources:
        candidate = (host_root / logical).resolve(strict=False)
        try:
            candidate.relative_to(host_root)
        except ValueError as exc:
            raise SystemExit(
                f"host source escapes declared root: {logical!r} -> {candidate}"
            ) from exc
        resolved[logical] = candidate
    return resolved


def forbidden_source_paths(sources: list[str]) -> dict[str, list[str]]:
    forbidden: dict[str, list[str]] = {}
    for selector in FORBIDDEN_SOURCE_PARTS:
        matches = [
            source
            for source in sources
            if source.startswith(selector)
            or selector.rstrip("/") in Path(source).parts
        ]
        if matches:
            forbidden[selector] = matches
    return forbidden


def normalize_text(value: str, roots: list[tuple[Path, str]]) -> str:
    normalized = value
    for root, label in sorted(roots, key=lambda item: len(str(item[0])), reverse=True):
        normalized = normalized.replace(str(root), label)
    return normalized.replace("\\", "/")


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-list", type=Path, required=True)
    parser.add_argument("--host-source-root", type=Path)
    parser.add_argument("--host-source-list", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-map", type=Path)
    parser.add_argument("--source-overlay", type=Path)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--string-define", action="append", default=[])
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    host_source_root, host_source_list = parse_host_inputs(args)

    vendor_sources, vendor_stages = read_sources(args.source_list)
    host_sources: list[str] = []
    host_stages: dict[str, str] = {}
    if host_source_root is not None and host_source_list is not None:
        host_sources, host_stages = read_sources(host_source_list, allow_empty=True)
        host_paths = validate_host_sources(host_source_root, host_sources)
    else:
        host_paths = {}
    overlap = sorted(set(vendor_sources) & set(host_sources))
    if overlap:
        raise SystemExit(f"vendor/host logical source collision: {overlap}")

    sources = vendor_sources + host_sources
    source_stages = {**vendor_stages, **host_stages}
    source_roots = {
        **{source: source_root for source in vendor_sources},
        **{source: host_source_root for source in host_sources},
    }
    source_paths = {
        **{source: (source_root / source).resolve(strict=False) for source in vendor_sources},
        **host_paths,
    }

    source_map_argument = args.source_map.absolute() if args.source_map else None
    if source_map_argument and source_map_argument.is_symlink():
        raise SystemExit(f"source map may not be a symlink: {source_map_argument}")
    source_overrides = (
        read_source_map(source_map_argument, vendor_sources)
        if source_map_argument
        else {}
    )
    overlay_overrides = read_source_overlay(args.source_overlay, vendor_sources)
    source_overrides = {**source_overrides, **overlay_overrides}
    forbidden_definitions = {
        selector: [item for item in args.define if selector in item]
        for selector in FORBIDDEN_SELECTORS
    }
    selected_forbidden = {
        "definitions": {key: value for key, value in forbidden_definitions.items() if value},
        "source_paths": forbidden_source_paths(sources),
    }
    if selected_forbidden["definitions"] or selected_forbidden["source_paths"]:
        raise SystemExit(
            "forbidden IA-32/SDL selector appeared in Phase 1 probe inputs: "
            + json.dumps(selected_forbidden, sort_keys=True)
        )

    include_dirs = [Path(item).resolve() for item in args.include]
    include_references, unresolved_includes = include_inventory(
        include_dirs, sources, source_roots, source_overrides, set(overlay_overrides)
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

    repo_root = source_root.parents[2]
    roots: list[tuple[Path, str]] = [(source_root, "<SOURCE_ROOT>"), (repo_root, "<REPO>")]
    if host_source_root is not None and host_source_root != repo_root:
        roots.insert(1, (host_source_root, "<HOST_SOURCE_ROOT>"))
    if source_map_argument:
        roots.insert(0, (source_map_argument.parent.resolve(), "<SOURCE_MAP_ROOT>"))
    roots.insert(0, (output_dir, "<OUTPUT_DIR>"))

    results = []
    command_lines = []
    for relative in sources:
        source = source_overrides.get(relative, source_paths[relative])
        quote_flags = []
        overlay_flags = []
        if relative in overlay_overrides:
            quote_flags = ["-iquote", str(source.parent)]
            overlay_flags = ["-I", str(source.parent)]
        elif relative in source_overrides:
            quote_flags = ["-iquote", str((source_root / relative).resolve().parent)]
        command = [
            args.compiler,
            *quote_flags,
            *overlay_flags,
            *common_flags,
            "-fsyntax-only",
            str(source),
        ]
        normalized_command = [normalize_text(str(item), roots) for item in command]
        command_lines.append(shlex.join(normalized_command))
        completed = subprocess.run(
            command,
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        output = normalize_text(completed.stdout, roots)
        if relative in overlay_overrides:
            ownership = "vendor-overlay"
        elif relative in source_overrides:
            ownership = "vendor-mapped"
        elif relative in vendor_stages:
            ownership = "vendor-pristine"
        else:
            ownership = "project-host"
        results.append(
            {
                "source": relative,
                "stage": source_stages[relative],
                "ownership": ownership,
                "exists": source.is_file(),
                "returncode": completed.returncode,
                "command": normalized_command,
                "missing_headers": split_missing_headers(output),
                "implicit_function_diagnostics": sorted(set(IMPLICIT_FUNCTION_RE.findall(output))),
                "diagnostics": output,
            }
        )

    passed = sum(item["returncode"] == 0 for item in results)
    normalized_unresolved_includes = []
    for item in unresolved_includes:
        normalized_item = dict(item)
        if normalized_item["resolved"]:
            normalized_item["resolved"] = normalize_text(normalized_item["resolved"], roots)
        normalized_unresolved_includes.append(normalized_item)
    summary = {
        "schema_version": 2,
        "compiler": args.compiler,
        "source_root": stable_path(source_root, roots),
        "source_list": stable_path(args.source_list.resolve(), roots),
        "include_dirs": [stable_path(item, roots) for item in include_dirs],
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
            {symbol for item in results for symbol in item["implicit_function_diagnostics"]}
        ),
        "include_reference_count": len(include_references),
        "unresolved_include_count": len(normalized_unresolved_includes),
        "unresolved_includes": normalized_unresolved_includes,
        "vendored_source_count": len(vendor_sources),
        "host_source_count": len(host_sources),
        "total_source_count": len(results),
        "source_count": len(results),
        "passed_count": passed,
        "failed_count": len(results) - passed,
        "results": results,
    }
    if source_map_argument:
        summary["source_map"] = stable_path(source_map_argument.resolve(), roots)
    if args.source_overlay is not None:
        summary["source_overlay"] = stable_path(args.source_overlay.resolve(), roots)
    if host_source_root is not None and host_source_list is not None:
        summary["host_source_root"] = stable_path(host_source_root, roots)
        summary["host_source_list"] = stable_path(host_source_list, roots)

    (output_dir / "compile_commands.txt").write_text(
        "\n".join(command_lines) + "\n", encoding="utf-8"
    )
    (output_dir / "compile-results.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    text_lines = [
        "Phase 1 compile-only inventory",
        f"vendored_sources={len(vendor_sources)}",
        f"host_sources={len(host_sources)}",
        f"total_sources={len(results)}",
        f"sources={len(results)} passed={passed} failed={len(results) - passed}",
        "link_probe=not-run; compile inventory only",
        "forbidden_selector_audit=PASS (CPUCORE_IA32 SUPPORT_IA32_HAXM USE_SDL i386 i286x)",
        f"include_references={len(include_references)} unresolved_includes={len(normalized_unresolved_includes)}",
        "",
    ]
    for item in results:
        status = "PASS" if item["returncode"] == 0 else f"FAIL({item['returncode']})"
        missing = ", ".join(item["missing_headers"]) or "-"
        text_lines.append(
            f"{status}\tstage={item['stage']}\t{item['source']}\tmissing_headers={missing}"
        )
    text_lines.extend(["", "Unresolved textual includes:"])
    for item in normalized_unresolved_includes:
        text_lines.append(
            f"{item['source']}:{item['line']}\t{item['delimiter']}{item['include']}"
        )
    (output_dir / "compile-results.txt").write_text(
        "\n".join(text_lines) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
