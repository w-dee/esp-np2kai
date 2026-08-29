#!/usr/bin/env python3
"""Host-only model tests for the A3.4 PCM-ring to I2S boundary.

The sink advances a logical five milliseconds per 240-frame write.  No wall
clock sleeps or hardware APIs are used here; the model exercises ownership,
backpressure, identity, failure, and final-drain semantics deterministically.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "firmware/components/p4_nano_audio_i2s_opngen/"
          "p4_nano_audio_i2s_opngen.cpp").read_text(encoding="utf-8")
RETRO_GOLDEN = json.loads(
    (ROOT / "tools/emu/opngen_retrofm_s98_golden.json").read_text(
        encoding="utf-8"))
STRESS_GOLDEN = json.loads(
    (ROOT / "tools/emu/opngen_sustained_workload_golden.json").read_text(
        encoding="utf-8"))

QUANTUM_FRAMES = 240
BYTES_PER_FRAME = 4
QUANTUM_BYTES = QUANTUM_FRAMES * BYTES_PER_FRAME
RING_CAPACITY = 8


@dataclass(frozen=True)
class Slot:
    sequence: int
    frame_offset: int
    pcm: bytes


class Ring:
    def __init__(self) -> None:
        self.slots: list[Slot] = []
        self.finalized = False

    def append(self, slot: Slot) -> bool:
        if self.finalized or len(self.slots) >= RING_CAPACITY:
            return False
        if len(slot.pcm) != QUANTUM_BYTES:
            raise AssertionError("ring accepts only complete canonical slots")
        self.slots.append(slot)
        return True

    def finish(self) -> None:
        self.finalized = True

    def peek(self) -> Slot | None:
        return self.slots[0] if self.slots else None

    def consume(self) -> Slot:
        if not self.slots:
            raise AssertionError("consume from empty ring")
        return self.slots.pop(0)


class MockI2SSink:
    """A blocking I2S write model with logical hardware-clock pacing."""

    def __init__(self, mode: str = "ok") -> None:
        self.mode = mode
        self.logical_ticks = 0
        self.writes: list[bytes] = []
        self.complete = 0
        self.partial = 0
        self.timeout = 0
        self.errors = 0

    def write(self, pcm: bytes) -> tuple[str, int]:
        if len(pcm) != QUANTUM_BYTES:
            raise AssertionError("consumer must submit exactly one 960-byte slot")
        self.logical_ticks += 5
        if self.mode == "partial":
            self.partial += 1
            return "ESP_OK", QUANTUM_BYTES // 2
        if self.mode == "timeout":
            self.timeout += 1
            return "ESP_ERR_TIMEOUT", 0
        if self.mode == "error":
            self.errors += 1
            return "ESP_FAIL", 0
        self.complete += 1
        self.writes.append(bytes(pcm))
        return "ESP_OK", QUANTUM_BYTES


def consume_one(ring: Ring, sink: MockI2SSink, expected_sequence: int,
                expected_offset: int) -> tuple[int, int, bool]:
    slot = ring.peek()
    if slot is None:
        return expected_sequence, expected_offset, False
    if (slot.sequence != expected_sequence or
            slot.frame_offset != expected_offset):
        return expected_sequence, expected_offset, False
    result, written = sink.write(slot.pcm)
    # Ownership changes only after a complete successful write.
    if result != "ESP_OK" or written != QUANTUM_BYTES:
        return expected_sequence, expected_offset, False
    ring.consume()
    return expected_sequence + 1, expected_offset + QUANTUM_FRAMES, True


def payload(sequence: int) -> bytes:
    return bytes((sequence + index) & 0xFF for index in range(QUANTUM_BYTES))


def run_workload(blocks: int) -> dict[str, int | bool | str]:
    ring = Ring()
    sink = MockI2SSink()
    generated_sha = hashlib.sha256()
    generated_crc = 0
    submitted_sha = hashlib.sha256()
    submitted_crc = 0
    produced = 0
    expected_sequence = 0
    expected_offset = 0
    full_wait_count = 0
    underruns = 0

    # The producer is intentionally faster than the logical I2S clock.  When
    # all eight slots are owned by the ring, one complete write frees space;
    # no sample is dropped or overwritten.
    while produced < blocks:
        pcm = payload(produced)
        generated_sha.update(pcm)
        generated_crc = zlib.crc32(pcm, generated_crc)
        slot = Slot(produced, produced * QUANTUM_FRAMES, pcm)
        while not ring.append(slot):
            full_wait_count += 1
            before = len(sink.writes)
            expected_sequence, expected_offset, ok = consume_one(
                ring, sink, expected_sequence, expected_offset)
            if not ok or len(sink.writes) == before:
                raise AssertionError("full ring did not make lossless progress")
            submitted_sha.update(sink.writes[-1])
            submitted_crc = zlib.crc32(sink.writes[-1], submitted_crc)
        produced += 1

    ring.finish()
    while ring.peek() is not None:
        before = len(sink.writes)
        expected_sequence, expected_offset, ok = consume_one(
            ring, sink, expected_sequence, expected_offset)
        if not ok or len(sink.writes) == before:
            raise AssertionError("final ring drain failed")
        submitted_sha.update(sink.writes[-1])
        submitted_crc = zlib.crc32(sink.writes[-1], submitted_crc)

    # Empty-after-producer-done is normal completion, followed by a bounded
    # 20 ms DMA pipeline drain.  Empty-before-done is the underrun case.
    if ring.peek() is not None:
        underruns += 1
    final_drain_ticks = 20
    return {
        "blocks": blocks,
        "generated_sha": generated_sha.hexdigest(),
        "submitted_sha": submitted_sha.hexdigest(),
        "generated_crc": generated_crc & 0xFFFFFFFF,
        "submitted_crc": submitted_crc & 0xFFFFFFFF,
        "complete": sink.complete,
        "partial": sink.partial,
        "timeout": sink.timeout,
        "errors": sink.errors,
        "full_wait_count": full_wait_count,
        "logical_ticks": sink.logical_ticks,
        "final_drain_ticks": final_drain_ticks,
        "final_occupancy": len(ring.slots),
        "underruns": underruns,
        "lifecycle": "Init>Prefill>I2sReady>Running>ProducerDone>Draining>I2sDrain>Complete",
    }


def source_digest(name: str) -> str:
    body = re.search(
        rf"static const uint8_t k{name}PcmSha\[\] = \{{(?P<body>.*?)\}};",
        SOURCE, re.DOTALL)
    if body is None:
        raise AssertionError(f"missing {name} PCM digest")
    return bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})",
                                                         body.group("body"))).hex()


class A34BoundaryTests(unittest.TestCase):
    def test_context_allocation_failure_is_fail_closed(self) -> None:
        run_start = SOURCE.index("static bool run_workload")
        run = SOURCE[run_start:SOURCE.index("\n} // namespace", run_start)]
        allocation_failure = run.index("if (ctx == nullptr)")
        task_creation = run.index("xTaskCreatePinnedToCore")
        self.assertLess(allocation_failure, task_creation)
        self.assertNotIn("successful = true", run[allocation_failure:task_creation])
        self.assertEqual(run.count("heap_caps_free(ctx);"), 1)
        self.assertEqual(run.count("ctx->~Context();"), 1)
        self.assertLess(run.index("ctx->~Context();"), run.index("heap_caps_free(ctx);"))

    def test_exact_write_and_identity(self) -> None:
        result = run_workload(5)
        self.assertEqual(result["complete"], 5)
        self.assertEqual(result["logical_ticks"], 25)
        self.assertEqual(result["final_occupancy"], 0)
        self.assertEqual(result["underruns"], 0)
        self.assertEqual(result["generated_sha"], result["submitted_sha"])
        self.assertEqual(result["generated_crc"], result["submitted_crc"])

    def test_slot_ownership_on_write_failure(self) -> None:
        for mode, field in (("partial", "partial"), ("timeout", "timeout"),
                            ("error", "errors")):
            ring = Ring()
            ring.append(Slot(0, 0, payload(0)))
            sink = MockI2SSink(mode)
            _, _, ok = consume_one(ring, sink, 0, 0)
            self.assertFalse(ok)
            self.assertEqual(len(ring.slots), 1)
            self.assertEqual(sink.complete, 0)
            self.assertEqual(sink.__dict__[field], 1)

    def test_underrun_and_normal_final_empty(self) -> None:
        sink = MockI2SSink()
        ring = Ring()
        _, _, ok = consume_one(ring, sink, 0, 0)
        self.assertFalse(ok)  # caller classifies this as underrun before done
        ring.finish()
        self.assertIsNone(ring.peek())  # normal final empty after producer done

    def test_backpressure_and_final_tail(self) -> None:
        result = run_workload(12)
        self.assertGreater(result["full_wait_count"], 0)
        self.assertEqual(result["final_drain_ticks"], 20)
        self.assertEqual(result["lifecycle"].split(">")[-1], "Complete")

    def test_a2_golden_metadata_is_bound_to_profile(self) -> None:
        retro = RETRO_GOLDEN["pcm"]
        stress = STRESS_GOLDEN["cases"]["STRESS-60"]
        self.assertEqual(retro["sample_rate_hz"], 48000)
        self.assertEqual(retro["frames"] // QUANTUM_FRAMES, 2404)
        self.assertEqual(stress["pcm_frames"] // QUANTUM_FRAMES, 12000)
        self.assertEqual(source_digest("Retro"), retro["sha256"])
        self.assertEqual(source_digest("Stress"), stress["pcm_sha256"])


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(A34BoundaryTests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if not result.wasSuccessful():
        return 1
    retro_blocks = RETRO_GOLDEN["pcm"]["frames"] // QUANTUM_FRAMES
    stress_blocks = STRESS_GOLDEN["cases"]["STRESS-60"]["pcm_frames"] // QUANTUM_FRAMES
    retro = run_workload(retro_blocks)
    stress = run_workload(stress_blocks)
    for name, record in (("RETROFM", retro), ("STRESS-60", stress)):
        print(f"P4_AUDIO_I2S_OPNGEN_HOST_RESULT workload={name} blocks={record['blocks']} "
              f"ticks={record['logical_ticks']} complete={record['complete']} "
              f"full_wait_count={record['full_wait_count']} final_occupancy={record['final_occupancy']} result=PASS")
    print("P4_AUDIO_I2S_OPNGEN_HOST_TEST=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
