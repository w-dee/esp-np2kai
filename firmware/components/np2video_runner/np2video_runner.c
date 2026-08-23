#include "np2video_runner/np2video_runner.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <compiler.h>
#include <cpumem.h>
#include <cpucore.h>
#include <diskimage/fddfile.h>
#include <pccore.h>
#include <scrnmng.h>

#include <np2host/taskmng_esp.h>
#include <taskmng.h>
#include <stage1_machine_config.h>
#include <video_control_v1.h>

#include "np2_fixture.h"
#include "np2video_golden.h"

#define NP2VIDEO_FORMAL_EXTMEM_MB 13U
#define NP2VIDEO_EFFECTIVE_EXTMEM_MB 8U
#define NP2VIDEO_RUNNER_STACK_BYTES 32768U
#define NP2VIDEO_RUNNER_PRIORITY (tskIDLE_PRIORITY + 3)
#define NP2VIDEO_READY_SLICE_LIMIT UINT32_C(4096)
#define NP2VIDEO_POST_READY_SLICE_LIMIT UINT32_C(4)

typedef struct {
    np2video_runner_output_fn output;
    void *output_context;
    np2video_runner_ready_fn ready;
    np2video_runner_scene_ready_fn scene_ready;
    np2video_runner_stopping_fn stopping;
    np2video_runner_complete_fn complete;
    void *complete_context;
    void *lifecycle_context;
    np2video_runner_stop_requested_fn stop_requested;
} np2video_runner_task_state;

static np2video_runner_task_state np2video_task_config;
static int np2video_runner_started;

static const np2_fixture_descriptor np2video_fixture_descriptor = {
    NP2_FIXTURE_PARTITION_LABEL,
    "./np2video-fd1232.hdm",
    np2video_golden_fixture_image_size,
    np2video_golden_fixture_sha256,
    NP2_FIXTURE_TRACKS,
    NP2_FIXTURE_SECTORS,
    NP2_FIXTURE_N,
    DISKTYPE_2HD,
};

static bool np2video_emit(np2video_runner_task_state *state,
                          const char *format, ...)
{
    char line[768];
    va_list arguments;
    int length;

    if ((state == NULL) || (state->output == NULL)) {
        return false;
    }
    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if ((length < 0) || ((size_t)length >= sizeof(line))) {
        return false;
    }
    return state->output(state->output_context, line, (size_t)length);
}

static void np2video_sha256_text(char output[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0; index < NP2_FIXTURE_SHA256_SIZE; ++index) {
        output[index * 2] = digits[np2video_golden_fixture_sha256[index] >> 4];
        output[index * 2 + 1] =
            digits[np2video_golden_fixture_sha256[index] & 0x0f];
    }
    output[64] = '\0';
}

static const char *np2video_fixture_failure(esp_err_t error)
{
    switch (error) {
        case ESP_ERR_NOT_FOUND:
            return "fixture_partition_not_found";
        case ESP_ERR_INVALID_SIZE:
            return "fixture_size_mismatch";
        case ESP_ERR_INVALID_CRC:
            return "fixture_sha_mismatch";
        default:
            return "fixture_acquire_failed";
    }
}

static bool np2video_verify_extmem(np2video_runner_task_state *state)
{
    const uint32_t expected_bytes =
        NP2VIDEO_EFFECTIVE_EXTMEM_MB * 1024U * 1024U;
    const int pointer_external =
        (i286core.e.ext != NULL) && esp_ptr_external_ram(i286core.e.ext);

    np2video_emit(state,
                  "NP2VIDEO_MEMORY extmem_mb=%u actual_bytes=%lu "
                  "ptr_external=%d psram_size=%lu free_spiram=%lu "
                  "largest_spiram=%lu\n",
                  NP2VIDEO_EFFECTIVE_EXTMEM_MB,
                  (unsigned long)i286core.e.extsize,
                  pointer_external,
                  (unsigned long)esp_psram_get_size(),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned long)heap_caps_get_largest_free_block(
                      MALLOC_CAP_SPIRAM));
    if (i286core.e.extsize != expected_bytes) {
        return false;
    }
    return (i286core.e.ext != NULL) && pointer_external;
}

static bool np2video_verify_framebuffer_contract(void)
{
    SCRNMNG_STATUS status;
    SCRNMNG_SURFACE_VIEW view;

    scrnmng_getstatus(&status);
    if (!status.initialized || status.failed || !status.external ||
        status.width != (int)np2video_golden_width ||
        status.height != (int)np2video_golden_height ||
        status.bytes != np2video_golden_visible_bytes) {
        return false;
    }
    if (scrnmng_get_surface_view(&view) != SCRNMNG_SNAPSHOT_OK) {
        return false;
    }
    return view.ptr != NULL && view.width == (int)np2video_golden_width &&
           view.height == (int)np2video_golden_height &&
           view.bpp == np2video_golden_bpp &&
           view.pixel_format == SCRNMNG_PIXEL_FORMAT_RGB565LE &&
           view.pitch == np2video_golden_pitch;
}

static int np2video_has_magic(const uint8_t *bytes)
{
    return bytes != NULL && bytes[0] == (uint8_t)'N' &&
           bytes[1] == (uint8_t)'P' && bytes[2] == (uint8_t)'2' &&
           bytes[3] == (uint8_t)'V';
}

static const char *np2video_control_failure(const uint8_t *control_bytes,
                                             np2v_control *control)
{
    const np2v_control_status status = np2v_control_parse_for_scene(
        control_bytes, NP2V_CONTROL_SIZE, np2video_golden_scene_id, control);

    if (status == NP2V_CONTROL_INVALID && !np2video_has_magic(control_bytes)) {
        return NULL;
    }
    if (status == NP2V_CONTROL_INVALID) {
        return "invalid_control_block";
    }
    if (status == NP2V_CONTROL_VALID && control->scene_id !=
        np2video_golden_scene_id) {
        return "invalid_control_block";
    }
    if (status == NP2V_CONTROL_VALID && control->state == NP2V_STATE_ERROR) {
        return "guest_reported_error";
    }
    return (status == NP2V_CONTROL_VALID &&
            control->state == NP2V_STATE_SCENE_READY) ? "" : NULL;
}

static bool np2video_final_snapshot_matches(
    const SCRNMNG_SNAPSHOT *snapshot, uint32_t generation, uint32_t sequence)
{
    return snapshot != NULL && snapshot->width == (int)np2video_golden_width &&
           snapshot->height == (int)np2video_golden_height &&
           snapshot->bpp == np2video_golden_bpp &&
           snapshot->pixel_format == SCRNMNG_PIXEL_FORMAT_RGB565LE &&
           snapshot->pitch == np2video_golden_pitch &&
           snapshot->visible_bytes == np2video_golden_visible_bytes &&
           snapshot->crc_algorithm ==
               np2video_golden_crc_algorithm_iso_hdlc &&
           snapshot->crc_version == np2video_golden_crc_version &&
           snapshot->crc32 == np2video_golden_crc32 &&
           snapshot->surface_generation == generation &&
           snapshot->surface_update_sequence > sequence;
}

static void np2video_task(void *argument)
{
    np2video_runner_task_state *state = argument;
    np2_fixture fixture;
    np2v_control control;
    SCRNMNG_SNAPSHOT final_snapshot;
    np2video_runner_result result;
    const char *failure = NULL;
    char digest[65];
    uint32_t ready_generation = 0;
    uint32_t ready_sequence = 0;
    uint32_t current_generation = 0;
    uint32_t current_sequence = 0;
    uint32_t cooperate_calls = 0;
    uint32_t slice;
    bool core_initialized = false;
    bool framebuffer_initialized = false;
    bool update_observed = false;
    esp_err_t fixture_error;
#if defined(NP2VIDEO_BENCHMARK_PROFILE)
    uint64_t pccore_exec_count = 0;
    uint64_t pccore_exec_first_us = 0;
    uint64_t pccore_exec_min_us = UINT64_MAX;
    uint64_t pccore_exec_max_us = 0;
    uint64_t pccore_exec_total_us = 0;
#endif

    np2_fixture_init(&fixture);
    memset(&control, 0, sizeof(control));
    memset(&final_snapshot, 0, sizeof(final_snapshot));
    memset(&result, 0, sizeof(result));
    np2video_sha256_text(digest);

    np2video_emit(state,
                  "NP2VIDEO_PROFILE profile=esp32p4-reduced-video "
                  "formal_extmem=%u effective_extmem=%u\n",
                  NP2VIDEO_FORMAL_EXTMEM_MB,
                  NP2VIDEO_EFFECTIVE_EXTMEM_MB);

    np2_stage1_configure_machine();
    np2cfg.EXTMEM = NP2VIDEO_EFFECTIVE_EXTMEM_MB;
    np2_host_taskmng_reset();
    pccore_init();
    core_initialized = true;
    pccore_reset();

    if (!np2video_verify_extmem(state)) {
        failure = (i286core.e.extsize !=
                   NP2VIDEO_EFFECTIVE_EXTMEM_MB * 1024U * 1024U) ?
                      "extmem_size_mismatch" : "extmem_not_external";
        goto cleanup;
    }
    if (!scrnmng_initialize()) {
        failure = "framebuffer_failure";
        goto cleanup;
    }
    framebuffer_initialized = true;
    if (!np2video_verify_framebuffer_contract()) {
        failure = "framebuffer_not_external";
        goto cleanup;
    }
    if (state->ready != NULL && !state->ready(state->lifecycle_context)) {
        failure = "integration_ready_failed";
        goto cleanup;
    }

    fixture_error = np2_fixture_acquire_for(&fixture,
                                           &np2video_fixture_descriptor);
    if (fixture_error != ESP_OK) {
        failure = np2video_fixture_failure(fixture_error);
        goto cleanup;
    }
    if (np2_fixture_attach_dosio(&fixture) != ESP_OK ||
        np2_fixture_attach_fdd(&fixture) != ESP_OK) {
        failure = "fixture_attach_failed";
        goto cleanup;
    }
    np2video_emit(state,
                  "NP2VIDEO_FIXTURE fixture_id=%s scene_id=%u fixture_sha256=%s "
                  "image_bytes=%lu partition=%s\n",
                  np2video_golden_fixture_id,
                  (unsigned)np2video_golden_scene_id, digest,
                  (unsigned long)np2video_golden_fixture_image_size,
                  NP2_FIXTURE_PARTITION_LABEL);

    for (slice = 0; slice < NP2VIDEO_READY_SLICE_LIMIT; ++slice) {
        const char *control_failure;

        pccore_exec(FALSE);
        if (scrnmng_haserror()) {
            failure = "framebuffer_failure";
            goto cleanup;
        }
        control_failure = np2video_control_failure(
            mem + NP2V_CONTROL_PHYSICAL_ADDRESS, &control);
        if (control_failure != NULL && control_failure[0] != '\0') {
            failure = control_failure;
            goto cleanup;
        }
        if (control_failure != NULL && control_failure[0] == '\0') {
            update_observed = true;
            break;
        }
    }
    if (!update_observed) {
        failure = "scene_ready_not_reached";
        goto cleanup;
    }
    if (!np2video_verify_framebuffer_contract()) {
        failure = "framebuffer_metadata_mismatch";
        goto cleanup;
    }
    scrnmng_get_surface_counters(&ready_generation, &ready_sequence);
    np2video_emit(state,
                  "NP2VIDEO_READY fixture_id=%s scene_id=%u state=SCENE_READY "
                  "generation=%u surface_update_sequence=%u\n",
                  np2video_golden_fixture_id,
                  (unsigned)np2video_golden_scene_id,
                  (unsigned)ready_generation,
                  (unsigned)ready_sequence);
    if (state->scene_ready != NULL) {
        state->scene_ready(ready_generation, ready_sequence,
                           state->lifecycle_context);
    }

#if defined(NP2VIDEO_BENCHMARK_PROFILE)
    /* The benchmark producer is deliberately free-running.  The consumer
     * requests termination asynchronously; this task observes that request
     * only after pccore_exec(TRUE) returns at its normal screen/event slice. */
    {
        bool stop_observed = false;

        for (slice = 0; slice < UINT32_C(1048576); ++slice) {
            const char *control_failure;
            const uint64_t pccore_exec_start_us =
                (uint64_t)esp_timer_get_time();

            pccore_exec(TRUE);
            const uint64_t pccore_exec_wall_us =
                (uint64_t)esp_timer_get_time() - pccore_exec_start_us;
            if (pccore_exec_count == 0U) {
                pccore_exec_first_us = pccore_exec_wall_us;
            }
            ++pccore_exec_count;
            pccore_exec_total_us += pccore_exec_wall_us;
            if (pccore_exec_wall_us < pccore_exec_min_us) {
                pccore_exec_min_us = pccore_exec_wall_us;
            }
            if (pccore_exec_wall_us > pccore_exec_max_us) {
                pccore_exec_max_us = pccore_exec_wall_us;
            }
            /* This benchmark-only cooperation point is after a complete NP2
             * event/frame slice.  It is not presentation backpressure and is
             * outside the pccore_exec_wall_us interval above. */
            np2_host_taskmng_cooperate();
            ++cooperate_calls;
            if (scrnmng_haserror()) {
                failure = "framebuffer_failure";
                goto cleanup;
            }
            control_failure = np2video_control_failure(
                mem + NP2V_CONTROL_PHYSICAL_ADDRESS, &control);
            if (control_failure != NULL && control_failure[0] != '\0') {
                failure = control_failure;
                goto cleanup;
            }
            if (control_failure == NULL) {
                failure = "invalid_control_block";
                goto cleanup;
            }
            scrnmng_get_surface_counters(&current_generation, &current_sequence);
            if (current_generation != ready_generation) {
                failure = "unexpected_resize";
                goto cleanup;
            }
            if (current_sequence > ready_sequence) {
                update_observed = true;
            }
            if (state->stop_requested != NULL &&
                state->stop_requested(state->lifecycle_context)) {
                stop_observed = true;
                break;
            }
        }
        if (!stop_observed) {
            failure = "benchmark_stop_timeout";
            goto cleanup;
        }
        if (!update_observed || scrnmng_snapshot(&final_snapshot) !=
            SCRNMNG_SNAPSHOT_OK) {
            failure = "benchmark_frame_not_observed";
            goto cleanup;
        }
        np2video_emit(state,
                      "NP2VIDEO_BENCHMARK_STOP observed=1 generation=%u "
                      "surface_update_sequence=%u\n",
                      (unsigned)final_snapshot.surface_generation,
                      (unsigned)final_snapshot.surface_update_sequence);
    }
#else
    update_observed = false;
    for (slice = 0; slice < NP2VIDEO_POST_READY_SLICE_LIMIT; ++slice) {
        const char *control_failure;

        pccore_exec(TRUE);
        if (scrnmng_haserror()) {
            failure = "framebuffer_failure";
            goto cleanup;
        }
        control_failure = np2video_control_failure(
            mem + NP2V_CONTROL_PHYSICAL_ADDRESS, &control);
        if (control_failure != NULL && control_failure[0] != '\0') {
            failure = control_failure;
            goto cleanup;
        }
        if (control_failure == NULL) {
            failure = "invalid_control_block";
            goto cleanup;
        }
        scrnmng_get_surface_counters(&current_generation, &current_sequence);
        if (current_generation != ready_generation) {
            failure = "unexpected_resize";
            goto cleanup;
        }
        if (current_sequence > ready_sequence) {
            update_observed = true;
            break;
        }
    }
    if (!update_observed) {
        failure = "render_update_not_reached";
        goto cleanup;
    }
    if (scrnmng_snapshot(&final_snapshot) != SCRNMNG_SNAPSHOT_OK) {
        failure = "snapshot_failed";
        goto cleanup;
    }
    if (!np2video_final_snapshot_matches(&final_snapshot,
                                         ready_generation, ready_sequence)) {
        if (final_snapshot.crc32 != np2video_golden_crc32) {
            failure = "framebuffer_crc_mismatch";
        } else {
            failure = "framebuffer_metadata_mismatch";
        }
        goto cleanup;
    }
    {
        SCRNMNG_STATUS status;

        scrnmng_getstatus(&status);
        np2video_emit(state,
                      "NP2VIDEO_FRAMEBUFFER fixture_id=%s scene_id=%u "
                      "width=%d height=%d "
                      "bytes=%lu format=rgb565le bpp=%u pitch=%lu "
                      "generation=%u surface_update_sequence=%u "
                      "crc_algorithm=crc32_iso_hdlc crc32=0x%08lx "
                      "storage_external=%d\n",
                      np2video_golden_fixture_id,
                      (unsigned)np2video_golden_scene_id,
                      final_snapshot.width, final_snapshot.height,
                      (unsigned long)final_snapshot.visible_bytes,
                      (unsigned)final_snapshot.bpp,
                      (unsigned long)final_snapshot.pitch,
                      (unsigned)final_snapshot.surface_generation,
                      (unsigned)final_snapshot.surface_update_sequence,
                      (unsigned long)final_snapshot.crc32,
                      status.external ? 1 : 0);
        if (!status.external) {
            failure = "framebuffer_not_external";
            goto cleanup;
        }
    }
#endif

cleanup:
    if (fixture.fdd_attached) {
        np2_fixture_detach_fdd(&fixture);
    }
    if (core_initialized) {
        pccore_term();
    }
    if (framebuffer_initialized) {
        if (state->stopping != NULL) {
            state->stopping(state->lifecycle_context);
        }
        scrnmng_shutdown();
    }
    np2_fixture_release(&fixture);
    if (failure == NULL) {
        result.status = ESP_OK;
        result.source_generation = final_snapshot.surface_generation;
        result.source_update_sequence = final_snapshot.surface_update_sequence;
        result.source_crc32 = final_snapshot.crc32;
        result.width = (uint32_t)final_snapshot.width;
        result.height = (uint32_t)final_snapshot.height;
        result.bpp = final_snapshot.bpp;
        result.pitch = (uint32_t)final_snapshot.pitch;
        result.visible_bytes = (uint32_t)final_snapshot.visible_bytes;
#if defined(NP2VIDEO_BENCHMARK_PROFILE)
        np2video_emit(state, "NP2VIDEO_BENCHMARK_RESULT=PASS\n");
#else
        np2video_emit(state, "NP2VIDEO_GOLDEN_RESULT=PASS\n");
#endif
    } else {
        result.status = ESP_FAIL;
#if defined(NP2VIDEO_BENCHMARK_PROFILE)
        np2video_emit(state, "NP2VIDEO_BENCHMARK_RESULT=FAIL reason=%s\n",
                      failure);
#else
        np2video_emit(state, "NP2VIDEO_GOLDEN_RESULT=FAIL reason=%s\n",
                      failure);
#endif
    }
    result.cooperate_calls = cooperate_calls;
#if defined(NP2VIDEO_BENCHMARK_PROFILE)
    result.pccore_exec_count = pccore_exec_count;
    result.pccore_exec_first_us = pccore_exec_first_us;
    result.pccore_exec_min_us =
        pccore_exec_count == 0U ? 0U : pccore_exec_min_us;
    result.pccore_exec_max_us = pccore_exec_max_us;
    result.pccore_exec_total_us = pccore_exec_total_us;
#endif
    if (state->complete != NULL) {
        state->complete(&result, state->complete_context);
    }
    vTaskDelete(NULL);
}

esp_err_t np2video_runner_start(np2video_runner_output_fn output,
                                void *output_context)
{
    const np2video_runner_config config = {
        .output = output,
        .output_context = output_context,
        .ready = NULL,
        .scene_ready = NULL,
        .stopping = NULL,
        .complete = NULL,
        .complete_context = NULL,
        .lifecycle_context = NULL,
        .stop_requested = NULL,
        .task_scheduling_override = false,
        .task_core_id = 0,
        .task_priority = 0,
    };
    return np2video_runner_start_ex(&config);
}

esp_err_t np2video_runner_start_ex(const np2video_runner_config *config)
{
    BaseType_t task_result;

    if (config == NULL || config->output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (np2video_runner_started) {
        return ESP_ERR_INVALID_STATE;
    }
    np2video_task_config.output = config->output;
    np2video_task_config.output_context = config->output_context;
    np2video_task_config.ready = config->ready;
    np2video_task_config.scene_ready = config->scene_ready;
    np2video_task_config.stopping = config->stopping;
    np2video_task_config.complete = config->complete;
    np2video_task_config.complete_context = config->complete_context;
    np2video_task_config.lifecycle_context = config->lifecycle_context;
    np2video_task_config.stop_requested = config->stop_requested;
    if (config->task_scheduling_override) {
        if (config->task_core_id < 0 ||
            config->task_core_id >= configNUMBER_OF_CORES ||
            config->task_priority >= (uint32_t)configMAX_PRIORITIES) {
            return ESP_ERR_INVALID_ARG;
        }
        task_result = xTaskCreatePinnedToCore(
            np2video_task, "np2video_runner", NP2VIDEO_RUNNER_STACK_BYTES,
            &np2video_task_config, (UBaseType_t)config->task_priority, NULL,
            (BaseType_t)config->task_core_id);
    } else {
        task_result = xTaskCreate(np2video_task, "np2video_runner",
                                  NP2VIDEO_RUNNER_STACK_BYTES,
                                  &np2video_task_config,
                                  NP2VIDEO_RUNNER_PRIORITY, NULL);
    }
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    np2video_runner_started = 1;
    return ESP_OK;
}
