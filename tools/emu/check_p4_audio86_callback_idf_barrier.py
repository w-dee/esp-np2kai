#!/usr/bin/env python3
"""Fail-closed source audit for the ESP-IDF v5.5.4 I2S callback barrier."""

import os
import re
from pathlib import Path


def require(text: str, needles: tuple[str, ...], label: str) -> None:
    missing = [needle for needle in needles if needle not in text]
    if missing:
        raise SystemExit(f"{label}: missing {missing!r}")


def require_condition(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def extract_braced(text: str, opening: int) -> str:
    depth = 0
    for offset in range(opening, len(text)):
        if text[offset] == "{":
            depth += 1
        elif text[offset] == "}":
            depth -= 1
            if depth == 0:
                return text[opening:offset + 1]
    raise SystemExit("unterminated ESP-IDF function/block")


def tx_callback_bodies(i2s: str) -> list[str]:
    signature = re.compile(
        r"static\s+(?:bool|void)\s+IRAM_ATTR\s+"
        r"i2s_dma_tx_callback\s*\([^)]*\)\s*\{")
    return [extract_braced(i2s, match.end() - 1)
            for match in signature.finditer(i2s)]


def check_drain_qovf_to_eof(i2s: str) -> None:
    sent_call = "handle->callbacks.on_sent(handle, &evt, handle->user_data)"
    full_test = "if (xQueueIsQueueFullFromISR(handle->msg_queue))"
    qovf_call = (
        "handle->callbacks.on_send_q_ovf(handle, &evt, handle->user_data)")
    callbacks = tx_callback_bodies(i2s)
    require_condition(len(callbacks) == 2,
                      "ESP-IDF TX DMA callback variants drifted")
    require_condition(i2s.count(qovf_call) == len(callbacks),
                      "on_send_q_ovf gained a non-TX-DMA-EOF call site")
    eof_sources = (
        "event_data->tx_eof_desc_addr",
        "status & I2S_LL_EVENT_TX_EOF",
    )
    for index, callback in enumerate(callbacks):
        require_condition(callback.count(sent_call) == 1,
                          f"TX callback {index}: on_sent call count drifted")
        require_condition(callback.count(full_test) == 1,
                          f"TX callback {index}: queue-full test count drifted")
        require_condition(callback.count(qovf_call) == 1,
                          f"TX callback {index}: q_ovf call count drifted")
        require_condition(eof_sources[index] in callback,
                          f"TX callback {index}: EOF source drifted")
        require_condition(callback.index(sent_call) < callback.index(full_test) <
                          callback.index(qovf_call),
                          f"TX callback {index}: on_sent/q_ovf order drifted")
        full_open = callback.index("{", callback.index(full_test))
        full_block = extract_braced(callback, full_open)
        require_condition(qovf_call in full_block,
                          f"TX callback {index}: q_ovf escaped full-queue block")
        require_condition(re.search(r"\b(?:for|while)\s*\(", callback) is None,
                          f"TX callback {index}: loop invalidates one-event bound")


def check_exact_idf_version(version: str) -> None:
    expected = {"MAJOR": "5", "MINOR": "5", "PATCH": "4"}
    for component, value in expected.items():
        matches = re.findall(
            rf"^set\(IDF_VERSION_{component}\s+([0-9]+)\)$",
            version, re.MULTILINE)
        require_condition(matches == [value],
                          "ESP-IDF version is not exactly pinned to v5.5.4")


def main() -> int:
    root_value = os.environ.get("IDF_PATH")
    if not root_value:
        raise SystemExit("IDF_PATH is required")
    root = Path(root_value).resolve()
    version = (root / "tools/cmake/version.cmake").read_text(encoding="utf-8")
    check_exact_idf_version(version)
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
    check_drain_qovf_to_eof(i2s)
    print(
        "5D1_STATIC_EVIDENCE schema=1 property_id=idf_delete_barrier "
        "evidence_class=STATIC_IDF_SOURCE "
        "fields=version|i2s_del_channel|gdma_del_channel|esp_intr_free|blocking_ipc "
        "predicate=PASS")
    print(
        "5D1_STATIC_EVIDENCE schema=1 "
        "property_id=idf_drain_qovf_eof_order "
        "evidence_class=STATIC_IDF_SOURCE "
        "fields=version|tx_dma_eof|on_sent_before_q_ovf|one_q_ovf_per_eof "
        "predicate=PASS")
    print(
        "5D1_NON_ACCEPTANCE_SUMMARY schema=1 evidence_class=STATIC_IDF_SOURCE "
        "name=CALLBACK_BARRIER_SOURCE_PROOF value=COMPLETE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
