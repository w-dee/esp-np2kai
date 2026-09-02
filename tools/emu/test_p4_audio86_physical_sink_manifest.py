#!/usr/bin/env python3
"""Validate the distinct 86R.5D.1 acceptance-property manifest."""

from pathlib import Path


MANIFEST = Path(__file__).with_name(
    "p4_audio86_physical_sink_acceptance_manifest.tsv")


def main() -> int:
    rows = []
    for number, line in enumerate(MANIFEST.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise SystemExit(f"manifest line {number}: expected 3 nonempty fields")
        rows.append(fields)
    identifiers = [row[0] for row in rows]
    if len(identifiers) != len(set(identifiers)):
        raise SystemExit("manifest contains duplicate property identifiers")
    if len(rows) < 68:
        raise SystemExit(f"manifest too small: {len(rows)}")
    print(f"5D1_ACCEPTANCE_MANIFEST={len(rows)}_DISTINCT_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
