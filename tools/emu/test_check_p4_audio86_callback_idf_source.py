#!/usr/bin/env python3
"""Drift mutations for the ESP-IDF v5.5.4 Audio 86 source proof."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/emu/check_p4_audio86_callback_idf_barrier.py"
REQUIRED_FILES = (
    "tools/cmake/version.cmake",
    "components/esp_driver_i2s/i2s_common.c",
    "components/esp_driver_i2s/i2s_std.c",
    "components/esp_hw_support/dma/gdma.c",
    "components/esp_hw_support/intr_alloc.c",
    "components/esp_system/esp_ipc.c",
)


def run_checker(idf_root: Path) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["IDF_PATH"] = str(idf_root)
    return subprocess.run(
        [sys.executable, str(CHECKER)], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def mutate_rejected(path: Path, old: str, new: str, label: str) -> None:
    original = path.read_text(encoding="utf-8")
    if old not in original:
        raise SystemExit(f"{label}: mutation target is missing")
    path.write_text(original.replace(old, new, 1), encoding="utf-8")
    try:
        result = run_checker(temporary_idf_root(path))
        if result.returncode == 0:
            raise SystemExit(f"{label}: source drift was accepted")
    finally:
        path.write_text(original, encoding="utf-8")
    print(f"{label}=PASS")


def temporary_idf_root(path: Path) -> Path:
    for parent in path.parents:
        if (parent / "tools/cmake/version.cmake").is_file():
            return parent
    raise SystemExit(f"cannot locate temporary IDF root for {path}")


def main() -> int:
    source_value = os.environ.get("IDF_PATH")
    if not source_value:
        raise SystemExit("IDF_PATH is required")
    source_root = Path(source_value).resolve()
    with tempfile.TemporaryDirectory() as directory:
        test_root = Path(directory) / "esp-idf"
        for relative in REQUIRED_FILES:
            destination = test_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_root / relative, destination)

        control = run_checker(test_root)
        if control.returncode != 0:
            raise SystemExit(
                "canonical ESP-IDF source was rejected: " + control.stderr)
        print("DRAIN_Q_OVF_TO_EOF_SOURCE_PROOF=PASS")
        print("DRAIN_Q_OVF_TO_EOF_PROOF_VERSION_PINNED=PASS")

        version = test_root / "tools/cmake/version.cmake"
        i2s = test_root / "components/esp_driver_i2s/i2s_common.c"
        mutate_rejected(version, "set(IDF_VERSION_PATCH 4)",
                        "set(IDF_VERSION_PATCH 5)",
                        "DRAIN_Q_OVF_IDF_VERSION_DRIFT_REJECTED")
        mutate_rejected(i2s, "event_data->tx_eof_desc_addr",
                        "event_data->tx_descriptor_addr",
                        "DRAIN_Q_OVF_TX_EOF_SOURCE_DRIFT_REJECTED")
        mutate_rejected(
            i2s,
            "handle->callbacks.on_sent(handle, &evt, handle->user_data)",
            "handle->callbacks.on_sent_after_qovf(handle, &evt, "
            "handle->user_data)",
            "DRAIN_Q_OVF_ON_SENT_ORDER_DRIFT_REJECTED")
        qovf = "handle->callbacks.on_send_q_ovf(handle, &evt, handle->user_data)"
        mutate_rejected(i2s, qovf, qovf + ";\n            " + qovf,
                        "DRAIN_Q_OVF_MULTIPLE_PER_EOF_DRIFT_REJECTED")

    print("DRAIN_Q_OVF_TO_EOF_PROOF_DRIFT_FAIL_CLOSED=PASS")
    print("DRAIN_Q_OVF_IDF_SOURCE_CHECK_CAUSAL=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
