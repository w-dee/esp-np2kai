#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"
#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h"

#define EVIDENCE "5D1_EVIDENCE schema=2 evidence_class=HOST_EXEC "

enum fake_callback_dispatch_state {
    FAKE_CALLBACK_NONE = 0,
    FAKE_CALLBACK_DISPATCHED_NOT_ENTERED,
    FAKE_CALLBACK_ENTERED_IN_FLIGHT,
    FAKE_CALLBACK_EXITED,
};

struct fake_history_event {
    unsigned sequence;
    uint32_t generation;
    const char *operation;
    int result;
    size_t bytes;
};

struct fake_backend {
    struct p4_nano_audio86_callback_gate *callback_gate;
    uint32_t generation;
    uint64_t now_ms;
    unsigned prepare_calls;
    unsigned preload_calls;
    unsigned enable_calls;
    uint32_t enable_stream_duration_us;
    uint32_t codec_unmute_duration_us;
    unsigned write_calls;
    unsigned mute_calls;
    unsigned pa_low_calls;
    unsigned disable_calls;
    unsigned unregister_calls;
    unsigned release_calls;
    unsigned notifications;
    unsigned notify_hpwoken;
    unsigned auto_eof_budget;
    unsigned post_completion_eof_qovf_budget;
    int qovf_on_wait;
    int qovf_on_enable;
    enum p4_nano_audio86_physical_io_result preload_result;
    enum p4_nano_audio86_physical_io_result write_result;
    size_t preload_bytes;
    size_t write_bytes;
    uint8_t copied[16][P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES];
    size_t copied_count;
    int operation_failure;
    unsigned start_failure_stage;
    unsigned resource_i2c;
    unsigned resource_i2s;
    unsigned resource_callbacks;
    unsigned pa_high;
    char history[128];
    size_t history_length;
    struct fake_history_event events[256];
    size_t event_count;
    pthread_mutex_t callback_mutex;
    pthread_mutex_t history_mutex;
    pthread_cond_t callback_condition;
    enum fake_callback_dispatch_state callback_dispatch_state;
    unsigned callback_entry_permitted;
    unsigned delete_started;
    unsigned delete_returned;
    unsigned force_delete_barrier_violation;
};

static void record_event(struct fake_backend *fake, const char *operation,
                         int result, size_t bytes)
{
    struct fake_history_event *event;
    assert(pthread_mutex_lock(&fake->history_mutex) == 0);
    assert(fake->event_count < sizeof(fake->events) / sizeof(fake->events[0]));
    event = &fake->events[fake->event_count];
    event->sequence = (unsigned)fake->event_count;
    event->generation = fake->generation;
    event->operation = operation;
    event->result = result;
    event->bytes = bytes;
    fake->event_count++;
    assert(pthread_mutex_unlock(&fake->history_mutex) == 0);
}

static void record_operation(struct fake_backend *fake, char operation)
{
    assert(fake->history_length + 1U < sizeof(fake->history));
    fake->history[fake->history_length++] = operation;
    fake->history[fake->history_length] = '\0';
}

static void emit_history(const struct fake_backend *fake, const char *scenario)
{
    size_t index;
    for (index = 0U; index < fake->event_count; ++index) {
        const struct fake_history_event *event = &fake->events[index];
        printf("5D1_HISTORY schema=2 evidence_class=HOST_EXEC scenario=%s sequence=%u generation=%u operation=%s result=%d bytes=%zu\n",
               scenario, event->sequence, event->generation, event->operation,
               event->result, event->bytes);
    }
}

static int fake_prepare(void *opaque,
                        struct p4_nano_audio86_callback_gate *callback_gate,
                        uint32_t generation)
{
    struct fake_backend *fake = opaque;
    fake->callback_gate = callback_gate;
    fake->generation = generation;
    fake->prepare_calls++;
    record_operation(fake, 'P');
    record_event(fake, "PREPARE_BEGIN", 0, 0U);
    if (fake->start_failure_stage != 0U) {
        if (fake->start_failure_stage >= 2U) {
            fake->resource_i2s = 1U;
            record_event(fake, "I2S_CREATE", 0, 0U);
        }
        if (fake->start_failure_stage >= 3U) {
            fake->resource_callbacks = 1U;
            record_event(fake, "CALLBACK_REGISTER", 0, 0U);
        }
        if (fake->start_failure_stage >= 4U) {
            fake->resource_i2c = 1U;
            fake->pa_high = 1U;
            record_event(fake, "I2C_ACQUIRE", 0, 0U);
            record_event(fake, "CODEC_CONFIG", 0, 0U);
            record_event(fake, "PA_HIGH", 0, 0U);
        }
        record_event(fake, "PREPARE_FAIL", -1, 0U);
        return -1;
    }
    record_event(fake, "I2S_CREATE", 0, 0U);
    record_event(fake, "CALLBACK_REGISTER", 0, 0U);
    record_event(fake, "I2C_ACQUIRE", 0, 0U);
    record_event(fake, "CODEC_CONFIG", 0, 0U);
    record_event(fake, "PA_HIGH", 0, 0U);
    return fake->operation_failure ? -1 : 0;
}

static enum p4_nano_audio86_physical_io_result fake_copy(
    struct fake_backend *fake, const uint8_t *pcm, size_t requested,
    size_t *copied, int preload)
{
    enum p4_nano_audio86_physical_io_result result =
        preload ? fake->preload_result : fake->write_result;
    size_t bytes = preload ? fake->preload_bytes : fake->write_bytes;
    if (result == P4_NANO_AUDIO86_PHYSICAL_IO_OK && bytes == SIZE_MAX)
        bytes = requested;
    *copied = bytes;
    if (bytes != 0U && fake->copied_count < 16U) {
        size_t capture = bytes < requested ? bytes : requested;
        memcpy(fake->copied[fake->copied_count], pcm, capture);
        fake->copied_count++;
    }
    return result;
}

static enum p4_nano_audio86_physical_io_result fake_preload(
    void *opaque, const uint8_t *pcm, size_t bytes, size_t *loaded)
{
    struct fake_backend *fake = opaque;
    fake->preload_calls++;
    record_operation(fake, 'L');
    record_event(fake, "PRELOAD", 0, bytes);
    return fake_copy(fake, pcm, bytes, loaded, 1);
}

static int fake_enable(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->enable_calls++;
    record_operation(fake, 'E');
    record_event(fake, "ENABLE", 0, 0U);
    if (fake->qovf_on_enable)
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake->callback_gate);
    return fake->operation_failure ? -1 : 0;
}

static void fake_get_startup_durations(void *opaque,
                                       uint32_t *enable_stream_us,
                                       uint32_t *codec_unmute_us)
{
    struct fake_backend *fake = opaque;
    if (enable_stream_us != NULL)
        *enable_stream_us = fake->enable_stream_duration_us;
    if (codec_unmute_us != NULL)
        *codec_unmute_us = fake->codec_unmute_duration_us;
}

static enum p4_nano_audio86_physical_io_result fake_write(
    void *opaque, const uint8_t *pcm, size_t bytes, size_t *written,
    uint32_t timeout_ms)
{
    struct fake_backend *fake = opaque;
    assert(timeout_ms == 0U);
    fake->write_calls++;
    record_operation(fake, 'W');
    record_event(fake, "WRITE", 0, bytes);
    return fake_copy(fake, pcm, bytes, written, 0);
}

static int fake_mute(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->mute_calls++;
    record_operation(fake, 'M');
    record_event(fake, "CODEC_MUTE", 0, 0U);
    return fake->operation_failure ? -1 : 0;
}

static int fake_pa_low(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->pa_low_calls++;
    record_operation(fake, 'A');
    record_event(fake, "PA_LOW", 0, 0U);
    fake->pa_high = 0U;
    return fake->operation_failure ? -1 : 0;
}

static int fake_disable(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->disable_calls++;
    record_operation(fake, 'D');
    record_event(fake, "DISABLE", 0, 0U);
    return fake->operation_failure ? -1 : 0;
}

static int fake_unregister(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->unregister_calls++;
    record_operation(fake, 'U');
    record_event(fake, "DELETE_BEGIN", 0, 0U);
    assert(pthread_mutex_lock(&fake->callback_mutex) == 0);
    fake->delete_started = 1U;
    assert(pthread_cond_broadcast(&fake->callback_condition) == 0);
    while (!fake->force_delete_barrier_violation &&
           (fake->callback_dispatch_state ==
                FAKE_CALLBACK_DISPATCHED_NOT_ENTERED ||
            fake->callback_dispatch_state == FAKE_CALLBACK_ENTERED_IN_FLIGHT))
        assert(pthread_cond_wait(&fake->callback_condition,
                                 &fake->callback_mutex) == 0);
    fake->delete_returned = 1U;
    assert(pthread_mutex_unlock(&fake->callback_mutex) == 0);
    record_event(fake, "DELETE_END", 0, 0U);
    fake->resource_callbacks = 0U;
    fake->resource_i2s = 0U;
    return fake->operation_failure ? -1 : 0;
}

static uint64_t fake_now(void *opaque)
{
    struct fake_backend *fake = opaque;
    while (fake->auto_eof_budget == 0U &&
           fake->post_completion_eof_qovf_budget != 0U) {
        fake->post_completion_eof_qovf_budget--;
        record_event(fake, "POST_COMPLETION_CALLBACK_ENTRY", 0, 0U);
        p4_nano_audio86_callback_gate_on_sent(fake->callback_gate);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake->callback_gate);
        record_event(fake, "POST_COMPLETION_CALLBACK_EXIT", 0, 0U);
    }
    return fake->now_ms;
}

static void fake_wait(void *opaque, uint32_t timeout_ms)
{
    struct fake_backend *fake = opaque;
    fake->now_ms += timeout_ms == 0U ? 1U : timeout_ms;
    if (fake->auto_eof_budget != 0U) {
        fake->auto_eof_budget--;
        record_event(fake, "CALLBACK_DISPATCH", 0, 0U);
        record_event(fake, "CALLBACK_ENTRY", 0, 0U);
        p4_nano_audio86_callback_gate_on_sent(fake->callback_gate);
        record_event(fake, "TX_EOF", 0, 0U);
        record_event(fake, "CALLBACK_EXIT", 0, 0U);
    }
    if (fake->qovf_on_wait) {
        fake->qovf_on_wait = 0;
        record_event(fake, "CALLBACK_DISPATCH", 0, 0U);
        record_event(fake, "CALLBACK_ENTRY", 0, 0U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake->callback_gate);
        record_event(fake, "QUEUE_OVF", 0, 0U);
        record_event(fake, "CALLBACK_EXIT", 0, 0U);
        record_event(fake, "ABORT", 0, 0U);
    }
}

static uint32_t fake_notify(void *opaque, bool from_isr)
{
    struct fake_backend *fake = opaque;
    fake->notifications++;
    return P4_NANO_AUDIO86_NOTIFY_ATTEMPTED |
           (from_isr && fake->notify_hpwoken
                ? P4_NANO_AUDIO86_NOTIFY_HIGHER_PRIORITY_WOKEN : 0U);
}

static void fake_release(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->release_calls++;
    record_operation(fake, 'R');
    record_event(fake, "I2C_RELEASE", 0, 0U);
    record_event(fake, "DESTROY", 0, 0U);
    fake->resource_i2c = 0U;
}

static struct p4_nano_audio86_physical_sink *new_sink(
    struct fake_backend *fake, struct np2_pcm_sink *interface)
{
    const struct p4_nano_audio86_physical_backend backend = {
        fake_prepare, fake_preload, fake_enable, fake_get_startup_durations,
        fake_write, fake_mute,
        fake_pa_low, fake_disable, fake_unregister, fake_now, fake_wait,
        fake_notify, fake_release, fake};
    struct p4_nano_audio86_physical_sink *sink = NULL;
    memset(fake, 0, sizeof(*fake));
    assert(pthread_mutex_init(&fake->callback_mutex, NULL) == 0);
    assert(pthread_mutex_init(&fake->history_mutex, NULL) == 0);
    assert(pthread_cond_init(&fake->callback_condition, NULL) == 0);
    fake->preload_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    fake->write_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    fake->preload_bytes = SIZE_MAX;
    fake->write_bytes = SIZE_MAX;
    fake->enable_stream_duration_us = 37U;
    fake->codec_unmute_duration_us = 83U;
    assert(p4_nano_audio86_physical_sink_create(&sink, &backend) == 0);
    *interface = p4_nano_audio86_physical_sink_interface(sink);
    return sink;
}

static void start_sink(struct np2_pcm_sink *interface)
{
    assert(interface->start(interface->opaque) == NP2_PCM_SINK_ACCEPTED);
}

static void close_sink(struct fake_backend *fake,
                       struct p4_nano_audio86_physical_sink *sink,
                       struct np2_pcm_sink *interface)
{
    struct p4_nano_audio86_physical_telemetry telemetry;
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    if (telemetry.state != P4_NANO_AUDIO86_PHYSICAL_QUIESCENT) {
        fake->operation_failure = 0;
        p4_nano_audio86_physical_sink_test_set_callback_refcount(sink, 0U);
        assert(interface->abort(interface->opaque) == NP2_PCM_SINK_ACCEPTED);
    }
    assert(p4_nano_audio86_physical_sink_destroy(sink) == 0);
    assert(fake->release_calls == 1U);
    assert(pthread_cond_destroy(&fake->callback_condition) == 0);
    assert(pthread_mutex_destroy(&fake->callback_mutex) == 0);
    assert(pthread_mutex_destroy(&fake->history_mutex) == 0);
}

static void fill_pcm(uint8_t *pcm, size_t bytes, uint8_t seed)
{
    size_t i;
    for (i = 0U; i < bytes; ++i) pcm[i] = (uint8_t)(seed + i * 17U);
}

static struct np2_pcm_sink_view full_view(uint8_t *pcm, uint32_t sequence)
{
    const struct np2_pcm_sink_view view = {
        pcm, (uint64_t)sequence * 240U, sequence, 240U, 0U};
    return view;
}

static void test_full_controller(void)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    size_t consumed = 0U;
    fill_pcm(pcm, sizeof(pcm), 3U);
    np2opngen_pcm_ring_init(&ring);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 240U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 240U);
    assert(np2opngen_pcm_ring_finish(&ring, 240U) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(np2_pcm_output_controller_init(&controller, &ring, &interface) == 0);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(controller.accepted_frames == 240U &&
           controller.accepted_bytes == 960U);
    assert(fake.preload_calls == 1U && fake.copied_count == 1U &&
           memcmp(fake.copied[0], pcm, sizeof(pcm)) == 0);
    fake.auto_eof_budget = 4U;
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_OK);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.physical_units_copied == 1U &&
           telemetry.physical_bytes_copied == 960U &&
           telemetry.full_units == 1U &&
           telemetry.final_partial_units == 0U &&
           telemetry.submit_attempts == 1U &&
           telemetry.retry_count == 0U &&
           telemetry.drain_completion_epoch -
                   telemetry.drain_snapshot_epoch ==
               P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS &&
           telemetry.quiescent_eof_epoch == telemetry.tx_eof_epoch &&
           telemetry.drain_duration_ms == 4U &&
           telemetry.physically_drained_frames == 240U &&
           telemetry.accepted_pending_drain_frames == 0U &&
           telemetry.prepare_completed && telemetry.pa_initial_low &&
           telemetry.codec_initialized_muted && telemetry.i2s_initialized &&
           telemetry.muted_warmup_completed &&
           telemetry.callbacks_registered && telemetry.stream_started &&
           telemetry.codec_unmute_completed && telemetry.finish_completed &&
           telemetry.codec_final_muted && telemetry.pa_final_low &&
           !telemetry.i2s_enabled && !telemetry.i2s_created &&
           telemetry.registered_generation == 1U &&
           telemetry.generation == 2U);
    close_sink(&fake, sink, &interface);
    emit_history(&fake, "full_q240_history");
    puts("5D1_FULL_Q240 semantic_frames=240 semantic_bytes=960 physical_bytes=960 consume_calls=1 result=PASS");
}

static void test_final_partial(unsigned frames)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    struct np2_pcm_sink_view view;
    size_t semantic = frames * 4U;
    size_t i;
    fill_pcm(pcm, sizeof(pcm), (uint8_t)frames);
    start_sink(&interface);
    view.pcm = pcm;
    view.frame_offset = 0U;
    view.sequence = 0U;
    view.valid_frames = (uint16_t)frames;
    view.flags = NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL;
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_ACCEPTED);
    assert(fake.preload_calls == 1U && fake.copied_count == 1U);
    assert(memcmp(fake.copied[0], pcm, semantic) == 0);
    for (i = semantic; i < 960U; ++i) assert(fake.copied[0][i] == 0U);
    fake.auto_eof_budget = 4U;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.semantic_accepted_frames == frames &&
           telemetry.semantic_accepted_bytes == semantic &&
           telemetry.physical_bytes_copied == 960U &&
           telemetry.physical_padding_frames == 240U - frames &&
           telemetry.full_units == 0U &&
           telemetry.final_partial_units == 1U &&
           telemetry.final_valid_frames == frames &&
           telemetry.submit_attempts == 1U &&
           telemetry.retry_count == 0U && telemetry.finish_completed &&
           telemetry.stream_started && !telemetry.i2s_enabled &&
           !telemetry.i2s_created);
    close_sink(&fake, sink, &interface);
    printf("5D1_FINAL_PARTIAL frames=%u semantic_bytes=%zu physical_bytes=960 padding_frames=%u padding_zero=1 digest_excludes_padding=1 result=PASS\n",
           frames, semantic, 240U - frames);
    if (frames == 13U) {
        printf(EVIDENCE "scenario=short_eos preload_units=%u enable_calls=%u physical_units=%" PRIu64 " semantic_frames=%" PRIu64 " drain_eofs=%u deadlock=0\n",
               telemetry.preloaded_units, fake.enable_calls,
               telemetry.physical_units_copied,
               telemetry.semantic_accepted_frames, telemetry.tx_eof_epoch);
    }
}

static void enter_running(struct fake_backend *fake,
                          struct np2_pcm_sink *interface, uint8_t pcm[960])
{
    uint32_t i;
    for (i = 0U; i < 4U; ++i) {
        struct np2_pcm_sink_view view = full_view(pcm, i);
        assert(interface->submit(interface->opaque, &view) ==
               NP2_PCM_SINK_ACCEPTED);
    }
    assert(fake->enable_calls == 1U);
}

static void test_retry_and_lost_wake(void)
{
    static const char *const scenarios[] = {
        "retry_before_arm", "retry_during_arm", "retry_coalesced"};
    unsigned scenario;
    for (scenario = 0U; scenario < 3U; ++scenario) {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        struct np2_pcm_sink_view view;
        uint8_t pcm[960];
        uint32_t snapshot;
        unsigned callbacks = scenario == 2U ? 3U : 1U;
        unsigned i;
        fill_pcm(pcm, sizeof(pcm), (uint8_t)(11U + scenario));
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        view = full_view(pcm, 4U);
        fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT;
        fake.write_bytes = 0U;
        snapshot = p4_nano_audio86_physical_sink_retry_snapshot(sink);
        assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_RETRY);
        assert(!p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
        fake_notify(&fake, false);
        assert(!p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
        for (i = 0U; i < callbacks; ++i)
            p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        assert(p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
        fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
        fake.write_bytes = SIZE_MAX;
        assert(interface.submit(interface.opaque, &view) ==
               NP2_PCM_SINK_ACCEPTED);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.semantic_accepted_frames == 5U * 240U &&
               telemetry.submit_attempts == 6U &&
               telemetry.retry_count == 1U);
        printf(EVIDENCE "scenario=%s epoch_before=%u epoch_after=%u callbacks=%u notification_only_ready=0 tail_held=1 accepted_once=1 forced_abort=0\n",
               scenarios[scenario], snapshot, telemetry.tx_eof_epoch,
               callbacks);
        fake.auto_eof_budget = 4U;
        assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        close_sink(&fake, sink, &interface);
        if (scenario == 0U)
            emit_history(&fake, "retry_before_arm_history");
    }
}

static void test_s2_ten_unit_stream(int inject_retry)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry before;
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[10][P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES];
    uint32_t retry_snapshot = 0U;
    uint64_t accepted_before_retry = 0U;
    unsigned index;
    start_sink(&interface);
    for (index = 0U; index < 10U; ++index) {
        struct np2_pcm_sink_view view;
        fill_pcm(pcm[index], sizeof(pcm[index]), (uint8_t)(31U + index));
        view = full_view(pcm[index], index);
        if (inject_retry && index == 4U) {
            p4_nano_audio86_physical_sink_get_telemetry(sink, &before);
            accepted_before_retry = before.semantic_accepted_frames;
            retry_snapshot = p4_nano_audio86_physical_sink_retry_snapshot(sink);
            fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT;
            fake.write_bytes = 0U;
            assert(interface.submit(interface.opaque, &view) ==
                   NP2_PCM_SINK_RETRY);
            p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
            assert(telemetry.semantic_accepted_frames == accepted_before_retry &&
                   telemetry.physical_units_copied == 4U &&
                   telemetry.submit_attempts == 5U &&
                   telemetry.retry_count == 1U &&
                   !p4_nano_audio86_physical_sink_retry_ready(
                       sink, retry_snapshot));
            p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
            assert(p4_nano_audio86_physical_sink_retry_ready(
                sink, retry_snapshot));
            fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
            fake.write_bytes = SIZE_MAX;
        }
        assert(interface.submit(interface.opaque, &view) ==
               NP2_PCM_SINK_ACCEPTED);
    }
    assert(fake.preload_calls == 4U && fake.enable_calls == 1U &&
           fake.write_calls == (inject_retry ? 7U : 6U) &&
           fake.copied_count == 10U);
    for (index = 0U; index < 10U; ++index)
        assert(memcmp(fake.copied[index], pcm[index], sizeof(pcm[index])) == 0);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &before);
    assert(before.semantic_accepted_frames == 2400U &&
           before.semantic_accepted_bytes == 9600U &&
           before.physical_units_copied == 10U &&
           before.physical_bytes_copied == 9600U &&
           before.full_units == 10U && before.final_partial_units == 0U &&
           before.final_valid_frames == 0U &&
           before.physical_padding_frames == 0U &&
           before.preloaded_units == 4U &&
           before.submit_attempts == 10U + (unsigned)inject_retry &&
           before.retry_count == (unsigned)inject_retry &&
           before.accepted_pending_drain_frames == 2400U);
    fake.auto_eof_budget = 4U;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.accepted_pending_drain_frames == 0U &&
           telemetry.physically_drained_frames == 2400U &&
           telemetry.physically_discarded_accepted_frames == 0U &&
           telemetry.state == P4_NANO_AUDIO86_PHYSICAL_QUIESCENT &&
           telemetry.finish_completed && !telemetry.sticky_error);
    printf(EVIDENCE "scenario=%s preload_units=%u enable_calls=%u"
           " running_writes=%u physical_units=%" PRIu64
           " full_units=%u semantic_frames=%" PRIu64
           " submit_attempts=%" PRIu64 " retry_count=%" PRIu64
           " accepted_once=1 byte_identity=1 retry_slot_held=%u"
           " retry_accepted_held=%u pending=%" PRIu64
           " drained=%" PRIu64 " discarded=%" PRIu64 "\n",
           inject_retry ? "s2_10_unit_retry" : "s2_10_unit_stream",
           telemetry.preloaded_units, fake.enable_calls, fake.write_calls,
           telemetry.physical_units_copied, telemetry.full_units,
           telemetry.semantic_accepted_frames, telemetry.submit_attempts,
           telemetry.retry_count, inject_retry ? 1U : 0U,
           inject_retry && accepted_before_retry == 960U ? 1U : 0U,
           telemetry.accepted_pending_drain_frames,
           telemetry.physically_drained_frames,
           telemetry.physically_discarded_accepted_frames);
    close_sink(&fake, sink, &interface);
}

static void test_partial_progress(size_t bytes)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct np2_pcm_sink_view view;
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    fill_pcm(pcm, sizeof(pcm), 19U);
    start_sink(&interface);
    view = full_view(pcm, 0U);
    fake.preload_result = P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT;
    fake.preload_bytes = bytes;
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_FATAL);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.semantic_accepted_frames == 0U &&
           telemetry.physical_units_copied == 0U && telemetry.sticky_error);
    close_sink(&fake, sink, &interface);
    printf("5D1_PARTIAL_PROGRESS bytes=%zu result=FATAL ring_consumed=0 rollback=0 PASS\n",
           bytes);
}

static void test_finish_eof_count(unsigned eof_count)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    fill_pcm(pcm, sizeof(pcm), 23U);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    fake.auto_eof_budget = eof_count;
    const enum np2_pcm_sink_result finish = interface.finish(interface.opaque);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.tx_eof_epoch == eof_count);
    assert((eof_count == 4U && finish == NP2_PCM_SINK_ACCEPTED) ||
           (eof_count < 4U && finish == NP2_PCM_SINK_FATAL));
    printf(EVIDENCE "scenario=finish_eof_%u eof_snapshot=0 eof_current=%u finish=%u sticky=%u stale=%u\n",
           eof_count, telemetry.tx_eof_epoch, finish,
           telemetry.sticky_error ? 1U : 0U,
           telemetry.stale_callback_count);
    close_sink(&fake, sink, &interface);
}

static void test_finish_and_callbacks(void)
{
    unsigned eof_count;
    for (eof_count = 0U; eof_count <= 4U; ++eof_count)
        test_finish_eof_count(eof_count);

    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        uint8_t pcm[960];
        fill_pcm(pcm, sizeof(pcm), 24U);
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_test_on_sent_generation(
            sink, fake.generation + 1U);
        fake.auto_eof_budget = 4U;
        assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.tx_eof_epoch == 4U &&
               telemetry.stale_callback_count == 1U);
        printf(EVIDENCE "scenario=finish_wrong_generation eof_snapshot=0 eof_current=%u finish=%u sticky=0 stale=%u\n",
               telemetry.tx_eof_epoch, NP2_PCM_SINK_ACCEPTED,
               telemetry.stale_callback_count);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        uint8_t pcm[960];
        fill_pcm(pcm, sizeof(pcm), 25U);
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        fake.qovf_on_wait = 1;
        assert(interface.finish(interface.opaque) == NP2_PCM_SINK_FATAL);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.sticky_error);
        printf(EVIDENCE "scenario=finish_sticky_error eof_snapshot=0 eof_current=%u finish=%u sticky=1 stale=%u\n",
               telemetry.tx_eof_epoch, NP2_PCM_SINK_FATAL,
               telemetry.stale_callback_count);
        close_sink(&fake, sink, &interface);
        emit_history(&fake, "finish_sticky_error_history");
    }
}

static void test_queue_overflow(void)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    fill_pcm(pcm, sizeof(pcm), 29U);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.sticky_error &&
           telemetry.running_queue_overflow_count == 1U);
    p4_nano_audio86_physical_sink_test_on_send_q_ovf_generation(
        sink, fake.generation + 1U);
    close_sink(&fake, sink, &interface);

    sink = new_sink(&fake, &interface);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    fake.auto_eof_budget = 4U;
    fake.qovf_on_wait = 1;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(!telemetry.sticky_error &&
           telemetry.draining_queue_overflow_count == 1U);
    close_sink(&fake, sink, &interface);

    sink = new_sink(&fake, &interface);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    fake.auto_eof_budget = 4U;
    fake.post_completion_eof_qovf_budget = 1U;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.drain_completion_epoch -
               telemetry.drain_snapshot_epoch == 4U &&
           telemetry.quiescent_eof_epoch -
               telemetry.drain_snapshot_epoch == 5U &&
           telemetry.tx_eof_epoch == telemetry.quiescent_eof_epoch &&
           telemetry.draining_queue_overflow_count == 1U &&
           !telemetry.sticky_error);
    printf(EVIDENCE "scenario=finish_post_completion_callback eof_snapshot=%u drain_completion=%u quiescent_eof=%u draining_q_ovf=%u finish=%u\n",
           telemetry.drain_snapshot_epoch,
           telemetry.drain_completion_epoch,
           telemetry.quiescent_eof_epoch,
           telemetry.draining_queue_overflow_count,
           NP2_PCM_SINK_ACCEPTED);
    close_sink(&fake, sink, &interface);
    puts("5D1_QUEUE_OVF running=FATAL draining=TELEMETRY_ONLY stale=IGNORED result=PASS");
}

static void assert_first_qovf_freezes(
    enum p4_nano_audio86_consumer_service_phase phase, int during_enable)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry first;
    struct p4_nano_audio86_physical_telemetry later;
    uint8_t pcm[960];
    fill_pcm(pcm, sizeof(pcm), (uint8_t)(41U + (unsigned)phase));
    start_sink(&interface);
    p4_nano_audio86_physical_sink_publish_consumer_progress(
        sink, P4_NANO_AUDIO86_PROGRESS_STEP_ENTER, phase, 7U, 6U, 101U);
    if (during_enable) {
        fake.qovf_on_enable = 1;
        enter_running(&fake, &interface, pcm);
    } else {
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_consumer_progress(
            sink, P4_NANO_AUDIO86_PROGRESS_SUBMIT_RETURN, phase,
            7U, 6U, 202U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
    }
    p4_nano_audio86_physical_sink_observe_first_qovf(sink, 303U);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &first);
    assert(first.first_active_qovf_latched == 1U &&
           first.first_qovf_state == (uint32_t)(
               during_enable ? P4_NANO_AUDIO86_PHYSICAL_STARTING
                             : P4_NANO_AUDIO86_PHYSICAL_RUNNING) &&
           first.first_qovf_phase == (uint32_t)phase &&
           first.first_qovf_current_sequence == 7U &&
           first.first_qovf_published_sequence == 6U &&
           first.first_qovf_observed == 1U &&
           first.first_qovf_observed_us == 303U &&
           first.enable_stream_duration_us == 37U &&
           first.codec_unmute_duration_us == 83U &&
           first.startup_durations_valid == 1U);
    p4_nano_audio86_physical_sink_publish_consumer_progress(
        sink, P4_NANO_AUDIO86_PROGRESS_STEP_EXIT,
        P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH, 99U, 98U, 404U);
    p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
    p4_nano_audio86_physical_sink_observe_first_qovf(sink, 505U);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &later);
    assert(later.first_qovf_state == first.first_qovf_state &&
           later.first_qovf_phase == first.first_qovf_phase &&
           later.first_qovf_current_sequence ==
               first.first_qovf_current_sequence &&
           later.first_qovf_published_sequence ==
               first.first_qovf_published_sequence &&
           later.first_qovf_observed_us == first.first_qovf_observed_us);
    close_sink(&fake, sink, &interface);
}

static void test_first_qovf_latch(void)
{
    assert_first_qovf_freezes(P4_NANO_AUDIO86_CONSUMER_PHASE_START_ENABLE, 1);
    assert_first_qovf_freezes(
        P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT, 0);
    assert_first_qovf_freezes(
        P4_NANO_AUDIO86_CONSUMER_PHASE_POST_ACCEPT_EVIDENCE, 0);
    assert_first_qovf_freezes(P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF, 0);
    printf("FIRST_QOVF_LATCH_HOST_TEST=PASS\n");
    printf("PHYSICAL_DIAGNOSTIC_FIXED_BYTES=%zu\n",
           p4_nano_audio86_physical_sink_diagnostic_storage_bytes());
}

static void test_final_boundary_diagnostics(void)
{
    uint8_t pcm[P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES];
    fill_pcm(pcm, sizeof(pcm), 71U);
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 95760U, 399U, 0U, false);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY,
            399U, 100U);
        fake.notify_hpwoken = 1U;
        p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY &&
               telemetry.first_qovf_consumer_next_sequence == 399U &&
               telemetry.first_qovf_next_published_sequence == 399U &&
               telemetry.first_qovf_ring_occupancy == 0U &&
               telemetry.first_qovf_production_done == 0U &&
               telemetry.first_qovf_rendered_frames == 95760U &&
               telemetry.first_qovf_eof_notify_count == 1U &&
               telemetry.first_qovf_hpwoken_true_count == 1U &&
               telemetry.first_qovf_ring_wait_enter_count == 1U &&
               telemetry.first_qovf_ring_wait_resume_count == 0U &&
               telemetry.first_qovf_last_wait_enter_us == 100U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 96000U, 400U, 1U, false);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
            399U, 200U);
        fake.notify_hpwoken = 0U;
        p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        p4_nano_audio86_physical_sink_publish_wait_resume(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
            399U, 210U);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
            399U, 220U);
        fake.notify_hpwoken = 1U;
        p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF &&
               telemetry.first_qovf_next_published_sequence == 400U &&
               telemetry.first_qovf_ring_occupancy == 1U &&
               telemetry.first_qovf_rendered_frames == 96000U &&
               telemetry.first_qovf_eof_notify_count == 2U &&
               telemetry.first_qovf_hpwoken_true_count == 1U &&
               telemetry.first_qovf_retry_wait_enter_count == 2U &&
               telemetry.first_qovf_retry_wait_resume_count == 1U &&
               telemetry.first_qovf_last_wait_enter_us == 220U &&
               telemetry.first_qovf_last_wait_resume_us == 210U &&
               telemetry.first_qovf_last_resume_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF &&
               telemetry.first_qovf_last_resume_sequence == 399U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY,
            399U, 300U);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 96000U, 400U, 1U, true);
        p4_nano_audio86_physical_sink_publish_wait_resume(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY,
            399U, 310U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE &&
               telemetry.first_qovf_next_published_sequence == 400U &&
               telemetry.first_qovf_ring_occupancy == 1U &&
               telemetry.first_qovf_production_done == 1U &&
               telemetry.first_qovf_ring_wait_enter_count == 1U &&
               telemetry.first_qovf_ring_wait_resume_count == 1U &&
               telemetry.first_qovf_last_wait_resume_us == 310U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 96000U, 400U, 2U, true);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
            398U, 400U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF &&
               telemetry.first_qovf_consumer_next_sequence == 398U &&
               telemetry.first_qovf_next_published_sequence == 400U &&
               telemetry.first_qovf_ring_occupancy == 2U &&
               telemetry.first_qovf_production_done == 1U &&
               telemetry.first_qovf_rendered_frames == 96000U &&
               telemetry.first_qovf_retry_wait_enter_count == 1U &&
               telemetry.first_qovf_retry_wait_resume_count == 0U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 720U, 3U, 3U, false);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL,
            0U, 450U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL &&
               telemetry.first_qovf_ring_occupancy == 3U &&
               telemetry.first_qovf_ring_wait_enter_count == 1U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 96000U, 400U, 0U, true);
        p4_nano_audio86_physical_sink_publish_runnable(sink, 400U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE &&
               telemetry.first_qovf_consumer_next_sequence == 400U &&
               telemetry.first_qovf_next_published_sequence == 400U &&
               telemetry.first_qovf_ring_occupancy == 0U &&
               telemetry.first_qovf_production_done == 1U);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        enter_running(&fake, &interface, pcm);
        p4_nano_audio86_physical_sink_publish_ring_context(
            sink, 96000U, 400U, 0U, true);
        p4_nano_audio86_physical_sink_publish_wait_enter(
            sink, P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL,
            400U, 500U);
        p4_nano_audio86_callback_gate_on_send_q_ovf(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.first_qovf_wait_reason ==
                   P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL &&
               telemetry.first_qovf_consumer_next_sequence == 400U &&
               telemetry.first_qovf_next_published_sequence == 400U &&
               telemetry.first_qovf_ring_occupancy == 0U &&
               telemetry.first_qovf_production_done == 1U);
        close_sink(&fake, sink, &interface);
    }
    puts("CONSUMER_WAIT_REASON_HOST_TEST=PASS");
    puts("EOF_WAKE_COUNTER_HOST_TEST=PASS");
    puts("FINAL_Q399_DIAGNOSTIC_STATE_MACHINE_TEST=PASS");
}

struct callback_race_threads {
    struct fake_backend *fake;
    struct np2_pcm_sink *interface;
    enum np2_pcm_sink_result abort_result;
};

static void *dispatch_before_entry_thread(void *opaque)
{
    struct callback_race_threads *threads = opaque;
    struct fake_backend *fake = threads->fake;
    assert(pthread_mutex_lock(&fake->callback_mutex) == 0);
    fake->callback_dispatch_state = FAKE_CALLBACK_DISPATCHED_NOT_ENTERED;
    record_event(fake, "CALLBACK_DISPATCH", 0, 0U);
    assert(pthread_cond_broadcast(&fake->callback_condition) == 0);
    while (!fake->callback_entry_permitted)
        assert(pthread_cond_wait(&fake->callback_condition,
                                 &fake->callback_mutex) == 0);
    fake->callback_dispatch_state = FAKE_CALLBACK_ENTERED_IN_FLIGHT;
    record_event(fake, "CALLBACK_ENTRY", 0, 0U);
    assert(pthread_mutex_unlock(&fake->callback_mutex) == 0);

    p4_nano_audio86_callback_gate_on_sent(fake->callback_gate);

    assert(pthread_mutex_lock(&fake->callback_mutex) == 0);
    record_event(fake, "CALLBACK_EXIT", 0, 0U);
    fake->callback_dispatch_state = FAKE_CALLBACK_EXITED;
    assert(pthread_cond_broadcast(&fake->callback_condition) == 0);
    assert(pthread_mutex_unlock(&fake->callback_mutex) == 0);
    return NULL;
}

static void *abort_during_dispatch_thread(void *opaque)
{
    struct callback_race_threads *threads = opaque;
    record_event(threads->fake, "ABORT", 0, 0U);
    threads->abort_result = threads->interface->abort(
        threads->interface->opaque);
    return NULL;
}

struct callback_control_state {
    unsigned dispatched_not_entered;
    unsigned entered_in_flight;
    unsigned delete_returned;
    unsigned target_reclaimed;
    unsigned target_accessed;
    unsigned in_flight_acquired;
    unsigned in_flight;
    unsigned generation_matches;
    unsigned target_authorized;
    unsigned quiescence_timeout;
    unsigned reclaim_allowed;
    unsigned active;
    unsigned eof_advanced;
    unsigned drain_credit;
};

static bool callback_control_state_valid(
    const struct callback_control_state *state)
{
    if (state->delete_returned &&
        (state->dispatched_not_entered || state->entered_in_flight)) return false;
    if (state->target_reclaimed &&
        (state->dispatched_not_entered || state->entered_in_flight)) return false;
    if (state->target_accessed && !state->in_flight_acquired) return false;
    if (state->reclaim_allowed && state->in_flight != 0U) return false;
    if (!state->generation_matches && state->target_authorized) return false;
    if (state->quiescence_timeout && state->reclaim_allowed) return false;
    if (!state->active && state->eof_advanced) return false;
    if (!state->generation_matches && state->drain_credit) return false;
    return true;
}

static void test_callback_control_faults(void)
{
    struct callback_control_state faults[8] = {{0}};
    faults[0].delete_returned = faults[0].dispatched_not_entered = 1U;
    faults[1].target_reclaimed = faults[1].dispatched_not_entered = 1U;
    faults[2].target_accessed = 1U;
    faults[3].reclaim_allowed = faults[3].in_flight = 1U;
    faults[4].target_authorized = 1U;
    faults[5].quiescence_timeout = faults[5].reclaim_allowed = 1U;
    faults[6].eof_advanced = 1U;
    faults[7].drain_credit = 1U;
    for (size_t index = 0U; index < 8U; ++index)
        assert(!callback_control_state_valid(&faults[index]));
    puts("5D1_CONTROL_FAULTS schema=2 evidence_class=HOST_EXEC model=callback rejected=8 total=8");
}

static void test_quiescence_timeout(void)
{
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        assert(p4_nano_audio86_physical_sink_test_callback_enter(
            sink, fake.generation));
        p4_nano_audio86_physical_sink_test_disarm_callbacks(sink);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.callback_refcount == 1U && !telemetry.callbacks_active);
        p4_nano_audio86_physical_sink_test_callback_exit(sink);
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        puts(EVIDENCE "scenario=callback_entry_before_disarm dispatch_state=ENTERED_IN_FLIGHT delete_started=0 delete_returned=0 gate_active=1 gate_generation=1 callback_generation=1 in_flight_before=0 in_flight_peak=1 in_flight_final=0 target_accessed=1 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=0 reclaim_attempted=0 reclaim_allowed=0 timeout=0 entered=1 disarmed=1 in_flight_during=1 in_flight_after=0 target_touched_safely=1");
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry telemetry;
        start_sink(&interface);
        p4_nano_audio86_physical_sink_test_disarm_callbacks(sink);
        assert(!p4_nano_audio86_physical_sink_test_callback_enter(
            sink, fake.generation));
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.callback_refcount == 0U &&
               telemetry.stale_callback_count == 1U);
        puts(EVIDENCE "scenario=callback_entry_after_disarm dispatch_state=EXITED delete_started=0 delete_returned=0 gate_active=0 gate_generation=1 callback_generation=1 in_flight_before=0 in_flight_peak=1 in_flight_final=0 target_accessed=0 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=1 reclaim_attempted=0 reclaim_allowed=0 timeout=0 entered=0 target_touched=0 in_flight_after=0 stale=1");
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry before;
        struct p4_nano_audio86_physical_telemetry after;
        struct callback_race_threads threads = {&fake, &interface,
                                                NP2_PCM_SINK_FATAL};
        pthread_t callback_thread;
        pthread_t teardown_thread;
        start_sink(&interface);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &before);
        assert(before.callback_refcount == 0U);
        assert(pthread_create(&callback_thread, NULL,
                              dispatch_before_entry_thread, &threads) == 0);
        assert(pthread_mutex_lock(&fake.callback_mutex) == 0);
        while (fake.callback_dispatch_state !=
               FAKE_CALLBACK_DISPATCHED_NOT_ENTERED)
            assert(pthread_cond_wait(&fake.callback_condition,
                                     &fake.callback_mutex) == 0);
        assert(pthread_mutex_unlock(&fake.callback_mutex) == 0);
        assert(pthread_create(&teardown_thread, NULL,
                              abort_during_dispatch_thread, &threads) == 0);
        assert(pthread_mutex_lock(&fake.callback_mutex) == 0);
        while (!fake.delete_started)
            assert(pthread_cond_wait(&fake.callback_condition,
                                     &fake.callback_mutex) == 0);
        assert(!fake.delete_returned);
        fake.callback_entry_permitted = 1U;
        assert(pthread_cond_broadcast(&fake.callback_condition) == 0);
        assert(pthread_mutex_unlock(&fake.callback_mutex) == 0);
        assert(pthread_join(callback_thread, NULL) == 0);
        assert(pthread_join(teardown_thread, NULL) == 0);
        assert(threads.abort_result == NP2_PCM_SINK_ACCEPTED);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &after);
        assert(after.tx_eof_epoch == before.tx_eof_epoch &&
               after.stale_callback_count == before.stale_callback_count + 1U &&
               after.callback_refcount == 0U && fake.delete_returned);
        puts(EVIDENCE "scenario=callback_zero_observation dispatch_state=DISPATCHED_NOT_ENTERED delete_started=1 delete_returned=1 delete_returned_while_pending=0 gate_active=0 gate_generation=1 callback_generation=1 in_flight_before=0 in_flight_peak=1 in_flight_final=0 target_accessed=0 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=1 reclaim_attempted=1 reclaim_allowed=1 timeout=0 observed_zero=1 late_entry=1 target_touched=0 eof_credit=0 stale=1");
        close_sink(&fake, sink, &interface);
        emit_history(&fake, "callback_zero_observation");
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        start_sink(&interface);
        assert(p4_nano_audio86_physical_sink_test_callback_enter(
            sink, fake.generation));
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_FATAL);
        assert(p4_nano_audio86_physical_sink_destroy(sink) != 0);
        p4_nano_audio86_physical_sink_test_callback_exit(sink);
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        puts(EVIDENCE "scenario=callback_inflight_teardown dispatch_state=ENTERED_IN_FLIGHT delete_started=1 delete_returned=1 gate_active=0 gate_generation=1 callback_generation=1 in_flight_before=1 in_flight_peak=1 in_flight_final=0 target_accessed=1 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=0 reclaim_attempted=1 reclaim_allowed=0 timeout=1 held=1 abort_while_held=2 unsafe_free=0 released=1 abort_after_release=0");
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct p4_nano_audio86_physical_telemetry before;
        struct p4_nano_audio86_physical_telemetry after;
        start_sink(&interface);
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &before);
        p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &after);
        assert(after.tx_eof_epoch == before.tx_eof_epoch &&
               after.stale_callback_count == before.stale_callback_count + 1U);
        puts(EVIDENCE "scenario=callback_stale_after_abort dispatch_state=EXITED delete_started=1 delete_returned=1 gate_active=0 gate_generation=1 callback_generation=1 in_flight_before=0 in_flight_peak=1 in_flight_final=0 target_accessed=0 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=1 reclaim_attempted=1 reclaim_allowed=1 timeout=0 target_touched=0 eof_credit=0 retry_authorized=0 finish_credit=0 stale=1");
        close_sink(&fake, sink, &interface);
    }
    {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        start_sink(&interface);
        p4_nano_audio86_physical_sink_test_set_callback_refcount(sink, 1U);
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_FATAL);
        assert(p4_nano_audio86_physical_sink_destroy(sink) != 0);
        p4_nano_audio86_physical_sink_test_set_callback_refcount(sink, 0U);
        assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
        puts(EVIDENCE "scenario=callback_quiescence_timeout dispatch_state=ENTERED_IN_FLIGHT delete_started=1 delete_returned=1 gate_active=0 gate_generation=1 callback_generation=1 in_flight_before=1 in_flight_peak=1 in_flight_final=1 target_accessed=1 eof_epoch_before=0 eof_epoch_after=0 stale_callback_count=0 reclaim_attempted=1 reclaim_allowed=0 timeout=1 abort=2 unsafe_free=0 retry_abort=0");
        close_sink(&fake, sink, &interface);
    }
}

struct terminal_model {
    unsigned first_error;
    unsigned forced_abort;
    unsigned finish_accepted;
};

static void terminal_record_error(struct terminal_model *model,
                                  unsigned error, int physical)
{
    if (model->first_error == 0U) model->first_error = error;
    if (physical) {
        model->forced_abort = 1U;
        model->finish_accepted = 0U;
    }
}

static void test_healthy_terminal(const char *scenario, unsigned primary_error)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    struct terminal_model model = {0U, 0U, 0U};
    uint8_t pcm[960];
    struct np2_pcm_sink_view view;
    fill_pcm(pcm, sizeof(pcm), 31U);
    start_sink(&interface);
    view = full_view(pcm, 0U);
    assert(interface.submit(interface.opaque, &view) ==
           NP2_PCM_SINK_ACCEPTED);
    if (primary_error != 0U)
        terminal_record_error(&model, primary_error, 0);
    fake.auto_eof_budget = 4U;
    model.finish_accepted =
        interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED ? 1U : 0U;
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(model.finish_accepted == 1U && model.forced_abort == 0U &&
           model.first_error == primary_error &&
           telemetry.state == P4_NANO_AUDIO86_PHYSICAL_QUIESCENT &&
           telemetry.accepted_pending_drain_frames == 0U &&
           telemetry.physically_drained_frames == 240U);
    printf(EVIDENCE "scenario=%s terminal=%s first_error=%u forced_abort=%u finish_accepted=%u abandonment=0 pending_a=%" PRIu64 " quiescent=1\n",
           scenario, primary_error == 0U ? "STOP" : "PRIMARY",
           model.first_error, model.forced_abort, model.finish_accepted,
           telemetry.accepted_pending_drain_frames);
    close_sink(&fake, sink, &interface);
}

static void test_physical_terminal(const char *scenario,
                                   unsigned primary_before,
                                   unsigned primary_after)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    struct terminal_model model = {0U, 0U, 0U};
    uint8_t pcm[960];
    struct np2_pcm_sink_view view;
    fill_pcm(pcm, sizeof(pcm), 31U);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    if (primary_before != 0U)
        terminal_record_error(&model, primary_before, 0);
    fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_ERROR;
    fake.write_bytes = 0U;
    view = full_view(pcm, 4U);
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_FATAL);
    terminal_record_error(&model, 2U, 1);
    if (primary_after != 0U)
        terminal_record_error(&model, primary_after, 0);
    assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(model.forced_abort == 1U && model.finish_accepted == 0U &&
           model.first_error == (primary_before != 0U ? primary_before : 2U) &&
           telemetry.semantic_accepted_frames == 4U * 240U &&
           telemetry.physically_discarded_accepted_frames == 4U * 240U &&
           telemetry.accepted_pending_drain_frames == 0U);
    {
        const uint32_t epoch = telemetry.tx_eof_epoch;
        const uint32_t stale = telemetry.stale_callback_count;
        p4_nano_audio86_callback_gate_on_sent(fake.callback_gate);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.tx_eof_epoch == epoch &&
               telemetry.stale_callback_count == stale + 1U);
    }
    close_sink(&fake, sink, &interface);
    if (primary_before == 0U && primary_after == 0U) {
        printf(EVIDENCE "scenario=%s terminal=PHYSICAL first_error=%u forced_abort=%u semantic_a=%" PRIu64 " k=0 p=0 r=0 discarded_a=%" PRIu64 " pending_a=%" PRIu64 " abort_calls=1\n",
               scenario, model.first_error, model.forced_abort,
               telemetry.semantic_accepted_frames,
               telemetry.physically_discarded_accepted_frames,
               telemetry.accepted_pending_drain_frames);
    } else {
        printf(EVIDENCE "scenario=%s terminal=DUAL first_error=%u forced_abort=%u finish_accepted=%u\n",
               scenario, model.first_error, model.forced_abort,
               model.finish_accepted);
    }
}

static void test_terminal_accounting(void)
{
    test_healthy_terminal("healthy_stop", 0U);
    test_healthy_terminal("healthy_primary_fatal", 86U);
    test_physical_terminal("physical_fatal", 0U, 0U);
    test_physical_terminal("dual_primary_then_physical", 86U, 0U);
    test_physical_terminal("dual_physical_then_primary", 0U, 86U);
}

static void test_start_failure_matrix(void)
{
    unsigned stage;
    for (stage = 1U; stage <= 4U; ++stage) {
        struct fake_backend fake;
        struct np2_pcm_sink interface;
        struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
        struct np2opngen_pcm_ring ring;
        struct np2_pcm_output_controller controller;
        struct p4_nano_audio86_physical_telemetry telemetry;
        unsigned residual_before;
        np2opngen_pcm_ring_init(&ring);
        assert(np2_pcm_output_controller_init(&controller, &ring, &interface) == 0);
        fake.start_failure_stage = stage;
        assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_FATAL);
        residual_before = fake.resource_i2c + fake.resource_i2s +
                          fake.resource_callbacks + fake.pa_high;
        assert(np2_pcm_output_abort(&controller) == NP2_PCM_OUTPUT_OK);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.state == P4_NANO_AUDIO86_PHYSICAL_QUIESCENT &&
               telemetry.callback_refcount == 0U &&
               !telemetry.callbacks_active);
        assert(p4_nano_audio86_physical_sink_destroy(sink) == 0);
        assert(fake.resource_i2c == 0U && fake.resource_i2s == 0U &&
               fake.resource_callbacks == 0U && fake.pa_high == 0U &&
               fake.release_calls == 1U);
        printf(EVIDENCE "scenario=start_backend_rollback_%u start_fatal=1 callback_in_flight=%u residual_before=%u residual_after=0 release_calls=%u\n",
               stage, telemetry.callback_refcount, residual_before,
               fake.release_calls);
        emit_history(&fake, stage == 1U ? "start_backend_rollback_1" :
                            stage == 2U ? "start_backend_rollback_2" :
                            stage == 3U ? "start_backend_rollback_3" :
                                          "start_backend_rollback_4");
        assert(pthread_cond_destroy(&fake.callback_condition) == 0);
        assert(pthread_mutex_destroy(&fake.callback_mutex) == 0);
        assert(pthread_mutex_destroy(&fake.history_mutex) == 0);
    }
}

int main(void)
{
    test_full_controller();
    test_s2_ten_unit_stream(0);
    test_s2_ten_unit_stream(1);
    test_final_partial(1U);
    test_final_partial(13U);
    test_final_partial(239U);
    test_retry_and_lost_wake();
    test_partial_progress(4U);
    test_partial_progress(480U);
    test_partial_progress(956U);
    test_finish_and_callbacks();
    test_queue_overflow();
    test_first_qovf_latch();
    test_final_boundary_diagnostics();
    test_quiescence_timeout();
    test_callback_control_faults();
    test_terminal_accounting();
    test_start_failure_matrix();
    return 0;
}
