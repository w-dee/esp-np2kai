#!/usr/bin/env python3
"""Fail-closed project-source proof for the S1 drain q_ovf interval."""

from __future__ import annotations

import argparse
from pathlib import Path


def require(condition: bool, message: str) -> None:
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
    raise SystemExit("unterminated project function/block")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    return extract_braced(text, text.index("{", start))


def ordered(text: str, *tokens: str) -> bool:
    offset = 0
    for token in tokens:
        position = text.find(token, offset)
        if position < 0:
            return False
        offset = position + len(token)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    root = parser.parse_args().root.resolve()
    sink = (root / "firmware/components/p4_nano_audio86_physical_sink/"
            "p4_nano_audio86_physical_sink.c").read_text(encoding="utf-8")
    header = (root / "firmware/components/p4_nano_audio86_physical_sink/"
              "include/p4_nano_audio86_physical_sink/"
              "p4_nano_audio86_physical_sink.h").read_text(encoding="utf-8")
    adapter = (root / "firmware/components/p4_nano_audio86_physical_sink/"
               "p4_nano_audio86_physical_sink_idf.cpp").read_text(
                   encoding="utf-8")
    binding = (root / "firmware/components/p4_nano_audio86_guest_binding/"
               "p4_nano_audio86_guest_binding.cpp").read_text(
                   encoding="utf-8")
    terminal = (root / "firmware/components/p4_nano_audio86_guest_binding/"
                "include/p4_nano_audio86_guest_binding/"
                "p4_nano_audio86_terminal_predicate.hpp").read_text(
                    encoding="utf-8")
    controller = (root / "firmware/components/np2pcm_output/"
                  "np2pcm_output.c").read_text(encoding="utf-8")

    require("uint32_t quiescent_eof_epoch;" in header and
            "uint32_t quiescent_eof_epoch;" in sink,
            "quiescent EOF telemetry field missing")

    submit = function_body(sink, "static enum np2_pcm_sink_result physical_submit")
    require(ordered(
        submit,
        "sink->accepted_pending_drain_frames += view->valid_frames;",
        "sink->drain_snapshot_epoch = atomic_load_explicit(",
        "if (state == P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING)",
        "start_stream(sink, P4_NANO_AUDIO86_PHYSICAL_RUNNING)",
        "return NP2_PCM_SINK_ACCEPTED;"),
        "final-copy EOF snapshot/acceptance ordering drifted")
    require("P4_NANO_AUDIO86_PHYSICAL_DRAINING" not in submit,
            "submit path entered DRAINING around final-copy snapshot")

    finish = function_body(sink, "static enum np2_pcm_sink_result physical_finish")
    require(ordered(
        finish,
        "snapshot = sink->drain_snapshot_epoch;",
        "P4_NANO_AUDIO86_PHYSICAL_DRAINING",
        "sink->drain_completion_epoch = atomic_load_explicit(",
        "disarm_callbacks(sink);",
        "sink->backend.mute",
        "sink->backend.pa_low",
        "sink->backend.disable",
        "close_callbacks(sink)",
        "sink->quiescent_eof_epoch = atomic_load_explicit(",
        "sink->i2s_created = false;",
        "P4_NANO_AUDIO86_PHYSICAL_QUIESCENT",
        "sink->finish_completed = true"),
        "drain completion/barrier/quiescent EOF ordering drifted")
    require(finish.count("sink->quiescent_eof_epoch =") == 1,
            "quiescent EOF capture is not unique")

    close = function_body(sink, "static bool close_callbacks")
    require(ordered(close, "sink->backend.unregister_callbacks",
                    "wait_for_callbacks_zero(sink)"),
            "callback delivery/in-flight barrier ordering drifted")
    disarm = function_body(sink, "static void disarm_callbacks")
    require(ordered(disarm, "sink->callback_gate.armed, 0U",
                    "sink->callback_gate.target, (uintptr_t)0U",
                    "&sink->generation, 1U"),
            "callback disarm/generation ordering drifted")

    on_sent = function_body(sink, "static CALLBACK_IRAM void callback_on_sent")
    on_qovf = function_body(
        sink, "static CALLBACK_IRAM void callback_on_send_q_ovf")
    gate_call = "callback_gate_enter(gate, generation, generation_override)"
    require(on_sent.count(gate_call) == 1 and on_qovf.count(gate_call) == 1,
            "EOF/q_ovf generation filtering diverged")
    require(ordered(on_sent, gate_call, "sink->tx_eof_epoch") and
            ordered(on_qovf, gate_call,
                    "P4_NANO_AUDIO86_PHYSICAL_DRAINING",
                    "sink->draining_queue_overflow_count"),
            "valid-generation callback accounting drifted")

    telemetry = function_body(
        sink, "void p4_nano_audio86_physical_sink_get_telemetry")
    require("telemetry->quiescent_eof_epoch = sink->quiescent_eof_epoch;" in
            telemetry and
            "telemetry->draining_queue_overflow_count = atomic_load_explicit(" in
            telemetry,
            "terminal EOF/q_ovf telemetry observation missing")

    consumer = function_body(binding, "void pcm_consumer_task")
    require(ordered(consumer, "np2_pcm_output_start",
                    "np2_pcm_output_finish",
                    "pcm_consumer_terminal_ack.store(1U",
                    "pcm_consumer_quiescent.store(1U",
                    "xSemaphoreGive(runtime->pcm_done_semaphore)",
                    "vTaskSuspend(nullptr)"),
            "consumer finish/quiescence publication ordering drifted")
    require("constexpr BaseType_t kPcmConsumerCore = 0;" in binding and
            ordered(binding, "xTaskCreateStaticPinnedToCore(",
                    "pcm_consumer_task", "kPcmConsumerCore"),
            "physical consumer core topology drifted")

    start = function_body(sink, "static enum np2_pcm_sink_result physical_start")
    prepare = function_body(adapter, "int prepare(")
    create_i2s = function_body(adapter, "esp_err_t create_i2s")
    require("sink->backend.prepare" in start and
            "create_i2s(backend)" in prepare and
            "i2s_channel_init_std_mode" in create_i2s,
            "consumer-owned I2S initialization call path drifted")

    step = function_body(controller, "enum np2_pcm_output_status np2_pcm_output_step")
    controller_finish = function_body(
        controller, "enum np2_pcm_output_status np2_pcm_output_finish")
    require(ordered(step, "controller->sink.submit",
                    "np2opngen_pcm_ring_consume"),
            "final physical acceptance no longer precedes ring consume")
    require(ordered(controller_finish, "np2opngen_pcm_ring_occupancy",
                    "controller->sink.finish"),
            "finish no longer follows empty-ring check")

    capture = function_body(binding, "void capture_physical_s1_snapshot")
    require(ordered(capture, "p4_nano_audio86_physical_sink_get_telemetry",
                    "runtime->physical_s1 = snapshot"),
            "runtime-owned terminal telemetry snapshot ordering drifted")
    cleanup = function_body(binding, "bool cleanup_pcm_start_failure")
    run = function_body(binding, "esp_err_t run_on_pc98_task")
    for owner, label in ((cleanup, "start-failure"), (run, "normal")):
        require(owner.count("capture_physical_s1_snapshot(runtime);") == 1,
                f"{label} physical snapshot occurrence drifted")
        require(ordered(owner,
                        "wait_task_suspended(runtime->pcm_consumer)",
                        "capture_physical_s1_snapshot(runtime);",
                        "vTaskDelete(runtime->pcm_consumer)",
                        "p4_nano_audio86_physical_sink_destroy"),
                f"{label} snapshot/quiescence/destroy ordering drifted")

    evidence = function_body(binding, "void emit_physical_s1_evidence")
    healthy = function_body(terminal, "bool physical_s1_snapshot_healthy")
    require("5D2_S1_FINISH schema=2" in evidence and
            "drain_completion_eof_epoch=" in evidence and
            "quiescent_eof_epoch=" in evidence and
            "drain_post_snapshot_eofs=" in evidence and
            "quiescent_post_snapshot_eofs=" in evidence,
            "S1 schema-2 interval evidence missing")
    require("sink.drain_completion_epoch - sink.drain_snapshot_epoch" in
            healthy and
            "sink.quiescent_eof_epoch - sink.drain_snapshot_epoch" in healthy and
            "quiescent_post_snapshot_eofs >= drain_post_snapshot_eofs" in
            healthy and
            "sink.draining_queue_overflow_count <=" in healthy,
            "physical-short interval predicate drifted")

    print(
        "5D1_STATIC_EVIDENCE schema=1 "
        "property_id=project_drain_qovf_interval "
        "evidence_class=STATIC_PROJECT_SOURCE "
        "fields=final_copy|drain_completion|callback_barrier|quiescent_eof|"
        "quiescent_qovf|generation|owner_snapshot|same_core "
        "predicate=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
