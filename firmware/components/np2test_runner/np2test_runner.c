#include "np2test_runner/np2test_runner.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <compiler.h>
#include <cpumem.h>
#include <cpucore.h>
#include <diskimage/fddfile.h>
#include <pccore.h>

#include <execution_controller.h>
#include <result_v1_parser.h>
#include <stage1_machine_config.h>

#include <np2host/taskmng_esp.h>
#include "np2_fixture.h"

#define NP2_RESULT_PHYSICAL_ADDRESS 0x29000U
#define NP2_FORMAL_EXTMEM_MB 13U
#define NP2_REDUCED_EXTMEM_MB 8U

/* Provisional bring-up value. xTaskCreate() and
 * uxTaskGetStackHighWaterMark() use bytes on ESP32-P4 in ESP-IDF 5.5.x. It is
 * deliberately measured, not treated as a final scheduling-budget decision. */
#define NP2TEST_RUNNER_STACK_BYTES 32768U
#define NP2TEST_RUNNER_PRIORITY (tskIDLE_PRIORITY + 3)

typedef struct {
    np2test_runner_config config;
} np2test_runner_task_config;

static np2test_runner_task_config np2test_runner_task_state;
/* One-shot by design: this task owns the NP2 lifecycle for the firmware
 * lifetime; no restart/reload API is provided. */
static int np2test_runner_started;

static const char *np2test_profile_name(np2test_profile profile)
{
    return (profile == NP2TEST_PROFILE_REDUCED_EXTMEM8) ? "reduced-extmem8" :
                                                          "formal";
}

static const char *np2test_namespace(np2test_profile profile)
{
    return (profile == NP2TEST_PROFILE_REDUCED_EXTMEM8) ? "NP2REDUCED" :
                                                          "NP2TEST";
}

static unsigned np2test_requested_extmem(np2test_profile profile)
{
    return (profile == NP2TEST_PROFILE_REDUCED_EXTMEM8) ?
               NP2_REDUCED_EXTMEM_MB : NP2_FORMAL_EXTMEM_MB;
}

static bool np2test_emit(np2test_runner_task_config *state,
                         const char *format,
                         ...)
{
    char line[512];
    va_list arguments;
    int length;

    if ((state == NULL) || (state->config.output == NULL)) {
        return false;
    }
    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if ((length < 0) || ((size_t)length >= sizeof(line))) {
        return false;
    }
    return state->config.output(state->config.output_context,
                                line, (size_t)length);
}

static void np2test_emit_heap(np2test_runner_task_config *state,
                              const char *prefix)
{
    np2test_emit(state,
                 "%s_MEMORY extmem_mb=%u actual_bytes=%lu ptr_external=%d "
                 "psram_size=%lu free_spiram=%lu largest_spiram=%lu\n",
                 prefix,
                 (unsigned)np2test_requested_extmem(state->config.profile),
                 (unsigned long)i286core.e.extsize,
                 (i286core.e.ext != NULL &&
                  esp_ptr_external_ram(i286core.e.ext)) ? 1 : 0,
                 (unsigned long)esp_psram_get_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static bool np2test_verify_extmem(np2test_runner_task_config *state)
{
    const UINT32 expected_bytes =
        (UINT32)np2test_requested_extmem(state->config.profile) * 1024U * 1024U;
    const bool pointer_external =
        (i286core.e.ext != NULL) && esp_ptr_external_ram(i286core.e.ext);

    np2test_emit_heap(state, np2test_namespace(state->config.profile));
    return (i286core.e.ext != NULL) && pointer_external &&
           (i286core.e.extsize == expected_bytes);
}

static void np2test_emit_snapshot_hex(np2test_runner_task_config *state,
                                      const uint8_t *snapshot)
{
    char line[512];
    size_t offset;
    int length;

    length = snprintf(line, sizeof(line), "%s_SNAPSHOT_HEX=",
                      np2test_namespace(state->config.profile));
    if (length < 0 || (size_t)length >= sizeof(line)) {
        return;
    }
    for (offset = 0; offset < NP2_RESULT_V1_SIZE; ++offset) {
        length += snprintf(line + length, sizeof(line) - (size_t)length,
                           "%02x", (unsigned)snapshot[offset]);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return;
        }
    }
    length += snprintf(line + length, sizeof(line) - (size_t)length, "\n");
    if (length > 0 && (size_t)length < sizeof(line)) {
        state->config.output(state->config.output_context, line, (size_t)length);
    }
}

static uint32_t np2test_read_u32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void np2test_emit_terminal(np2test_runner_task_config *state,
                                  np2_execution_outcome outcome,
                                  const uint8_t *snapshot,
                                  int have_snapshot,
                                  const np2_result_v1_result *parsed,
                                  const np2_execution_controller *controller)
{
    const char *prefix = np2test_namespace(state->config.profile);
    const char *outcome_name = "HARNESS_ERROR";

    switch (outcome) {
        case NP2_EXECUTION_PASS:
            outcome_name = "PASS";
            if (have_snapshot) {
                np2test_emit(state,
                             "%s_PASS completed=%u passed=%u failed=%u "
                             "stored_crc=0x%08x\n",
                             prefix,
                             (unsigned)parsed->completed_count,
                             (unsigned)parsed->passed_count,
                             (unsigned)parsed->failed_count,
                             (unsigned)np2test_read_u32le(
                                 snapshot + NP2_RESULT_V1_CRC_OFFSET));
            }
            break;
        case NP2_EXECUTION_FAIL:
            outcome_name = "FAIL";
            if (have_snapshot) {
                size_t index;
                int length = snprintf(NULL, 0,
                                      "%s_FAIL first_failed_id=0x%04x "
                                      "completed=%u passed=%u failed=%u "
                                      "diagnostic_length=%u diagnostic_hex=",
                                      prefix,
                                      (unsigned)parsed->first_failed_id,
                                      (unsigned)parsed->completed_count,
                                      (unsigned)parsed->passed_count,
                                      (unsigned)parsed->failed_count,
                                      (unsigned)parsed->diagnostic_length);
                char line[512];

                if (length > 0 && (size_t)length < sizeof(line)) {
                    length = snprintf(line, sizeof(line),
                                      "%s_FAIL first_failed_id=0x%04x "
                                      "completed=%u passed=%u failed=%u "
                                      "diagnostic_length=%u diagnostic_hex=",
                                      prefix,
                                      (unsigned)parsed->first_failed_id,
                                      (unsigned)parsed->completed_count,
                                      (unsigned)parsed->passed_count,
                                      (unsigned)parsed->failed_count,
                                      (unsigned)parsed->diagnostic_length);
                    for (index = 0; index < parsed->diagnostic_length; ++index) {
                        length += snprintf(line + length,
                                           sizeof(line) - (size_t)length,
                                           "%02x",
                                           (unsigned)parsed->diagnostic[index]);
                    }
                    if (length > 0 && (size_t)length + 1 < sizeof(line)) {
                        line[length++] = '\n';
                        state->config.output(state->config.output_context,
                                             line, (size_t)length);
                    }
                }
            }
            break;
        case NP2_EXECUTION_NOT_REACHED:
            outcome_name = "NOT_REACHED";
            break;
        case NP2_EXECUTION_RUNNING_TIMEOUT:
            outcome_name = "RUNNING_TIMEOUT";
            break;
        case NP2_EXECUTION_INVALID:
            outcome_name = "INVALID";
            break;
        case NP2_EXECUTION_HARNESS_ERROR:
        default:
            break;
    }
    np2test_emit(state, "%s_PRE_RUNNING_SLICES=%u\n", prefix,
                 (unsigned)controller->pre_running_slices);
    np2test_emit(state, "%s_RUNNING_SLICES=%u\n", prefix,
                 (unsigned)controller->running_slices);
    if (outcome != NP2_EXECUTION_PASS && outcome != NP2_EXECUTION_FAIL &&
        have_snapshot) {
        np2test_emit_snapshot_hex(state, snapshot);
    }
    /* Emit stack evidence before the terminal marker so bounded harnesses can
     * stop on the marker without losing the measurement. */
    np2test_emit(state, "%s_STACK configured_bytes=%u high_water_bytes=%u\n",
                 prefix,
                 (unsigned)NP2TEST_RUNNER_STACK_BYTES,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    np2test_emit(state, "%s_RESULT=%s\n", prefix, outcome_name);
}

static void np2test_task(void *argument)
{
    np2test_runner_task_config *state = argument;
    np2_fixture fixture;
    np2_result_v1_result parsed;
    np2_execution_controller controller;
    np2_execution_outcome outcome = NP2_EXECUTION_CONTINUE;
    uint8_t snapshot[NP2_RESULT_V1_SIZE];
    bool core_initialized = false;
    bool have_snapshot = false;
    bool emitted_running = false;
    esp_err_t fixture_error;

    memset(&fixture, 0, sizeof(fixture));
    memset(&parsed, 0, sizeof(parsed));
    memset(snapshot, 0, sizeof(snapshot));

    np2test_emit(state,
                 "%s profile=%s formal_extmem=%u effective_extmem=%u\n",
                 np2test_namespace(state->config.profile),
                 np2test_profile_name(state->config.profile),
                 (unsigned)NP2_FORMAL_EXTMEM_MB,
                 (unsigned)np2test_requested_extmem(state->config.profile));

    fixture_error = np2_fixture_acquire(&fixture);
    if (fixture_error != ESP_OK) {
        np2test_emit(state, "%s_RESULT=HARNESS_ERROR reason=fixture_%s\n",
                     np2test_namespace(state->config.profile),
                     np2_fixture_error_name(fixture_error));
        goto runner_done;
    }
    if (np2_fixture_attach_dosio(&fixture) != ESP_OK) {
        np2test_emit(state, "%s_RESULT=HARNESS_ERROR reason=dosio_attach\n",
                     np2test_namespace(state->config.profile));
        goto runner_cleanup;
    }

    /* This task is the sole owner of the NP2 configuration and lifecycle. */
    np2_stage1_configure_machine();
    if (state->config.profile == NP2TEST_PROFILE_REDUCED_EXTMEM8) {
        /* Explicit non-formal experiment; the shared formal helper remains 13. */
        np2cfg.EXTMEM = NP2_REDUCED_EXTMEM_MB;
    }
    np2_host_taskmng_reset();
    pccore_init();
    core_initialized = true;
    pccore_reset();

    if (!np2test_verify_extmem(state)) {
        np2test_emit(state, "%s_RESULT=HARNESS_ERROR reason=extmem_allocation\n",
                     np2test_namespace(state->config.profile));
        goto runner_cleanup;
    }
    if (np2_fixture_attach_fdd(&fixture) != ESP_OK) {
        np2test_emit(state, "%s_RESULT=HARNESS_ERROR reason=fdd_attach\n",
                     np2test_namespace(state->config.profile));
        goto runner_cleanup;
    }
    np2test_emit(state,
                 "%s_FDD_READY path=%s tracks=%u sectors=%u n=%u "
                 "disktype=%u read_only=1\n",
                 np2test_namespace(state->config.profile),
                 NP2_FIXTURE_PATH,
                 (unsigned)fddfile[0].inf.xdf.tracks,
                 (unsigned)fddfile[0].inf.xdf.sectors,
                 (unsigned)fddfile[0].inf.xdf.n,
                 (unsigned)fddfile[0].inf.xdf.disktype);

    np2_execution_controller_init(&controller);
    while (outcome == NP2_EXECUTION_CONTINUE) {
        pccore_exec(FALSE);
        memcpy(snapshot, mem + NP2_RESULT_PHYSICAL_ADDRESS,
               sizeof(snapshot));
        have_snapshot = true;
        const np2_result_v1_observation observation =
            np2_result_v1_parse(snapshot, sizeof(snapshot), &parsed);
        const bool task_exit = np2_host_taskmng_exit_requested() != FALSE;

        if (!emitted_running && observation == NP2_RESULT_V1_RUNNING) {
            np2test_emit(state, "%s_RUNNING first_slice=1\n",
                         np2test_namespace(state->config.profile));
            emitted_running = true;
        }
        outcome = np2_execution_controller_step(&controller, observation,
                                                task_exit);
    }
    np2test_emit_terminal(state, outcome, snapshot, have_snapshot, &parsed,
                          &controller);

runner_cleanup:
    if (fixture.fdd_attached) {
        np2_fixture_detach_fdd(&fixture);
    }
    if (core_initialized) {
        pccore_term();
    }
    np2_fixture_release(&fixture);

runner_done:
    vTaskDelete(NULL);
}

esp_err_t np2test_runner_start(const np2test_runner_config *config)
{
    BaseType_t task_result;

    if ((config == NULL) || (config->output == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (np2test_runner_started) {
        return ESP_ERR_INVALID_STATE;
    }
    np2test_runner_task_state.config = *config;
    task_result = xTaskCreate(np2test_task,
                              "np2test_runner",
                              NP2TEST_RUNNER_STACK_BYTES,
                              &np2test_runner_task_state,
                              NP2TEST_RUNNER_PRIORITY,
                              NULL);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    np2test_runner_started = 1;
    return ESP_OK;
}
