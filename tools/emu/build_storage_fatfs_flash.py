#!/usr/bin/env python3
"""Build a temporary 8 MiB flash image with the Step-6A.1 FATFS payload."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED_FIXTURE_SIZE = 0x134000
EXPECTED_FIXTURE_SHA256 = "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
EXPECTED_FACTORY_OFFSET = 0x10000
EXPECTED_FACTORY_SIZE = 0x200000
EXPECTED_FLASH_SIZE = 0x800000
HIGH_ADDRESS_SCAN_START = 0x400000
HIGH_ADDRESS_MARKER = b"STEP6A1-HIGH-ADDRESS-RAW-PROOF-v1"
HIGH_ADDRESS_PREFILL_PATH = "high-address-prefill/filler.bin"
NOSPACE_PREFILL_PATH = "files/upload/prefill.bin"


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def load_partitions(idf_path: Path, table_path: Path):
    sys.path.insert(0, str(idf_path / "components" / "partition_table"))
    import gen_esp32part  # type: ignore

    try:
        return gen_esp32part.PartitionTable.from_binary(table_path.read_bytes())
    except (OSError, ValueError) as exc:
        fail(f"cannot parse partition table {table_path}: {exc}")


def read_flash_images(build_dir: Path) -> tuple[list[str], list[tuple[int, Path]]]:
    lines = (build_dir / "flash_args").read_text(encoding="utf-8").splitlines()
    if not lines:
        fail("ESP-IDF flash_args is empty")
    options = lines[0].split()
    images: list[tuple[int, Path]] = []
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 2:
            fail(f"invalid flash_args line: {line}")
        images.append((int(fields[0], 0), build_dir / fields[1]))
    return options, images


def validate_flash_ranges(
    images: list[tuple[int, Path]],
    overlays: list[tuple[int, int, str]],
    flash_size: int,
) -> None:
    ranges: list[tuple[int, int, str]] = []
    for offset, image in images:
        size = image.stat().st_size
        ranges.append((offset, offset + size, image.name))
    ranges.extend((offset, offset + size, label) for offset, size, label in overlays)
    for start, end, label in ranges:
        if start < 0 or end > flash_size:
            fail(
                f"flash segment outside envelope: {label} "
                f"[0x{start:x},0x{end:x}) flash=0x{flash_size:x}"
            )
    for previous, current in zip(sorted(ranges), sorted(ranges)[1:]):
        if previous[1] > current[0]:
            fail(
                f"flash segment overlap: {previous[2]} "
                f"[0x{previous[0]:x},0x{previous[1]:x}) and {current[2]} "
                f"[0x{current[0]:x},0x{current[1]:x})"
            )


def verify_fixture(repository_root: Path) -> Path:
    fixture = repository_root / "tests/guest/np2test/golden/np2test-fd1232.image"
    if fixture.stat().st_size != EXPECTED_FIXTURE_SIZE:
        fail(f"fixture size mismatch: {fixture.stat().st_size}")
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    if digest != EXPECTED_FIXTURE_SHA256:
        fail(f"fixture SHA-256 mismatch: {digest}")
    print(f"STORAGEFATFS_GOLDEN size={fixture.stat().st_size} sha256={digest}")
    return fixture


def require_partition(partitions, name: str):
    matches = [partition for partition in partitions if partition.name == name]
    if len(matches) != 1:
        fail(f"expected exactly one {name} partition, found {len(matches)}")
    return matches[0]


def verify_partition_table(partitions) -> tuple[object, object]:
    factory = require_partition(partitions, "factory")
    fixture = require_partition(partitions, "np2test")
    storage = require_partition(partitions, "storage")
    if (factory.offset, factory.size) != (EXPECTED_FACTORY_OFFSET, EXPECTED_FACTORY_SIZE):
        fail(f"factory changed: offset=0x{factory.offset:x} size=0x{factory.size:x}")
    if (fixture.offset, fixture.size, fixture.type, fixture.subtype, fixture.readonly) != (
        factory.offset + factory.size, EXPECTED_FIXTURE_SIZE, 0x40, 0x01, True
    ):
        fail("np2test partition changed")
    if (storage.offset, storage.type, storage.subtype) != (
        fixture.offset + fixture.size, 1, 0x81
    ):
        fail(
            "storage partition mismatch: "
            f"offset=0x{storage.offset:x} size=0x{storage.size:x} "
            f"type=0x{storage.type:x} subtype=0x{storage.subtype:x}"
        )
    if (storage.offset + storage.size) != EXPECTED_FLASH_SIZE:
        fail(
            "storage partition does not end at flash envelope: "
            f"end=0x{storage.offset + storage.size:x} expected=0x{EXPECTED_FLASH_SIZE:x}"
        )
    if storage.offset % 0x1000 or storage.size % 0x1000:
        fail("storage partition is not 0x1000 aligned")
    ranges = sorted(
        (partition.offset, partition.offset + partition.size, partition.name)
        for partition in partitions
        if partition.offset is not None
    )
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            fail(f"partition overlap: {previous} and {current}")
    print(
        "STORAGEFATFS_PARTITIONS "
        f"factory=0x{factory.offset:x}:0x{factory.size:x} "
        f"np2test=0x{fixture.offset:x}:0x{fixture.size:x} "
        f"storage=0x{storage.offset:x}:0x{storage.size:x} "
        f"storage_end=0x{storage.offset + storage.size:x}"
    )
    return fixture, storage


def populate_source(source: Path, fixture: Path,
                    high_address_prefill_bytes: int = 0,
                    nospace_prefill_bytes: int = 0) -> None:
    (source / "files/seed").mkdir(parents=True)
    (source / "files/upload").mkdir(parents=True)
    (source / "files/long").mkdir(parents=True)
    (source / "fixtures").mkdir(parents=True)
    (source / ".np2-staging").mkdir(parents=True)
    (source / "fixtures/np2test-fd1232.hdm").write_bytes(fixture.read_bytes())
    (source / "files/seed/existing.bin").write_bytes(bytes(range(0xA0, 0xA0 + 37)))
    (source / "files/seed/page-00.bin").write_bytes(b"step6a1-seed\n")
    (source / "files/upload/.keep").write_bytes(b"keep\n")
    (source / "files/long/utf8-long.txt").write_bytes(b"utf8\n")
    (source / "files/long/long-name-abcdefghijklmnopqrstuvwxyz.txt").write_bytes(b"long\n")
    (source / ".np2-staging/.keep").write_bytes(b"staging\n")
    if high_address_prefill_bytes:
        prefill = source / HIGH_ADDRESS_PREFILL_PATH
        prefill.parent.mkdir(parents=True)
        # Keep the filler outside /persist/files and use a byte pattern that
        # cannot contain either Step-6A high-address marker.
        prefill.write_bytes(b"\xa5" * high_address_prefill_bytes)
    if nospace_prefill_bytes:
        (source / NOSPACE_PREFILL_PATH).write_bytes(b"\x5a" * nospace_prefill_bytes)


def measure_fat_image(image: Path, partition_size: int) -> dict[str, int | str]:
    data = image.read_bytes()
    if len(data) != partition_size:
        fail(
            f"generated storage image size is {len(data)} bytes, "
            f"expected parsed storage partition size {partition_size}"
        )
    boot_offset = 0x1000
    boot = data[boot_offset:]
    sector_size = struct.unpack_from("<H", boot, 11)[0]
    sectors_per_cluster = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_count = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    total_sectors = struct.unpack_from("<H", boot, 19)[0] or struct.unpack_from("<I", boot, 32)[0]
    fat_sectors = struct.unpack_from("<H", boot, 22)[0] or struct.unpack_from("<I", boot, 36)[0]
    root_sectors = (root_entries * 32 + sector_size - 1) // sector_size
    data_sectors = total_sectors - reserved - fat_count * fat_sectors - root_sectors
    total_data_clusters = data_sectors // sectors_per_cluster
    fat_start = boot_offset + reserved * sector_size
    fat = data[fat_start : fat_start + fat_sectors * sector_size]

    def fat12(cluster: int) -> int:
        offset = cluster + cluster // 2
        value = fat[offset] | (fat[offset + 1] << 8)
        return value >> 4 if cluster & 1 else value & 0xFFF

    allocated = sum(
        fat12(cluster) != 0
        for cluster in range(2, 2 + total_data_clusters)
    )
    free = total_data_clusters - allocated
    wl_total_sectors = len(data) // 4096
    wl_state_sectors = (64 + 16 * wl_total_sectors + 4095) // 4096
    wl_metadata_bytes = (1 + 1 + 2 * wl_state_sectors) * 4096
    fat_volume = total_sectors * sector_size
    return {
        "raw_bytes": len(data),
        "storage_partition_bytes": partition_size,
        "wl_sector_count": wl_total_sectors,
        "wl_state_sectors": wl_state_sectors,
        "wl_metadata_bytes": wl_metadata_bytes,
        "wl_overhead": wl_metadata_bytes,
        "fat_volume_bytes": fat_volume,
        "plain_fat_volume_bytes": fat_volume,
        "fat_type": "FAT12" if total_data_clusters < 4085 else "FAT16",
        "sector_size": sector_size,
        "sectors_per_cluster": sectors_per_cluster,
        "cluster_size": sector_size * sectors_per_cluster,
        "fat_count": fat_count,
        "fat_sectors": fat_sectors,
        "root_dir_sectors": root_sectors,
        "total_data_clusters": total_data_clusters,
        "cluster_count": total_data_clusters,
        "allocated_clusters": allocated,
        "free_clusters": free,
        "used_bytes": allocated * sector_size * sectors_per_cluster,
        "free_bytes": free * sector_size * sectors_per_cluster,
    }


def verify_high_address_state(state_image: Path, marker_text: str,
                              minimum_offset: int) -> None:
    try:
        marker = marker_text.encode("ascii")
    except UnicodeEncodeError as exc:
        fail(f"high-address marker must be ASCII: {exc}")
    if not marker:
        fail("high-address marker must not be empty")
    data = state_image.read_bytes()
    offset = data.find(marker, minimum_offset)
    if offset < 0:
        fail(
            "high-address marker not found: "
            f"marker={marker_text!r} minimum_offset=0x{minimum_offset:x}"
        )
    print(
        "STORAGEFATFS_HIGH_ADDRESS_RAW=PASS "
        f"marker={marker_text} offset=0x{offset:x} minimum_offset=0x{minimum_offset:x}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-state", type=Path)
    parser.add_argument(
        "--high-address-prefill-bytes",
        type=int,
        default=0,
        help="temporary private FATFS filler size for bounded high-address tests",
    )
    parser.add_argument(
        "--nospace-prefill-bytes",
        type=int,
        default=0,
        help="temporary /upload/prefill.bin size for bounded NoSpace tests",
    )
    parser.add_argument("--marker", default=HIGH_ADDRESS_MARKER.decode("ascii"))
    parser.add_argument(
        "--minimum-offset",
        type=lambda value: int(value, 0),
        default=HIGH_ADDRESS_SCAN_START,
        help="minimum physical marker offset (accepts decimal or 0x-prefixed hex)",
    )
    args = parser.parse_args()
    if args.high_address_prefill_bytes < 0:
        parser.error("--high-address-prefill-bytes must be non-negative")
    if args.nospace_prefill_bytes < 0:
        parser.error("--nospace-prefill-bytes must be non-negative")
    if args.high_address_prefill_bytes and args.nospace_prefill_bytes:
        parser.error("high-address and NoSpace prefills are mutually exclusive")

    repository_root = args.repository_root.resolve()
    build_dir = args.build_dir.resolve()
    idf_path_value = __import__("os").environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set")
    idf_path = Path(idf_path_value).resolve()
    fixture = verify_fixture(repository_root)
    partitions = load_partitions(idf_path, build_dir / "partition_table/partition-table.bin")
    fixture_partition, storage = verify_partition_table(partitions)
    options, images = read_flash_images(build_dir)
    if "--flash_size" not in options or options[options.index("--flash_size") + 1] != "8MB":
        fail(f"ESP-IDF flash arguments are not 8MB: {' '.join(options)}")
    for _, image in images:
        if not image.is_file():
            fail(f"missing flash image: {image}")
    validate_flash_ranges(
        images,
        [
            (storage.offset, storage.size, "storage partition"),
            (fixture_partition.offset, fixture_partition.size, "np2test partition"),
        ],
        EXPECTED_FLASH_SIZE,
    )

    with tempfile.TemporaryDirectory(prefix="step6a1-fatfs-") as temporary:
        temporary_path = Path(temporary)
        source = temporary_path / "source"
        source.mkdir()
        populate_source(
            source,
            fixture,
            args.high_address_prefill_bytes,
            args.nospace_prefill_bytes,
        )
        if args.high_address_prefill_bytes:
            print(
                "STORAGEFATFS_HIGH_ADDRESS_PREFILL "
                f"bytes={args.high_address_prefill_bytes} "
                f"path={HIGH_ADDRESS_PREFILL_PATH}"
            )
        if args.nospace_prefill_bytes:
            print(
                "STORAGEFATFS_NOSPACE_PREFILL "
                f"bytes={args.nospace_prefill_bytes} path={NOSPACE_PREFILL_PATH}"
            )
        storage_image = temporary_path / "storage.bin"
        generator = idf_path / "components/fatfs/wl_fatfsgen.py"
        generator_command = [
            sys.executable,
            str(generator),
            str(source),
            "--output_file",
            str(storage_image),
            "--partition_size",
            hex(storage.size),
            "--sector_size",
            "4096",
            "--long_name_support",
            "--use_default_datetime",
            "--fat_count",
            "2",
        ]
        completed = subprocess.run(generator_command, check=False)
        if completed.returncode != 0:
            fail(f"wl_fatfsgen failed with status {completed.returncode}")
        measurement = measure_fat_image(storage_image, storage.size)
        print("STORAGEFATFS_IMAGE " + " ".join(f"{key}={value}" for key, value in measurement.items()))

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
            *options,
            "--fill-flash-size",
            "8MB",
        ]
        for offset, image in images:
            merge_command.extend((f"0x{offset:x}", str(image)))
        merge_command.extend((f"0x{storage.offset:x}", str(storage_image)))
        merge_command.extend((f"0x{fixture_partition.offset:x}", str(fixture)))
        print("STORAGEFATFS_MERGE " + " ".join(merge_command))
        completed = subprocess.run(merge_command, check=False)
        if completed.returncode != 0:
            fail(f"esptool merge_bin failed with status {completed.returncode}")
        if output.stat().st_size != EXPECTED_FLASH_SIZE:
            fail(f"merged flash size is not 8 MiB: {output.stat().st_size}")
        print(f"STORAGEFATFS_MERGED path={output} size={output.stat().st_size}")
    if args.verify_state is not None:
        verify_high_address_state(
            args.verify_state.resolve(), args.marker, args.minimum_offset
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
