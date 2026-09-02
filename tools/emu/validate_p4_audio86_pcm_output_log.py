#!/usr/bin/env python3
"""Fail-closed validator for the 86R.5C.2 virtual PCM consumer profile."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from validate_p4_audio86_real_guest_log import FATAL_PATTERNS, one, require, validate


FULL_SHA = "176ea419f153382039e143163ff8476c5461abfddd055cd801003ef89c04a18a"
PRE_SHA = "d51e85a3e8d63ecd763988f02521ef38e754f914ad84fc728375c8d84b8bf9a7"


def fields(text: str, prefix: str) -> dict[str, str]:
    matches = re.findall(rf"(?m)^{re.escape(prefix)}([^\r\n]+)$", text)
    require(len(matches) == 1, f"{prefix}: expected one marker")
    return dict(item.split("=", 1) for item in matches[0].split())


def validate_pcm(text: str, partial: bool = False) -> None:
    if not partial:
        validate(text)
    else:
        for pattern in FATAL_PATTERNS:
            require(pattern.search(text) is None, f"raw fatal signature: {pattern.pattern}")
        require("ESP-ROM:esp32p4" in text, "boot ROM marker missing")
        require("main_task: Returned from app_main()" in text, "app_main completion missing")
        require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS", "guest result")
        require(one(text, "P4_NANO_AUDIO86_REAL_GUEST_STATUS") == "PASS", "main status")

    profile = fields(text, "P4_AUDIO86_PCM_OUTPUT ")
    require(profile == {
        "profile": "1", "producer_core": "0", "producer_priority": "6",
        "consumer_core": "0", "consumer_priority": "7", "consumer_index": "0",
        "prefill": "4", "ring_capacity": "8", "ring_quantum": "240",
        "ring_bytes": "7832", "consumer_stack": "4096", "internal": "1",
        "psram_fallback": "NO", "i2s_active": "0",
        "physical_timing_validated": "0",
    }, "PCM topology/geometry marker mismatch")
    completion = fields(text, "P4_AUDIO86_PCM_COMPLETION ")
    require(completion == {
        "ring_finished": "1", "pcm_done": "1", "worker_quiescent": "1",
        "consumer_ack": "1", "consumer_quiescent": "1", "sink_started": "1",
        "sink_finished": "1", "ring_before_done": "1", "eos_after_done": "1",
        "finish_after_empty": "1", "ack_after_finish": "1",
    }, "completion graph mismatch")
    reset = fields(text, "P4_AUDIO86_PCM_RESET ")
    expected_valid = "13" if partial else "240"
    require(reset == {
        "rendered": "13", "ring_owned": "13", "applied_after_ring": "1",
        "ack_after_ring": "1", "forced_publish": "0",
        "first_slot_valid": expected_valid,
    }, "RESET durability/order mismatch")

    residual = fields(text, "P4_AUDIO86_PCM_RESIDUAL ")
    frames, slots, partial_slots, first_occupancy = (
        ("13", "1", "1", "1") if partial else ("2400", "10", "0", "4"))
    require(residual == {
        "occupancy": "0", "partial": "0", "produced_frames": frames,
        "consumed_frames": frames, "produced_bytes": str(int(frames) * 4),
        "consumed_bytes": str(int(frames) * 4), "produced_slots": slots,
        "consumed_slots": slots, "partial_slots": partial_slots, "drops": "0",
        "overwrite": "0", "sequence_errors": "0", "offset_errors": "0",
        "forced_abort": "0", "first_submit_occupancy": first_occupancy,
    }, "PCM residual/accounting mismatch")
    require(one(text, "P4_AUDIO86_PCM_DIRECT_RING_EQUAL") == "1", "direct/ring mismatch")
    require(one(text, "P4_AUDIO86_PCM_OUTPUT_RESULT") == "PASS", "PCM result")
    require(one(text, "RING_PRE_RESET_PCM_SHA256") == PRE_SHA, "ring pre-reset SHA")
    require(one(text, "RING_FULL_PCM_SHA256") == (PRE_SHA if partial else FULL_SHA),
            "ring full SHA")
    require(one(text, "RING_FULL_PCM_FRAMES") == frames, "ring frame count")

    slot_lines = re.findall(
        r"(?m)^P4_AUDIO86_PCM_SLOT sequence=(\d+) frame_offset=(\d+) "
        r"valid_frames=(\d+) flags=(\d+) crc32=([0-9a-f]{8})$", text)
    require(len(slot_lines) == int(slots), "slot evidence count")
    for index, (sequence, offset, valid, flags, _crc) in enumerate(slot_lines):
        require(int(sequence) == index, "slot sequence mismatch")
        require(int(offset) == index * 240, "slot frame offset mismatch")
        expected_frames = 13 if partial else 240
        expected_flags = 1 if partial else 0
        require(int(valid) == expected_frames, "slot valid_frames mismatch")
        require(int(flags) == expected_flags, "slot flags mismatch")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()
    validate_pcm(args.log.read_text(errors="replace"), args.partial)
    print("5C2_VALIDATOR=PASS")
    if args.partial:
        print("5C2_PARTIAL_EOS=PASS")


if __name__ == "__main__":
    main()
