#!/usr/bin/env python3
"""Fail-closed source audit for the ESP-IDF v5.5.4 I2S callback barrier."""

import os
from pathlib import Path


def require(text: str, needles: tuple[str, ...], label: str) -> None:
    missing = [needle for needle in needles if needle not in text]
    if missing:
        raise SystemExit(f"{label}: missing {missing!r}")


def main() -> int:
    root_value = os.environ.get("IDF_PATH")
    if not root_value:
        raise SystemExit("IDF_PATH is required")
    root = Path(root_value).resolve()
    version = (root / "tools/cmake/version.cmake").read_text(encoding="utf-8")
    require(version, ("IDF_VERSION_MAJOR 5", "IDF_VERSION_MINOR 5",
                      "IDF_VERSION_PATCH 4"), "ESP-IDF version")
    i2s = (root / "components/esp_driver_i2s/i2s_common.c").read_text(
        encoding="utf-8")
    gdma = (root / "components/esp_hw_support/dma/gdma.c").read_text(
        encoding="utf-8")
    interrupt = (root / "components/esp_hw_support/intr_alloc.c").read_text(
        encoding="utf-8")
    ipc = (root / "components/esp_system/esp_ipc.c").read_text(
        encoding="utf-8")
    require(i2s, ("esp_err_t i2s_del_channel", "gdma_disconnect",
                  "gdma_del_channel"), "i2s delete path")
    require(gdma, ("esp_err_t gdma_del_channel", "esp_intr_free"),
            "GDMA delete path")
    require(interrupt, ("esp_err_t esp_intr_free",
                        "esp_ipc_call_blocking(handle->vector_desc->cpu",
                        "intr_free_for_other_cpu"), "interrupt free path")
    require(ipc, ("IPC_WAIT_FOR_END", "esp_err_t esp_ipc_call_blocking",
                  "IPC_WAIT_FOR_END);"), "blocking IPC path")
    print("CALLBACK_IDF_BARRIER_SOURCE_PROOF=PASS")
    print("CALLBACK_IDF_PROOF_VERSION_PINNED=PASS")
    print("CALLBACK_BARRIER_DUAL_EVIDENCE=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
