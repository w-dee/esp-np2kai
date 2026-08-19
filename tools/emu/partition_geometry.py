#!/usr/bin/env python3
"""Extract and validate the approved ESP32-P4 partition geometry."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys


PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
EXPECTED_FLASH_SIZE = 0x800000
EXPECTED_FACTORY_OFFSET = 0x10000
EXPECTED_FACTORY_SIZE = 0x200000
EXPECTED_NP2TEST_SIZE = 0x134000
EXPECTED_STORAGE_SIZE = 0x4BC000
EXPECTED_NP2TEST_TYPE = 0x40
EXPECTED_NP2TEST_SUBTYPE = 0x01
EXPECTED_STORAGE_TYPE = 0x01
EXPECTED_STORAGE_SUBTYPE = 0x81


@dataclass(frozen=True)
class PartitionGeometry:
    factory: object
    np2test: object
    storage: object
    flash_size: int = EXPECTED_FLASH_SIZE

    @property
    def np2test_end(self) -> int:
        return self.np2test.offset + self.np2test.size

    @property
    def storage_end(self) -> int:
        return self.storage.offset + self.storage.size


def _partition_table_module(idf_path: Path):
    partition_table_path = idf_path / "components" / "partition_table"
    if str(partition_table_path) not in sys.path:
        sys.path.insert(0, str(partition_table_path))
    import gen_esp32part  # type: ignore

    return gen_esp32part


def load_partition_table(idf_path: Path, table_path: Path):
    try:
        table_data = table_path.read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read partition table {table_path}: {exc}") from exc
    return _partition_table_module(idf_path).PartitionTable.from_binary(table_data)


def load_partition_table_from_image(idf_path: Path, image: bytes):
    table_end = PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE
    if len(image) < table_end:
        raise ValueError(
            f"flash image is too short for the partition table: "
            f"size=0x{len(image):x} end=0x{table_end:x}"
        )
    return _partition_table_module(idf_path).PartitionTable.from_binary(
        image[PARTITION_TABLE_OFFSET:table_end]
    )


def _require_partition(partitions, name: str):
    matches = [partition for partition in partitions if partition.name == name]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {name} partition, found {len(matches)}")
    return matches[0]


def extract_geometry(partitions) -> PartitionGeometry:
    factory = _require_partition(partitions, "factory")
    np2test = _require_partition(partitions, "np2test")
    storage = _require_partition(partitions, "storage")
    if (factory.offset, factory.size) != (
            EXPECTED_FACTORY_OFFSET, EXPECTED_FACTORY_SIZE):
        raise ValueError(
            f"factory changed: offset=0x{factory.offset:x} size=0x{factory.size:x}"
        )
    if (np2test.offset, np2test.size, np2test.type, np2test.subtype,
            np2test.readonly) != (
                EXPECTED_FACTORY_OFFSET + EXPECTED_FACTORY_SIZE,
                EXPECTED_NP2TEST_SIZE,
                EXPECTED_NP2TEST_TYPE,
                EXPECTED_NP2TEST_SUBTYPE,
                True,
            ):
        raise ValueError(
            "np2test partition changed: "
            f"offset=0x{np2test.offset:x} size=0x{np2test.size:x} "
            f"type=0x{np2test.type:x} subtype=0x{np2test.subtype:x} "
            f"readonly={np2test.readonly}"
        )
    if (storage.offset, storage.size, storage.type, storage.subtype) != (
            EXPECTED_FACTORY_OFFSET + EXPECTED_FACTORY_SIZE + EXPECTED_NP2TEST_SIZE,
            EXPECTED_STORAGE_SIZE,
            EXPECTED_STORAGE_TYPE,
            EXPECTED_STORAGE_SUBTYPE,
    ):
        raise ValueError(
            "storage partition changed: "
            f"offset=0x{storage.offset:x} size=0x{storage.size:x} "
            f"type=0x{storage.type:x} subtype=0x{storage.subtype:x}"
        )
    if storage.offset + storage.size != EXPECTED_FLASH_SIZE:
        raise ValueError(
            "storage partition does not end at flash envelope: "
            f"end=0x{storage.offset + storage.size:x} "
            f"expected=0x{EXPECTED_FLASH_SIZE:x}"
        )
    if any(partition.size <= 0 for partition in partitions if partition.offset is not None):
        raise ValueError("partition table contains a non-positive partition size")
    if (np2test.offset + np2test.size > EXPECTED_FLASH_SIZE or
            storage.offset + storage.size > EXPECTED_FLASH_SIZE):
        raise ValueError("partition geometry exceeds the flash envelope")
    if (storage.offset % 0x1000 or storage.size % 0x1000 or
            np2test.offset % 0x1000 or np2test.size % 0x1000):
        raise ValueError("geometry-sensitive partitions are not 0x1000 aligned")
    ranges = sorted(
        (partition.offset, partition.offset + partition.size, partition.name)
        for partition in partitions
        if partition.offset is not None
    )
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise ValueError(f"partition overlap: {previous} and {current}")
    return PartitionGeometry(factory, np2test, storage)


def geometry_lines(geometry: PartitionGeometry) -> list[str]:
    return [
        f"FACTORY_OFFSET={geometry.factory.offset}",
        f"FACTORY_SIZE={geometry.factory.size}",
        f"NP2TEST_OFFSET={geometry.np2test.offset}",
        f"NP2TEST_SIZE={geometry.np2test.size}",
        f"NP2TEST_END={geometry.np2test_end}",
        f"STORAGE_OFFSET={geometry.storage.offset}",
        f"STORAGE_SIZE={geometry.storage.size}",
        f"STORAGE_END={geometry.storage_end}",
        f"FLASH_SIZE={geometry.flash_size}",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--partition-table", type=Path, required=True)
    args = parser.parse_args()
    try:
        partitions = load_partition_table(args.idf_path.resolve(), args.partition_table.resolve())
        geometry = extract_geometry(partitions)
    except (OSError, ValueError, ImportError) as exc:
        parser.error(str(exc))
    print("\n".join(geometry_lines(geometry)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
