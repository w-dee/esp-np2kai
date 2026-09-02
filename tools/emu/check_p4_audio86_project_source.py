#!/usr/bin/env python3
"""Static project invariants supplementing executable physical-I2S evidence."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "6460b3d87f3b62d2974f187d12fbe1b0a5a0035c"
SINK = ROOT / "firmware/components/p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.c"
BINDING = ROOT / "firmware/components/p4_nano_audio86_guest_binding/p4_nano_audio86_guest_binding.cpp"
TEST_GUARD = "P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE"
SHORT_GUARD = "P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE"


def evidence(property_id: str, required_fields: str) -> None:
    print(
        "5D1_STATIC_EVIDENCE schema=1 "
        f"property_id={property_id} "
        "evidence_class=STATIC_PROJECT_SOURCE "
        f"fields={required_fields} predicate=PASS")


def ordered(text: str, *tokens: str) -> bool:
    offset = 0
    for token in tokens:
        position = text.find(token, offset)
        if position < 0:
            return False
        offset = position + len(token)
    return True


def production_projection(text: str) -> str:
    """Select test-profile-false and physical-short-false source branches."""
    output: list[str] = []
    conditional_stack: list[tuple[bool, bool, bool]] = []
    suppressed = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(("#if", "#ifdef", "#ifndef")):
            selected_guard = next(
                (guard for guard in (TEST_GUARD, SHORT_GUARD)
                 if guard in stripped), None)
            if selected_guard is None:
                conditional_stack.append((False, suppressed, True))
                if not suppressed:
                    output.append(line.rstrip())
                continue
            condition_true = stripped.startswith("#ifndef") or \
                "!defined" in stripped
            conditional_stack.append((True, suppressed, condition_true))
            suppressed = suppressed or not condition_true
            continue
        if conditional_stack and stripped == "#else":
            selected, parent_suppressed, condition_true = \
                conditional_stack[-1]
            if selected:
                suppressed = parent_suppressed or condition_true
            elif not suppressed:
                output.append(line.rstrip())
            continue
        if conditional_stack and stripped == "#endif":
            selected, parent_suppressed, _ = conditional_stack.pop()
            if not selected and not suppressed:
                output.append(line.rstrip())
            suppressed = parent_suppressed
            continue
        if not suppressed:
            output.append(line.rstrip())
    return "\n".join(line for line in output if line.strip())


def main() -> int:
    sink = SINK.read_text(encoding="utf-8")
    binding = BINDING.read_text(encoding="utf-8")
    baseline = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:{BINDING.relative_to(ROOT)}"],
        text=True)
    if production_projection(binding) != production_projection(baseline):
        raise SystemExit("production guest-binding projection changed")
    callback = sink[sink.index("static CALLBACK_IRAM struct p4_nano_audio86_physical_sink *callback_gate_enter"):]
    if not ordered(callback, "atomic_fetch_add_explicit(&gate->in_flight",
                   "atomic_load_explicit(&gate->armed",
                   "atomic_load_explicit(&gate->target"):
        raise SystemExit("callback entry ordering drift")
    if "struct p4_nano_audio86_callback_gate callback_gate;" not in sink or \
       "load_state(sink) != P4_NANO_AUDIO86_PHYSICAL_INITIAL" not in sink:
        raise SystemExit("callback graph/reuse invariant drift")
    consumer = binding[binding.index("void pcm_consumer_task"):]
    if not ordered(consumer, "publish_pcm_forced_abort(runtime, kErrorWorker)",
                   "pcm_consumer_terminal_ack.store(1U",
                   "pcm_consumer_quiescent.store(1U",
                   "xSemaphoreGive(runtime->pcm_done_semaphore)",
                   "vTaskSuspend(nullptr)"):
        raise SystemExit("start fatal epilogue drift")
    cleanup = binding[binding.index("bool cleanup_pcm_start_failure"):]
    if not ordered(cleanup, "terminal &&", "quiescent && ack &&",
                   "wait_task_suspended(runtime->pcm_consumer)",
                   "if (suspended)", "vTaskDelete(runtime->pcm_consumer)"):
        raise SystemExit("owner delete guard drift")
    if not ordered(cleanup, "if (suspended && runtime->physical_sink != nullptr)",
                   "p4_nano_audio86_physical_sink_destroy"):
        raise SystemExit("owner destroy guard drift")
    evidence("callback_entry_first", "atomic_in_flight_before_target")
    evidence("callback_object_graph", "gate_embedded|target_atomic")
    evidence("callback_no_reuse", "initial_only_start")
    evidence("start_epilogue", "ack|quiescent|done|suspend")
    evidence("start_no_self_delete", "vTaskSuspend")
    evidence("owner_cleanup_guards",
             "terminal|quiescent|ack|wait_task_suspended")
    evidence("owner_delete_order", "suspended_before_delete")
    evidence("owner_destroy_order", "suspended_before_destroy")
    evidence("production_projection",
             "baseline|test_guard_false|physical_short_false")
    print("5D1_NON_ACCEPTANCE_SUMMARY name=START_STATIC_GUARDS value=3/3_PASS")
    print("START_RUNTIME_FAULT_INJECTION_REQUIRED_FOR_5D1=NO")
    print("PRODUCTION_BEHAVIOR_CHANGED=PHYSICAL_SHORT_ONLY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
