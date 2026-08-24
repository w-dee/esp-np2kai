#!/usr/bin/env python3
"""Build a temporary 8 MiB flash image with the Step-6A.1 FATFS payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import partition_geometry


EXPECTED_FIXTURE_SIZE = partition_geometry.EXPECTED_NP2TEST_SIZE
EXPECTED_FIXTURE_SHA256 = "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
EXPECTED_NP2KBD_FIXTURE_SIZE = 1_261_568
EXPECTED_NP2KBD_FIXTURE_SHA256 = "65445b14b67b0ff94b5751d05fe87fb7acadfa3a6ce41f60c764ca11c58e7eca"
NP2KBD_FIXTURE_DESTINATION = "fixtures/np2kbdtest-fd1232.hdm"
EXPECTED_FLASH_SIZE = partition_geometry.EXPECTED_FLASH_SIZE
HIGH_ADDRESS_SCAN_START = 0x400000
HIGH_ADDRESS_MARKER = b"STEP6A1-HIGH-ADDRESS-RAW-PROOF-v1"
HIGH_ADDRESS_PREFILL_PATH = "high-address-prefill/filler.bin"
NOSPACE_PREFILL_PATH = "files/upload/prefill.bin"
NOSPACE_PREFILL_BYTE = 0x5A
NOSPACE_REQUEST_BYTES = 1024 * 1024
NOSPACE_METADATA_SCHEMA_VERSION = 1


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def load_partitions(idf_path: Path, table_path: Path):
    try:
        return partition_geometry.load_partition_table(idf_path, table_path)
    except (OSError, ValueError, ImportError) as exc:
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


def verify_np2kbd_fixture(repository_root: Path) -> Path:
    fixture = repository_root / "tests/guest/np2kbdtest/golden/np2kbdtest-fd1232.image"
    if not fixture.is_file():
        fail(f"keyboard fixture is missing: {fixture}")
    size = fixture.stat().st_size
    if size != EXPECTED_NP2KBD_FIXTURE_SIZE:
        fail(f"keyboard fixture size mismatch: {size}")
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    if digest != EXPECTED_NP2KBD_FIXTURE_SHA256:
        fail(f"keyboard fixture SHA-256 mismatch: {digest}")
    print(f"STORAGEFATFS_NP2KBD_GOLDEN size={size} sha256={digest}")
    return fixture


def verify_partition_table(partitions) -> tuple[object, object]:
    try:
        geometry = partition_geometry.extract_geometry(partitions)
    except ValueError as exc:
        fail(str(exc))
    factory = geometry.factory
    fixture = geometry.np2test
    storage = geometry.storage
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
                    nospace_prefill_bytes: int = 0,
                    *, keyboard_fixture: Path | None = None) -> None:
    (source / "files/seed").mkdir(parents=True)
    (source / "files/upload").mkdir(parents=True)
    (source / "files/long").mkdir(parents=True)
    (source / "fixtures").mkdir(parents=True)
    (source / ".np2-staging").mkdir(parents=True)
    (source / "fixtures/np2test-fd1232.hdm").write_bytes(fixture.read_bytes())
    if keyboard_fixture is not None:
        (source / NP2KBD_FIXTURE_DESTINATION).write_bytes(keyboard_fixture.read_bytes())
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
        (source / NOSPACE_PREFILL_PATH).write_bytes(
            bytes([NOSPACE_PREFILL_BYTE]) * nospace_prefill_bytes
        )


def generate_storage_image(generator: Path, source: Path, output: Path,
                           partition_size: int) -> None:
    generator_command = [
        sys.executable,
        str(generator),
        str(source),
        "--output_file",
        str(output),
        "--partition_size",
        hex(partition_size),
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


def write_nospace_metadata(path: Path, metadata: dict[str, int]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(metadata, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        fail(f"cannot write NoSpace metadata {path}: {exc}")


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
    try:
        data = state_image.read_bytes()
    except OSError as exc:
        fail(f"cannot read high-address state image {state_image}: {exc}")
    if len(data) != EXPECTED_FLASH_SIZE:
        fail(
            "high-address state image has the wrong flash envelope: "
            f"size=0x{len(data):x} expected=0x{EXPECTED_FLASH_SIZE:x}"
        )
    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set for high-address geometry verification")
    try:
        partitions = partition_geometry.load_partition_table_from_image(
            Path(idf_path_value).resolve(), data
        )
        geometry = partition_geometry.extract_geometry(partitions)
    except (OSError, ValueError, ImportError) as exc:
        fail(f"cannot derive high-address geometry from merged image: {exc}")
    offsets: list[int] = []
    search_offset = 0
    while True:
        offset = data.find(marker, search_offset)
        if offset < 0:
            break
        offsets.append(offset)
        search_offset = offset + 1
    if len(offsets) != 1:
        fail(
            "high-address marker occurrence count is not exactly one: "
            f"marker={marker_text!r} occurrences={len(offsets)}"
        )
    offset = offsets[0]
    if offset < minimum_offset:
        fail(
            "high-address marker is below the required threshold: "
            f"offset=0x{offset:x} minimum_offset=0x{minimum_offset:x}"
        )
    if not (geometry.storage.offset <= offset and
            offset + len(marker) <= geometry.storage_end):
        fail(
            "high-address marker is outside the storage partition: "
            f"offset=0x{offset:x} marker_end=0x{offset + len(marker):x} "
            f"storage=[0x{geometry.storage.offset:x},0x{geometry.storage_end:x})"
        )
    print(
        "STORAGEFATFS_HIGH_ADDRESS_RAW=PASS "
        f"marker={marker_text} offset=0x{offset:x} minimum_offset=0x{minimum_offset:x} "
        f"storage=[0x{geometry.storage.offset:x},0x{geometry.storage_end:x}) "
        "occurrences=1"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--include-np2kbdtest",
        action="store_true",
        help="include the opt-in deterministic keyboard guest fixture in FATFS",
    )
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
        help="explicit /upload/prefill.bin size for manual NoSpace tests",
    )
    parser.add_argument(
        "--nospace-derived",
        action="store_true",
        help="derive /upload/prefill.bin from the measured clean filesystem",
    )
    parser.add_argument(
        "--metadata-output",
        type=Path,
        help="write derived NoSpace geometry metadata to this JSON path",
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
    if args.nospace_derived and args.nospace_prefill_bytes:
        parser.error("--nospace-derived and --nospace-prefill-bytes are mutually exclusive")
    if args.nospace_derived and args.metadata_output is None:
        parser.error("--metadata-output is required with --nospace-derived")
    if args.metadata_output is not None and not args.nospace_derived:
        parser.error("--metadata-output requires --nospace-derived")
    if args.high_address_prefill_bytes and (args.nospace_prefill_bytes or args.nospace_derived):
        parser.error("high-address and NoSpace prefills are mutually exclusive")

    repository_root = args.repository_root.resolve()
    build_dir = args.build_dir.resolve()
    idf_path_value = __import__("os").environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set")
    idf_path = Path(idf_path_value).resolve()
    fixture = verify_fixture(repository_root)
    keyboard_fixture = (
        verify_np2kbd_fixture(repository_root) if args.include_np2kbdtest else None
    )
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
        generator = idf_path / "components/fatfs/wl_fatfsgen.py"
        base_measurement: dict[str, int | str] | None = None
        nospace_prefill_bytes = args.nospace_prefill_bytes
        nospace_metadata: dict[str, int] | None = None
        if args.nospace_derived:
            base_source = temporary_path / "base-source"
            base_source.mkdir()
            populate_source(base_source, fixture, keyboard_fixture=keyboard_fixture)
            base_image = temporary_path / "base-storage.bin"
            generate_storage_image(generator, base_source, base_image, storage.size)
            base_measurement = measure_fat_image(base_image, storage.size)
            cluster_size = int(base_measurement["cluster_size"])
            required_clusters = (
                NOSPACE_REQUEST_BYTES + cluster_size - 1
            ) // cluster_size
            target_free_clusters = required_clusters - 1
            if required_clusters <= 0 or target_free_clusters < 0:
                fail(
                    "invalid NoSpace geometry: "
                    f"request={NOSPACE_REQUEST_BYTES} cluster_size={cluster_size}"
                )
            prefill_clusters = int(base_measurement["free_clusters"]) - target_free_clusters
            if prefill_clusters <= 0:
                fail(
                    "clean filesystem does not have enough free clusters for NoSpace target: "
                    f"base_free={base_measurement['free_clusters']} "
                    f"target_free={target_free_clusters}"
                )
            nospace_prefill_bytes = prefill_clusters * cluster_size
            nospace_metadata = {
                "schema_version": NOSPACE_METADATA_SCHEMA_VERSION,
                "storage_offset": storage.offset,
                "storage_size": storage.size,
                "storage_end": storage.offset + storage.size,
                "cluster_size": cluster_size,
                "usable_clusters": int(base_measurement["total_data_clusters"]),
                "base_allocated_clusters": int(base_measurement["allocated_clusters"]),
                "base_free_clusters": int(base_measurement["free_clusters"]),
                "nospace_request_bytes": NOSPACE_REQUEST_BYTES,
                "nospace_required_clusters": required_clusters,
                "nospace_target_free_clusters": target_free_clusters,
                "nospace_prefill_clusters": prefill_clusters,
                "nospace_prefill_bytes": nospace_prefill_bytes,
                "nospace_prefill_byte": NOSPACE_PREFILL_BYTE,
            }
            print(
                "STORAGEFATFS_NOSPACE_GEOMETRY "
                + " ".join(f"{key}={value}" for key, value in nospace_metadata.items())
            )

        source = temporary_path / "source"
        source.mkdir()
        populate_source(
            source,
            fixture,
            args.high_address_prefill_bytes,
            nospace_prefill_bytes,
            keyboard_fixture=keyboard_fixture,
        )
        if keyboard_fixture is not None:
            print(
                "STORAGEFATFS_NP2KBD_POPULATED "
                f"path=/{NP2KBD_FIXTURE_DESTINATION} "
                f"size={keyboard_fixture.stat().st_size}"
            )
        if args.high_address_prefill_bytes:
            print(
                "STORAGEFATFS_HIGH_ADDRESS_PREFILL "
                f"bytes={args.high_address_prefill_bytes} "
                f"path={HIGH_ADDRESS_PREFILL_PATH}"
            )
        if nospace_prefill_bytes:
            print(
                "STORAGEFATFS_NOSPACE_PREFILL "
                f"bytes={nospace_prefill_bytes} path={NOSPACE_PREFILL_PATH}"
            )
        storage_image = temporary_path / "storage.bin"
        generate_storage_image(generator, source, storage_image, storage.size)
        measurement = measure_fat_image(storage_image, storage.size)
        if args.nospace_derived:
            assert nospace_metadata is not None
            if (int(measurement["cluster_size"]) != nospace_metadata["cluster_size"] or
                    int(measurement["total_data_clusters"]) != nospace_metadata["usable_clusters"]):
                fail(
                    "NoSpace image changed measured FAT geometry: "
                    f"final_cluster_size={measurement['cluster_size']} "
                    f"base_cluster_size={nospace_metadata['cluster_size']} "
                    f"final_usable_clusters={measurement['total_data_clusters']} "
                    f"base_usable_clusters={nospace_metadata['usable_clusters']}"
                )
            final_free_clusters = int(measurement["free_clusters"])
            target_free_clusters = nospace_metadata["nospace_target_free_clusters"]
            if final_free_clusters != target_free_clusters:
                fail(
                    "NoSpace image did not reach target free clusters: "
                    f"actual={final_free_clusters} expected={target_free_clusters}"
                )
            nospace_metadata["final_allocated_clusters"] = int(measurement["allocated_clusters"])
            nospace_metadata["final_free_clusters"] = final_free_clusters
            write_nospace_metadata(args.metadata_output.resolve(), nospace_metadata)
            print(
                "STORAGEFATFS_NOSPACE_METADATA "
                f"path={args.metadata_output.resolve()} "
                f"final_free_clusters={final_free_clusters}"
            )
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
