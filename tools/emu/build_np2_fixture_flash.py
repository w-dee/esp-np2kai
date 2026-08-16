#!/usr/bin/env python3
"""Verify the formal NP2TEST image and merge it into an ESP-IDF flash image."""

from __future__ import annotations

import argparse
import hashlib
import shlex
import subprocess
import sys
from pathlib import Path

EXPECTED_SIZE = 0x134000
EXPECTED_SHA256 = "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
PARTITION_TYPE = 0x40
PARTITION_SUBTYPE = 0x01


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def load_partition_table(idf_path: Path, table_path: Path):
    sys.path.insert(0, str(idf_path / "components" / "partition_table"))
    import gen_esp32part  # type: ignore

    try:
        return gen_esp32part.PartitionTable.from_binary(table_path.read_bytes())
    except (OSError, ValueError) as exc:
        fail(f"cannot parse partition table {table_path}: {exc}")


def read_flash_args(build_dir: Path) -> tuple[list[str], list[tuple[int, Path]]]:
    flash_args = build_dir / "flash_args"
    try:
        lines = flash_args.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        fail(f"cannot read ESP-IDF flash arguments: {exc}")
    if not lines:
        fail(f"empty ESP-IDF flash arguments: {flash_args}")

    options = shlex.split(lines[0])
    images: list[tuple[int, Path]] = []
    for line in lines[1:]:
        fields = shlex.split(line)
        if len(fields) != 2:
            fail(f"invalid flash argument line: {line}")
        try:
            offset = int(fields[0], 0)
        except ValueError as exc:
            fail(f"invalid flash offset in {line}: {exc}")
        images.append((offset, build_dir / fields[1]))
    if not images:
        fail("ESP-IDF flash arguments contain no images")
    return options, images


def verify_golden(repository_root: Path) -> tuple[Path, str]:
    guest_tools = repository_root / "tools" / "guest"
    sys.path.insert(0, str(guest_tools))
    try:
        from verify_np2test import verify  # type: ignore
    except ImportError as exc:
        fail(f"cannot import the existing NP2TEST verifier: {exc}")

    image = repository_root / "tests/guest/np2test/golden/np2test-fd1232.image"
    layout = repository_root / "tests/guest/np2test/layout.json"
    checksums = image.parent / "SHA256SUMS"
    try:
        digest = verify(layout, image, expected_sha256=EXPECTED_SHA256)
    except (OSError, ValueError) as exc:
        fail(f"formal NP2TEST fixture verification failed: {exc}")
    try:
        checksum_fields = checksums.read_text(encoding="ascii").strip().split()
    except (OSError, UnicodeDecodeError) as exc:
        fail(f"cannot read the tracked fixture checksum file: {exc}")
    if not checksum_fields or checksum_fields[0].lower() != digest:
        fail("tracked SHA256SUMS does not match the verified fixture")
    if image.stat().st_size != EXPECTED_SIZE:
        fail(f"fixture size is not 0x{EXPECTED_SIZE:x}: {image.stat().st_size}")
    print(f"NP2FIXTURE_GOLDEN size={EXPECTED_SIZE} sha256={digest}")
    return image, digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    repository_root = args.repository_root.resolve()
    build_dir = args.build_dir.resolve()
    idf_path_value = __import__("os").environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set")
    idf_path = Path(idf_path_value).resolve()

    golden, golden_digest = verify_golden(repository_root)
    table_path = build_dir / "partition_table" / "partition-table.bin"
    partitions = load_partition_table(idf_path, table_path)
    fixture = partitions.find_by_name("np2test")
    factory = partitions.find_by_name("factory")
    if fixture is None:
        fail("partition table has no np2test partition")
    if factory is None:
        fail("partition table has no factory application partition")
    if (fixture.type, fixture.subtype) != (PARTITION_TYPE, PARTITION_SUBTYPE):
        fail(f"np2test type/subtype is 0x{fixture.type:02x}/0x{fixture.subtype:02x}")
    if not fixture.readonly:
        fail("np2test partition is not flagged readonly")
    if fixture.size < EXPECTED_SIZE:
        fail(f"np2test partition is too small: 0x{fixture.size:x}")
    if fixture.size != EXPECTED_SIZE:
        fail(f"np2test partition must remain exact-size: 0x{fixture.size:x}")

    options, images = read_flash_args(build_dir)
    for _, image in images:
        if not image.is_file():
            fail(f"ESP-IDF flash image is missing: {image}")

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    esptool = idf_path / "components/esptool_py/esptool/esptool.py"
    merge_command = [
        sys.executable,
        str(esptool),
        "--chip",
        "esp32p4",
        "merge_bin",
        "--output",
        str(output),
        "--format",
        "raw",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "4MB",
        "--fill-flash-size",
        "4MB",
    ]
    for offset, image in images:
        merge_command.extend((f"0x{offset:x}", str(image)))
    merge_command.extend((f"0x{fixture.offset:x}", str(golden)))
    print("NP2FIXTURE_MERGE " + " ".join(shlex.quote(part) for part in merge_command))
    completed = subprocess.run(merge_command, check=False)
    if completed.returncode != 0:
        fail(f"esptool merge_bin failed with status {completed.returncode}")

    try:
        merged = output.read_bytes()
    except OSError as exc:
        fail(f"cannot read merged image {output}: {exc}")
    end = fixture.offset + EXPECTED_SIZE
    if len(merged) < end:
        fail(f"merged image ends before np2test partition: {len(merged):#x} < {end:#x}")
    extracted = merged[fixture.offset:end]
    extracted_digest = hashlib.sha256(extracted).hexdigest()
    if extracted_digest != golden_digest or extracted != golden.read_bytes():
        fail("merged np2test payload does not match the tracked golden bytes")

    print(
        "NP2FIXTURE_PARTITION "
        f"type=0x{fixture.type:02x} subtype=0x{fixture.subtype:02x} "
        f"label={fixture.name} offset=0x{fixture.offset:08x} "
        f"size=0x{fixture.size:x} app_size=0x{factory.size:x}"
    )
    print(
        "NP2FIXTURE_MERGED "
        f"path={output} size={len(merged)} "
        f"payload_sha256={extracted_digest} "
        f"payload_first16={extracted[:16].hex()} "
        f"payload_last16={extracted[-16:].hex()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
