#!/usr/bin/env python3
"""Ordering mutations for the project-side S1 interval source proof."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/emu/check_p4_audio86_project_interval.py"
FILES = (
    "firmware/components/p4_nano_audio86_physical_sink/"
    "p4_nano_audio86_physical_sink.c",
    "firmware/components/p4_nano_audio86_physical_sink/include/"
    "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h",
    "firmware/components/p4_nano_audio86_physical_sink/"
    "p4_nano_audio86_physical_sink_idf.cpp",
    "firmware/components/p4_nano_audio86_guest_binding/"
    "p4_nano_audio86_guest_binding.cpp",
    "firmware/components/np2pcm_output/np2pcm_output.c",
)


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def rejected(root: Path, relative: str, mutate, label: str) -> None:
    path = root / relative
    original = path.read_text(encoding="utf-8")
    changed = mutate(original)
    if changed == original:
        raise SystemExit(f"{label}: mutation did not apply")
    path.write_text(changed, encoding="utf-8")
    try:
        result = run_checker(root)
        if result.returncode == 0:
            raise SystemExit(f"{label}: interval drift was accepted")
    finally:
        path.write_text(original, encoding="utf-8")
    print(f"{label}=PASS")


def replace_once(old: str, new: str):
    def mutate(text: str) -> str:
        if text.count(old) != 1:
            raise SystemExit("mutation target is not unique")
        return text.replace(old, new, 1)
    return mutate


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "repo"
        for relative in FILES:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        control = run_checker(root)
        if control.returncode != 0:
            raise SystemExit("canonical project source rejected: " +
                             control.stderr)
        print("DRAIN_Q_OVF_PROJECT_INTERVAL_SOURCE_PROOF=PASS")

        sink_rel = FILES[0]
        binding_rel = FILES[3]
        barrier = (
            "    if (!close_callbacks(sink)) {\n"
            "        mark_failed(sink);\n"
            "        return NP2_PCM_SINK_FATAL;\n"
            "    }\n"
            "    /* unregister_callbacks() is the IDF interrupt-delivery barrier")

        def move_eof_before_barrier(text: str) -> str:
            capture = (
                "    sink->quiescent_eof_epoch = atomic_load_explicit(\n"
                "        &sink->tx_eof_epoch, memory_order_acquire);\n")
            if text.count(capture) != 1 or text.count(barrier) != 1:
                raise SystemExit("quiescent EOF mutation target missing")
            text = text.replace(capture, "", 1)
            return text.replace(barrier, capture + barrier, 1)

        rejected(root, sink_rel, move_eof_before_barrier,
                 "PROJECT_QUIESCENT_EOF_BEFORE_BARRIER_REJECTED")

        def move_snapshot_before_quiescence(text: str) -> str:
            block = (
                "    if (physical_snapshot_ready && "
                "runtime->physical_sink != nullptr)\n"
                "        capture_physical_s1_snapshot(runtime);\n")
            anchor = "    const bool pcm_terminal =\n"
            if text.count(block) != 2 or text.count(anchor) != 1:
                raise SystemExit("owner snapshot mutation target missing")
            run_start = text.index("esp_err_t run_on_pc98_task")
            block_at = text.index(block, run_start)
            text = text[:block_at] + text[block_at + len(block):]
            anchor_at = text.index(anchor, run_start)
            return (text[:anchor_at] +
                    "    capture_physical_s1_snapshot(runtime);\n" +
                    text[anchor_at:])

        rejected(root, binding_rel, move_snapshot_before_quiescence,
                 "PROJECT_QOVF_SNAPSHOT_BEFORE_QUIESCENCE_REJECTED")

        def move_final_copy_after_draining(text: str) -> str:
            capture = (
                "    sink->drain_snapshot_epoch = atomic_load_explicit(\n"
                "        &sink->tx_eof_epoch, memory_order_acquire);\n")
            anchor = (
                "    start_ms = sink->backend.now_ms(sink->backend.opaque);\n")
            if text.count(capture) != 1 or text.count(anchor) != 1:
                raise SystemExit("final-copy epoch mutation target missing")
            text = text.replace(capture, "", 1)
            return text.replace(anchor, capture + anchor, 1)

        rejected(root, sink_rel, move_final_copy_after_draining,
                 "PROJECT_FINAL_COPY_EPOCH_AFTER_DRAINING_REJECTED")

        def diverge_qovf_generation(text: str) -> str:
            start = text.index(
                "static CALLBACK_IRAM void callback_on_send_q_ovf")
            target = "callback_gate_enter(gate, generation, generation_override)"
            position = text.index(target, start)
            return (text[:position] +
                    "callback_gate_enter(gate, generation + 1U, "
                    "generation_override)" + text[position + len(target):])

        rejected(root, sink_rel, diverge_qovf_generation,
                 "PROJECT_QOVF_EOF_GENERATION_DIVERGENCE_REJECTED")
        rejected(root, binding_rel,
                 replace_once("constexpr BaseType_t kPcmConsumerCore = 0;",
                              "constexpr BaseType_t kPcmConsumerCore = 1;"),
                 "PROJECT_CONSUMER_CORE_DRIFT_REJECTED")

    print("DRAIN_Q_OVF_COMPLETE_INTERVAL_SOURCE_PROOF=PASS")
    print("DRAIN_Q_OVF_PROJECT_INTERVAL_DRIFT_FAIL_CLOSED=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
