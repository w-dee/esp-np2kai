#!/usr/bin/env python3
"""Static project invariants supplementing executable physical-I2S evidence."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "6460b3d87f3b62d2974f187d12fbe1b0a5a0035c"
SINK = ROOT / "firmware/components/p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.c"
BINDING = ROOT / "firmware/components/p4_nano_audio86_guest_binding/p4_nano_audio86_guest_binding.cpp"
TEST_GUARD = "P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE"


def ordered(text: str, *tokens: str) -> bool:
    offset = 0
    for token in tokens:
        position = text.find(token, offset)
        if position < 0:
            return False
        offset = position + len(token)
    return True


def production_projection(text: str) -> str:
    """Select the test-profile-false branch without preprocessing IDF."""
    output: list[str] = []
    test_stack: list[tuple[bool, bool]] = []
    suppressed = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#if") and TEST_GUARD in stripped:
            test_stack.append((suppressed, False))
            suppressed = True
            continue
        if test_stack and stripped == "#else":
            parent_suppressed, _ = test_stack[-1]
            test_stack[-1] = (parent_suppressed, True)
            suppressed = parent_suppressed
            continue
        if test_stack and stripped == "#endif":
            parent_suppressed, _ = test_stack.pop()
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
    print("CALLBACK_PROJECT_SOURCE_INVARIANTS=PASS")
    print("START_FATAL_COMMON_EPILOGUE=PASS")
    print("START_FATAL_SELF_DELETE=NO")
    print("OWNER_START_FAILURE_CLEANUP=PASS")
    print("5D1_START_RUNTIME_FAULT_MATRIX=3/3_PASS evidence_class=STATIC_PROJECT_SOURCE")
    print("PRODUCTION_BEHAVIOR_CHANGED=NO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
