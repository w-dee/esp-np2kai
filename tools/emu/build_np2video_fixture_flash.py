#!/usr/bin/env python3
"""Overlay the generated np2video image into the existing np2test raw slot."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


PARTITION_TYPE = 0x40
PARTITION_SUBTYPE = 0x01
FACTORY_SIZE = 0x100000
FIXTURE_OFFSET = 0x110000
FIXTURE_SIZE = 0x134000


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def load_golden(path: Path) -> tuple[int, str]:
    try:
        root: Any = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read golden.json: {exc}")
    if not isinstance(root, dict) or root.get("schema_version") != 1:
        fail("golden.json is not schema v1")
    size = root.get("image_size")
    digest = root.get("fixture_sha256")
    if (isinstance(size, bool) or not isinstance(size, int) or size != FIXTURE_SIZE or
            not isinstance(digest, str) or len(digest) != 64 or
            any(character not in "0123456789abcdef" for character in digest)):
        fail("golden.json fixture identity is invalid for the np2test slot")
    return size, digest


def load_partition_table(idf_path: Path, table_path: Path):
    sys.path.insert(0, str(idf_path / "components" / "partition_table"))
    import gen_esp32part  # type: ignore

    try:
        return gen_esp32part.PartitionTable.from_binary(table_path.read_bytes())
    except (OSError, ValueError) as exc:
        fail(f"cannot parse partition table {table_path}: {exc}")


def read_flash_args(build_dir: Path) -> tuple[list[str], list[tuple[int, Path]]]:
    try:
        lines = (build_dir / "flash_args").read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        fail(f"cannot read flash_args: {exc}")
    if not lines:
        fail("flash_args is empty")
    options = shlex.split(lines[0])
    images: list[tuple[int, Path]] = []
    for line in lines[1:]:
        fields = shlex.split(line)
        if len(fields) != 2:
            fail(f"invalid flash_args line: {line}")
        try:
            offset = int(fields[0], 0)
        except ValueError as exc:
            fail(f"invalid flash offset in {line}: {exc}")
        images.append((offset, build_dir / fields[1]))
    return options, images


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        fail(f"cannot hash {path}: {exc}")
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    repository_root = args.repository_root.resolve()
    build_dir = args.build_dir.resolve()
    descriptor = args.descriptor.resolve()
    fixture_image = args.fixture.resolve()
    output = args.output.resolve()
    image_size, expected_digest = load_golden(descriptor)
    if fixture_image.stat().st_size != image_size:
        fail(f"generated image size is {fixture_image.stat().st_size}, expected {image_size}")
    generated_digest = sha256(fixture_image)
    print(f"NP2VIDEO_GOLDEN_SOURCE size={image_size} sha256={expected_digest}")
    print(f"NP2VIDEO_GENERATED_IMAGE path={fixture_image} size={image_size} "
          f"sha256={generated_digest}")
    if generated_digest != expected_digest:
        fail("generated image does not match golden.json")

    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set")
    idf_path = Path(idf_path_value).resolve()
    table = load_partition_table(
        idf_path, build_dir / "partition_table" / "partition-table.bin")
    fixture = table.find_by_name("np2test")
    factory = table.find_by_name("factory")
    if fixture is None or factory is None:
        fail("required factory/np2test partitions are missing")
    if (factory.size != FACTORY_SIZE or fixture.offset != FIXTURE_OFFSET or
            fixture.size != FIXTURE_SIZE or
            (fixture.type, fixture.subtype) != (PARTITION_TYPE, PARTITION_SUBTYPE) or
            not fixture.readonly):
        fail("video profile partition table does not preserve the approved raw slot")

    options, images = read_flash_args(build_dir)
    if "--flash_size" not in options:
        fail("flash_args has no flash size")
    flash_size = options[options.index("--flash_size") + 1]
    if flash_size != "8MB":
        fail(f"video flash profile requires 8MB, got {flash_size}")
    for _, image in images:
        if not image.is_file():
            fail(f"flash image is missing: {image}")

    output.parent.mkdir(parents=True, exist_ok=True)
    esptool = idf_path / "components/esptool_py/esptool/esptool.py"
    merge_command = [
        sys.executable, str(esptool), "--chip", "esp32p4", "merge_bin",
        "--output", str(output), "--format", "raw", "--flash_mode", "dio",
        "--flash_freq", "80m", "--flash_size", flash_size,
        "--fill-flash-size", flash_size,
    ]
    for offset, image in images:
        merge_command.extend((f"0x{offset:x}", str(image)))
    merge_command.extend((f"0x{fixture.offset:x}", str(fixture_image)))
    print("NP2VIDEO_MERGE " + " ".join(shlex.quote(part) for part in merge_command))
    completed = subprocess.run(merge_command, check=False)
    if completed.returncode != 0:
        fail(f"esptool merge_bin failed with status {completed.returncode}")

    try:
        merged = output.read_bytes()
    except OSError as exc:
        fail(f"cannot read merged image: {exc}")
    if len(merged) != 8 * 1024 * 1024:
        fail(f"merged image size is {len(merged)}, expected 8388608")
    extracted = merged[fixture.offset:fixture.offset + image_size]
    merged_digest = hashlib.sha256(extracted).hexdigest()
    print(f"NP2VIDEO_MERGED_SLOT offset=0x{fixture.offset:08x} size={image_size} "
          f"sha256={merged_digest}")
    if merged_digest != expected_digest or extracted != fixture_image.read_bytes():
        fail("merged np2test slot does not match golden image")
    print(f"NP2VIDEO_PARTITION label={fixture.name} offset=0x{fixture.offset:08x} "
          f"size=0x{fixture.size:x} app_size=0x{factory.size:x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
