#include "p4_nano_audio86_live_service_profile.h"

#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE)

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p4_nano_audio86_live_service.h"

struct profile_sink {
    uint64_t frames;
    uint32_t slots;
    uint32_t next_sequence;
    uint64_t next_offset;
    uint32_t started;
    uint32_t finished;
    uint32_t aborted;
    uint32_t worker_core;
};

static struct p4_nano_audio86_live_service s_service;
static struct profile_sink s_sink;
static _Atomic uint32_t s_owner_ready;
static _Atomic uint32_t s_owner_done;
static _Atomic uint32_t s_owner_error;
static _Atomic uint32_t s_owner_core;
static StaticTask_t s_owner_tcb;
static StackType_t s_owner_stack[4096U / sizeof(StackType_t)];
static TaskHandle_t s_owner_task;

static esp_err_t profile_fail(const char *stage)
{
    struct p4_nano_audio86_live_status status;
    p4_nano_audio86_live_service_status(&s_service, &status);
    printf("P4_AUDIO86_LIVE_SERVICE_FAILURE stage=%s state=%u category=%u "
           "origin=%u subcode=%" PRIu32 " owner_error=%" PRIu32 " "
           "rendered=%" PRIu64 " final_horizon=%" PRIu64 " accepted=%"
           PRIu64 " guest=%" PRIu32 " sink=%" PRIu32 "\n",
           stage, (unsigned)status.state, (unsigned)status.category,
           (unsigned)status.origin, status.subcode,
           atomic_load_explicit(&s_owner_error, memory_order_acquire),
           status.rendered_frames, status.final_horizon,
           status.accepted_frames, status.guest_attached,
           status.sink_reachable);
    return ESP_FAIL;
}

static enum np2_pcm_sink_result profile_start(void *opaque)
{
    struct profile_sink *sink = opaque;
    sink->worker_core = (uint32_t)xPortGetCoreID();
    if (sink->worker_core != 0U)
        return NP2_PCM_SINK_FATAL;
    sink->started = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result profile_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    struct profile_sink *sink = opaque;
    if (view == NULL || view->sequence != sink->next_sequence ||
        view->frame_offset != sink->next_offset || view->valid_frames == 0U ||
        view->valid_frames > NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES)
        return NP2_PCM_SINK_FATAL;
    sink->frames += view->valid_frames;
    sink->next_offset += view->valid_frames;
    ++sink->next_sequence;
    ++sink->slots;
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result profile_finish(void *opaque)
{
    struct profile_sink *sink = opaque;
    /* This virtual backend has no callback source.  ACCEPTED therefore proves
     * both no-new-callback and zero in-flight callbacks. */
    sink->finished = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result profile_abort(void *opaque)
{
    struct profile_sink *sink = opaque;
    sink->aborted = 1U;
    return NP2_PCM_SINK_ACCEPTED;
}

static void owner_entry(void *opaque)
{
    struct p4_nano_audio86_live_service *service = opaque;
    atomic_store_explicit(&s_owner_core, (uint32_t)xPortGetCoreID(),
                          memory_order_release);
    if (atomic_load_explicit(&s_owner_core, memory_order_acquire) != 1U) {
        atomic_store_explicit(&s_owner_error, 4U, memory_order_release);
        atomic_store_explicit(&s_owner_done, 1U, memory_order_release);
        vTaskSuspend(NULL);
    }
    np2audio86_guest_opna_unbind();
    np2audio86_guest_host_set_clock(49152000U, 1U);
    np2audio86_guest_host_set_cpumode(0U);
    np2audio86_guest_host_set_cpu_position(0U);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608, 0U, 1U, 2U);
    np2audio86_guest_opna_set_config(3U, 0U);
    np2audio86_guest_opna_bind();
    np2audio86_guest_pcm86_stream_bind();
    if (p4_nano_audio86_live_service_attach_guest(service) !=
            P4_NANO_AUDIO86_LIVE_OK ||
        p4_nano_audio86_live_service_owner_checkpoint(service) !=
            P4_NANO_AUDIO86_LIVE_OK) {
        atomic_store_explicit(&s_owner_error, 1U, memory_order_release);
        atomic_store_explicit(&s_owner_done, 1U, memory_order_release);
        vTaskSuspend(NULL);
    }
    np2audio86_guest_host_set_cpu_position(4U * 1024U);
    np2audio86_guest_opna_write_address_low(0x28U);
    np2audio86_guest_opna_write_data_low(0xf0U);
    np2audio86_guest_host_set_cpu_position(13U * 1024U);
    if (p4_nano_audio86_live_service_owner_checkpoint(service) !=
        P4_NANO_AUDIO86_LIVE_OK)
        atomic_store_explicit(&s_owner_error, 2U, memory_order_release);
    atomic_store_explicit(&s_owner_ready, 1U, memory_order_release);
    while (atomic_load_explicit(&service->stop_intent,
                                memory_order_acquire) == 0U)
        vTaskDelay(1U);
    if (p4_nano_audio86_live_service_owner_checkpoint(service) !=
        P4_NANO_AUDIO86_LIVE_OK)
        atomic_store_explicit(&s_owner_error, 3U, memory_order_release);
    atomic_store_explicit(&s_owner_done, 1U, memory_order_release);
    vTaskSuspend(NULL);
}

esp_err_t p4_nano_audio86_live_service_run_profile(void)
{
    const struct np2_pcm_sink pcm_sink = {
        &s_sink, profile_start, profile_submit, profile_finish, profile_abort};
    const struct p4_nano_audio86_live_config config = {&pcm_sink};
    struct p4_nano_audio86_live_status status;
    uint32_t timeout;

    memset(&s_service, 0, sizeof(s_service));
    memset(&s_sink, 0, sizeof(s_sink));
    atomic_init(&s_owner_ready, 0U);
    atomic_init(&s_owner_done, 0U);
    atomic_init(&s_owner_error, 0U);
    atomic_init(&s_owner_core, UINT32_MAX);
    if (p4_nano_audio86_live_service_init(&s_service, &config) !=
            P4_NANO_AUDIO86_LIVE_OK ||
        p4_nano_audio86_live_service_start(&s_service) !=
            P4_NANO_AUDIO86_LIVE_OK)
        return profile_fail("init-start");
    s_owner_task = xTaskCreateStaticPinnedToCore(
        owner_entry, "audio86_owner",
        sizeof(s_owner_stack) / sizeof(s_owner_stack[0]), &s_service,
        tskIDLE_PRIORITY + 5U, s_owner_stack, &s_owner_tcb, 1);
    if (s_owner_task == NULL)
        return profile_fail("owner-create");
    for (timeout = 0U; timeout < 5000U &&
                       atomic_load_explicit(&s_owner_ready,
                                            memory_order_acquire) == 0U;
         ++timeout)
        vTaskDelay(1U);
    if (timeout == 5000U ||
        atomic_load_explicit(&s_owner_error, memory_order_acquire) != 0U ||
        p4_nano_audio86_live_service_request_stop(&s_service) !=
            P4_NANO_AUDIO86_LIVE_OK)
        return profile_fail("owner-ready-stop");
    for (timeout = 0U; timeout < 5000U &&
                       atomic_load_explicit(&s_owner_done,
                                            memory_order_acquire) == 0U;
         ++timeout)
        vTaskDelay(1U);
    if (timeout == 5000U ||
        atomic_load_explicit(&s_owner_error, memory_order_acquire) != 0U ||
        p4_nano_audio86_live_service_join(&s_service, 5000U, &status) !=
            P4_NANO_AUDIO86_LIVE_OK)
        return profile_fail("owner-done-join");
    if (status.state != P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT ||
        status.rendered_frames != 13U || status.final_horizon != 13U ||
        status.accepted_frames != 13U || status.guest_attached != 0U ||
        status.sink_reachable != 0U || s_sink.started != 1U ||
        s_sink.finished != 1U || s_sink.aborted != 0U ||
        s_sink.frames != 13U || s_sink.slots != 1U ||
        atomic_load_explicit(&s_owner_core, memory_order_acquire) != 1U ||
        s_sink.worker_core != 0U)
        return profile_fail("terminal-contract");
    vTaskDelete(s_owner_task);
    s_owner_task = NULL;
    if (p4_nano_audio86_live_service_destroy(&s_service) !=
        P4_NANO_AUDIO86_LIVE_OK)
        return profile_fail("destroy");
    printf("P4_AUDIO86_LIVE_SERVICE owner_core=1 worker_core=0 "
           "final_horizon=%" PRIu64 " accepted_frames=%" PRIu64
           " slots=%" PRIu32 " partial=1\n",
           status.final_horizon, status.accepted_frames, s_sink.slots);
    printf("P4_AUDIO86_LIVE_SERVICE_RESIDUAL guest=0 sink=0 ownership=0\n");
    printf("P4_AUDIO86_LIVE_SERVICE_RESULT=PASS\n");
    return ESP_OK;
}

#else

esp_err_t p4_nano_audio86_live_service_run_profile(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
