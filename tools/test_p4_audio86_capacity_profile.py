#!/usr/bin/env python3
"""Static contract checks for the isolated 86H P4 capacity profile."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN_CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text()
MAIN_CPP = (ROOT / "firmware/main/main.cpp").read_text()
COMP_CMAKE = (ROOT / "firmware/components/p4_nano_audio86_capacity/CMakeLists.txt").read_text()
COMP_CPP = (ROOT / "firmware/components/p4_nano_audio86_capacity/p4_nano_audio86_capacity.cpp").read_text()
BUILD = (ROOT / "tools/emu/build-production.sh").read_text()
DEFAULTS = (ROOT / "firmware/sdkconfig.defaults.p4-audio86-capacity").read_text()
VALIDATOR_TEST = ROOT / "tools/test_validate_p4_audio86_capacity_log.py"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    check("P4_NANO_AUDIO86_CAPACITY_PROFILE_ACTIVE" in MAIN_CMAKE, "profile not routed")
    check("p4_nano_audio86_capacity" in MAIN_CMAKE, "component not selected")
    check("P4_NANO_AUDIO86_CAPACITY_PROFILE" in MAIN_CPP, "app_main profile missing")
    check("--audio86-capacity" in BUILD and "P4_NANO_AUDIO86_CAPACITY_PROFILE" in BUILD,
          "build script option missing")
    check("build-${board}-${variant}-audio86-capacity" in BUILD, "profile build directory missing")
    check("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y" in DEFAULTS and
          "CONFIG_FREERTOS_HZ=100" in DEFAULTS and
          "CONFIG_SPIRAM_SPEED_200M=y" in DEFAULTS and
          "# CONFIG_PM_ENABLE is not set" in DEFAULTS, "sdkconfig defaults incomplete")
    check("PRIV_REQUIRES np2audio86_fixture" in COMP_CMAKE, "portable fixture not required")
    check("P4_AUDIO86_PROFILE_MODE" in COMP_CMAKE,
          "profile component-timing mode is not selectable")

    # The capacity branch must not pull the normal runtime/display/audio sink.
    branch = re.search(
        r"elseif\(P4_NANO_AUDIO86_CAPACITY_PROFILE_ACTIVE\).*?\n(elseif|else\(\))",
        MAIN_CMAKE,
        re.DOTALL,
    )
    check(branch is not None, "capacity CMake branch missing")
    forbidden = ("p4_nano_display", "np2core", "storage_fatfs", "audio_i2s",
                 "es8311", "pcm86io", "board86")
    check(not any(token in branch.group(0).lower() for token in forbidden),
          "capacity branch enables forbidden runtime dependency")
    source_lower = COMP_CPP.lower()
    check(not any(token in source_lower for token in ("i2s", "es8311", "pcm86io", "board86")),
          "capacity component contains forbidden sink/runtime integration")
    check("xTaskNotifyGive" in COMP_CPP and "xSemaphoreGive(ctx->pacing)" in COMP_CPP,
          "transport and pacing wake channels are not distinct")
    check("compare_exchange_strong" in COMP_CPP,
          "first-error CAS missing")
    check("static_cast<uint64_t>(q) * kQuantumUs" in COMP_CPP and
          "finish >= deadline" in COMP_CPP, "absolute schedule/deadline missing")
    check("ceil(p * N) - 1" not in COMP_CPP, "placeholder percentile text present")
    # MEM.1 ownership and terminal contracts.
    check("struct np2audio86_event *plan" in COMP_CPP and
          "struct np2audio86_event_ring *events" in COMP_CPP and
          "struct np2audio86_byte_ring *pcm_bytes" in COMP_CPP and
          "struct np2audio86_render_state *render" in COMP_CPP,
          "formal storage is not explicitly owned")
    check("uint32_t *service_us" in COMP_CPP and
          "uint8_t *worker_run" in COMP_CPP and "uint8_t *canonical" in COMP_CPP,
          "large storage pointers missing")
    check("refill_quanta[NP2_AUDIO86_QUANTA]" not in COMP_CPP and
          "refill_times[NP2_AUDIO86_QUANTA]" not in COMP_CPP and
          "nonrefill_count = 0U" not in COMP_CPP,
          "redundant refill arrays remain")
    check("ctx->plan[scan].opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN" in COMP_CPP and
          "ctx->service_us[q]" in COMP_CPP,
          "refill statistics are not derived from plan and samples")
    check("MALLOC_CAP_SPIRAM)" not in COMP_CPP or
          "heap_caps_calloc(1U, bytes, internal_caps)" in COMP_CPP,
          "unexpected allocator capability change")
    check("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in COMP_CPP and
          "MALLOC_CAP_SPIRAM" in COMP_CPP,
          "internal allocator contract missing")
    benchmark = COMP_CPP[COMP_CPP.index("static esp_err_t run_benchmark"):
                        COMP_CPP.index("esp_err_t run()")]
    check("AUDIO86_P4_RESULT=" not in benchmark,
          "run_benchmark owns the physical terminal")
    check("AUDIO86_P4_MEMORY_PREALLOC" in COMP_CPP and
          "AUDIO86_P4_ALLOC seq=" in COMP_CPP and
          "AUDIO86_P4_FAILURE first_error=ALLOCATION allocation=" in COMP_CPP,
          "early allocation evidence contract missing")
    check("cleanup(ctx, raw)" in COMP_CPP and "vSemaphoreDelete" in COMP_CPP,
          "partial allocation cleanup missing")
    check("kProducerCore = 1" in COMP_CPP and "kWorkerCore = 0" in COMP_CPP and
          "kProducerPriority = tskIDLE_PRIORITY + 3" in COMP_CPP and
          "kWorkerPriority = tskIDLE_PRIORITY + 4" in COMP_CPP,
          "task topology changed")

    # TIME.1 pacing epoch and per-quantum correctness contracts.
    worker_epoch = COMP_CPP[COMP_CPP.index("static void worker_task"):
                             COMP_CPP.index("static void timer_callback")]
    benchmark = COMP_CPP[COMP_CPP.index("static esp_err_t run_benchmark"):
                         COMP_CPP.index("esp_err_t run()")]
    check("const uint64_t t0 = ctx->start_us;" in worker_epoch and
          "const uint64_t t0 = esp_timer_get_time();" not in worker_epoch,
          "worker establishes an independent pacing epoch")
    check("ctx->start_us = esp_timer_get_time();" in benchmark and
          "esp_timer_start_periodic(ctx->timer, kQuantumUs)" in benchmark,
          "coordinator-owned epoch or timer arm missing")
    start = benchmark.index("if (start_status == ESP_OK) {")
    release = benchmark.index("xSemaphoreGive(ctx->worker_start);", start)
    formal_start = benchmark[start:release]
    check("std::printf" not in formal_start and "emit_resource" not in formal_start and
          "heap_caps_" not in formal_start,
          "formal timer arm has logging/allocation before worker release")
    check("xSemaphoreTake(ctx->pacing, portMAX_DELAY)" in worker_epoch and
          "q != 0U" in worker_epoch and "ticks > q" in worker_epoch and
          "ticks < q" in worker_epoch,
          "q0/q1 pacing permit contract missing")
    check("vTaskDelay(1)" not in worker_epoch and "taskYIELD" not in worker_epoch,
          "100Hz pacing wait remains")
    callback = COMP_CPP[COMP_CPP.index("static void timer_callback"):
                        COMP_CPP.index("static uint32_t percentile")]
    check("fetch_add" in callback and "xSemaphoreGive(ctx->pacing)" in callback and
          "notify(ctx->worker_task)" not in callback,
          "timer callback pacing semantics changed")
    render = COMP_CPP[COMP_CPP.index("static bool render_quantum"):
                      COMP_CPP.index("static void producer_task")]
    check("std::memset(ctx->render->mix_scratch, 0," in render and
          render.index("service_start") < render.index("std::memset") <
          render.index("np2audio86_render_span"),
          "per-quantum scratch clear is not inside service interval")
    check(render.count("std::memset(ctx->render->mix_scratch") == 1,
          "scratch is cleared more than once in the quantum adapter")
    check(all(token in COMP_CPP for token in (
        "completed_quanta", "canonicalized_quanta", "service_sample_count",
        "failed_quantum")), "explicit progress state missing")
    check("AUDIO86_P4_PROGRESS" in COMP_CPP and
          "completed_frames" in COMP_CPP and "completed_bytes" in COMP_CPP,
          "actual progress log missing")
    check("ctx->input_starvation" in COMP_CPP and
          "formal_epoch_started.load" in COMP_CPP and
          "fail(ctx, NP2_AUDIO86_ASYNC_ERROR_WATERMARK)" in COMP_CPP,
          "post-epoch input starvation gate missing")
    check("subset_order_statistic" not in COMP_CPP and
          "NP2_AUDIO86_ASYNC_MAX_EVENTS]{}" in COMP_CPP and
          "ctx->service_sample_count" in COMP_CPP,
          "bounded partial timing algorithm missing")

    # LIFE.1 task ownership and terminal-park contracts.
    check("std::atomic<bool> producer_reap_ready{false};" in COMP_CPP and
          "std::atomic<bool> worker_reap_ready{false};" in COMP_CPP,
          "reap-ready state missing")
    producer = COMP_CPP[COMP_CPP.index("static void producer_task"):
                        COMP_CPP.index("static void worker_task")]
    worker = COMP_CPP[COMP_CPP.index("static void worker_task"):
                      COMP_CPP.index("static void timer_callback")]
    wait = COMP_CPP[COMP_CPP.index("static void wait_for_tasks"):
                    COMP_CPP.index("static void cleanup")]
    for task_name, body, ready_name, hwm_name in (
        ("producer", producer, "producer_reap_ready", "producer_hwm"),
        ("worker", worker, "worker_reap_ready", "worker_hwm"),
    ):
        tail_start = body.index(f"ctx->{ready_name}.store(true")
        tail = body[tail_start:]
        check(re.search(r"store\(true, std::memory_order_release\);\s*"
                        r"vTaskSuspend\(nullptr\);\s*}\s*$", tail),
              f"{task_name} terminal park is not an immediate suspend")
        check("ctx->" not in tail.split("vTaskSuspend(nullptr);", 1)[0].split(
            ");", 1)[1],
              f"{task_name} accesses Context after reap-ready publication")
        check(body.index(f"ctx->{hwm_name}.store") < tail_start,
              f"{task_name} HWM is not stored before reap-ready")
        check("ulTaskNotifyTake(pdTRUE, portMAX_DELAY);" not in tail,
              f"{task_name} terminal park still uses notifications")
    check("ctx->worker_reap_ready.load(std::memory_order_acquire)" in wait and
          "ctx->producer_reap_ready.load(std::memory_order_acquire)" in wait,
          "wait_for_tasks does not require reap-ready")
    check("ctx->worker_done.load(std::memory_order_acquire)" not in wait and
          "ctx->producer_done.load(std::memory_order_acquire)" not in wait,
          "logical done still authorizes deletion")
    check(wait.index("stop_timer(ctx);") < wait.index("vTaskDelete(ctx->worker_task);"),
          "worker timer is not stopped before reap")
    check("ctx->worker_task = nullptr;" in wait and
          "ctx->producer_task = nullptr;" in wait,
          "reaped task handles are not cleared")
    check("sizeof(struct np2audio86_event_ring)" in COMP_CPP,
          "event ring omitted from direct ownership accounting")
    lifecycle_states = ("RUNNING", "LOGICAL_DONE", "REAP_READY", "DELETED")
    check(lifecycle_states[1] != lifecycle_states[2],
          "logical done and reap-ready states collapsed")
    check(lifecycle_states.index("REAP_READY") < lifecycle_states.index("DELETED"),
          "lifecycle deletion ordering invalid")
    check(VALIDATOR_TEST.exists(), "focused validator test path missing")
    # Nearest-rank indices are part of the frozen 12,000-sample contract.
    check(int(0.99 * 12000 + 0.999999999) - 1 == 11879, "p99 index changed")
    check(int(0.999 * 12000 + 0.999999999) - 1 == 11987, "p99.9 index changed")
    print("P4_AUDIO86_STATIC_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
