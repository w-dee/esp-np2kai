#!/usr/bin/env python3
"""Prove the narrow 86R.1 prepared-source compile/link ownership boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


PINNED_COMMIT = "e2dc9046aa5c786fcfbfb87e883457e421026e31"
EXPECTED_MANIFEST_COUNT = 306
EXPECTED_SOURCES = ("cbus/board86.c", "cbus/pcm86io.c")
EXPECTED_SHA256 = {
    "cbus/board86.c": "c2db5b7f724ac3c4042830b06179a77839d46d74e3ba51f5fd7a3ad5f751b842",
    "cbus/pcm86io.c": "e81275514f7814c3698983d867bf8d5bcc991d4d50085900feec8b04dab653f5",
}
FORBIDDEN_PATTERNS = {
    "direct generator mutation": re.compile(r"\b(?:opngen|psggen|rhythm)_[A-Za-z0-9_]*\s*\("),
    "direct full OPNA/PCM86 globals": re.compile(r"\bg_(?:opna|pcm86)\b"),
    "broad sound synchronization": re.compile(r"\b(?:sound_sync|sound_streamregist)\s*\("),
    "direct fmboard mutation": re.compile(r"\bfmboard_(?:extreg|extenable)\s*\("),
    "direct PCM waveform buffer": re.compile(r"\.\s*buffer\s*\["),
    "full guest-owned PCM86 object": re.compile(r"\b_PCM86\s+g_[A-Za-z0-9_]+"),
}


def run(command: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_inventory(path: Path) -> list[str]:
    values: list[str] = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        value = raw.split("#", 1)[0].strip()
        if not value:
            continue
        candidate = Path(value)
        if (
            candidate.is_absolute()
            or candidate.as_posix() != value
            or not candidate.parts
            or any(part in {"", ".", ".."} for part in candidate.parts)
            or "\\" in value
        ):
            raise RuntimeError(f"unsafe inventory entry {path}:{line_number}: {value!r}")
        if value in values:
            raise RuntimeError(f"duplicate inventory entry {path}:{line_number}: {value}")
        values.append(value)
    return values


def validate_inputs(root: Path, upstream_root: Path | None) -> None:
    manifest_path = root / "third_party/np2kai/import-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("upstream", {}).get("commit") != PINNED_COMMIT:
        raise RuntimeError("import manifest is not pinned to the 86R.1 commit")
    files = manifest.get("files")
    if not isinstance(files, list) or len(files) != EXPECTED_MANIFEST_COUNT:
        raise RuntimeError(f"manifest count is not {EXPECTED_MANIFEST_COUNT}")
    destinations = {item.get("destination_path") for item in files}
    for source in EXPECTED_SOURCES:
        destination = f"src/{source}"
        if destination not in destinations:
            raise RuntimeError(f"manifest is missing {destination}")

        imported = root / "third_party/np2kai/src" / source
        actual = sha256(imported)
        expected = EXPECTED_SHA256[source]
        if actual != expected:
            raise RuntimeError(f"imported SHA mismatch for {source}: {actual}")
        if upstream_root is not None:
            upstream = upstream_root / source
            if not upstream.is_file() or sha256(upstream) != actual:
                raise RuntimeError(f"pinned upstream SHA mismatch for {source}")

    inventory = read_inventory(root / "host/probe/audio86_guest_sources.txt")
    if tuple(inventory) != EXPECTED_SOURCES:
        raise RuntimeError(f"unexpected audio86 inventory: {inventory!r}")
    core_inventory = set(read_inventory(root / "host/probe/host_core_sources.txt"))
    if core_inventory.intersection(inventory):
        raise RuntimeError("audio86 inventory duplicates an existing core source")


def prepare(root: Path, output: Path) -> tuple[Path, Path]:
    preparer = root / "host/tools/prepare_step4_sources.py"
    vendor_verify = root / "tools/np2kai/verify_np2kai.py"
    vendor = root / "third_party/np2kai"
    manifest = vendor / "import-manifest.json"
    patch_set = root / "host/patches/np2kai/audio86/patch-set.json"
    run([sys.executable, str(vendor_verify), "--vendor-root", str(vendor)])
    first = output / "prepared-one"
    second = output / "prepared-two"
    for destination in (first, second):
        if destination.exists():
            shutil.rmtree(destination)
        run(
            [
                sys.executable,
                str(preparer),
                "--vendor-root",
                str(vendor),
                "--import-manifest",
                str(manifest),
                "--patch-set",
                str(patch_set),
                "--output-root",
                str(destination),
            ]
        )
    comparison = subprocess.run(
        ["diff", "-ru", "--no-dereference", str(first), str(second)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if comparison.returncode:
        raise RuntimeError(f"prepared-source generation is not deterministic:\n{comparison.stdout}")
    return first, second


def compile_and_link(root: Path, prepared: Path, output: Path, compiler: str, nm: str) -> Path:
    objects = output / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    include = [
        f"-I{root / 'host/compat'}",
        f"-I{root / 'third_party/np2kai/src'}",
        f"-I{root / 'third_party/np2kai/src/i286c'}",
        f"-I{root / 'firmware/components/np2audio86_guest_adapter/include'}",
    ]
    flags = [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=unused-parameter",
        "-fno-common",
        "-DBYTESEX_LITTLE",
        "-DDISABLE_SOUND=1",
        "-DNP2_ASYNC_AUDIO86=1",
    ]
    sources = [
        prepared / "patched-src/cbus/board86.c",
        prepared / "patched-src/cbus/pcm86io.c",
        root / "host/probe/np2audio86_guest_contract_stubs.c",
    ]
    compiled: list[Path] = []
    for source in sources:
        object_path = objects / f"{source.stem}-{len(compiled)}.o"
        run([compiler, *flags, *include, "-c", str(source), "-o", str(object_path)])
        compiled.append(object_path)
    closure = output / "audio86-guest-closure.o"
    run(["ld", "-r", *map(str, compiled), "-o", str(closure)])
    unresolved = subprocess.run(
        [nm, "-u", str(closure)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=True,
    ).stdout.strip()
    if unresolved:
        raise RuntimeError(f"unexpected unresolved closure symbols:\n{unresolved}")
    return closure


def static_ownership_check(root: Path, prepared: Path, output: Path, compiler: str) -> None:
    include = [
        f"-I{root / 'host/compat'}",
        f"-I{root / 'third_party/np2kai/src'}",
        f"-I{root / 'third_party/np2kai/src/i286c'}",
        f"-I{root / 'firmware/components/np2audio86_guest_adapter/include'}",
    ]
    flags = ["-DBYTESEX_LITTLE", "-DDISABLE_SOUND=1", "-DNP2_ASYNC_AUDIO86=1"]
    preprocessed = output / "preprocessed"
    preprocessed.mkdir(parents=True, exist_ok=True)
    for source_name in EXPECTED_SOURCES:
        source = prepared / "patched-src" / source_name
        target = preprocessed / (Path(source_name).name + ".i")
        command = [compiler, "-E", "-P", *flags, *include, str(source), "-o", str(target)]
        run(command)
        text = target.read_text(encoding="utf-8", errors="replace")
        for label, pattern in FORBIDDEN_PATTERNS.items():
            match = pattern.search(text)
            if match:
                raise RuntimeError(f"{label} escaped the adapter seam in {source_name}: {match.group(0)!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--upstream-root", type=Path)
    parser.add_argument("--compiler", default="gcc")
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (args.output_dir or root / "host/build/phase2/audio86-contract").resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        validate_inputs(root, args.upstream_root.resolve() if args.upstream_root else None)
        prepared, _ = prepare(root, output)
        compile_and_link(root, prepared, output, args.compiler, args.nm)
        static_ownership_check(root, prepared, output, args.compiler)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"AUDIO86_GUEST_CONTRACT=FAIL {exc}", file=sys.stderr)
        return 1
    print("AUDIO86_GUEST_CONTRACT=PASS")
    print("DISABLE_SOUND=1 NP2_ASYNC_AUDIO86=1 SUPPORT_FMGEN=undefined")
    print("AUDIO86_GUEST_INVENTORY=PASS count=2")
    print("AUDIO86_GUEST_PREPARATION=PASS deterministic=1")
    print("AUDIO86_GUEST_LINK=PASS unresolved=0")
    print("AUDIO86_GUEST_OWNERSHIP=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
