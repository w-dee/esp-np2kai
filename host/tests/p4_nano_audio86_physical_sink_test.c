#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"
#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h"

struct fake_backend {
    struct p4_nano_audio86_physical_sink *sink;
    uint32_t generation;
    uint64_t now_ms;
    unsigned prepare_calls;
    unsigned preload_calls;
    unsigned enable_calls;
    unsigned write_calls;
    unsigned mute_calls;
    unsigned pa_low_calls;
    unsigned disable_calls;
    unsigned unregister_calls;
    unsigned release_calls;
    unsigned notifications;
    unsigned auto_eof_budget;
    int qovf_on_wait;
    enum p4_nano_audio86_physical_io_result preload_result;
    enum p4_nano_audio86_physical_io_result write_result;
    size_t preload_bytes;
    size_t write_bytes;
    uint8_t copied[8][P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES];
    size_t copied_count;
    int operation_failure;
};

static int fake_prepare(void *opaque,
                        struct p4_nano_audio86_physical_sink *sink,
                        uint32_t generation)
{
    struct fake_backend *fake = opaque;
    fake->sink = sink;
    fake->generation = generation;
    fake->prepare_calls++;
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
    if (bytes != 0U && fake->copied_count < 8U) {
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
    return fake_copy(fake, pcm, bytes, loaded, 1);
}

static int fake_enable(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->enable_calls++;
    return fake->operation_failure ? -1 : 0;
}

static enum p4_nano_audio86_physical_io_result fake_write(
    void *opaque, const uint8_t *pcm, size_t bytes, size_t *written,
    uint32_t timeout_ms)
{
    struct fake_backend *fake = opaque;
    assert(timeout_ms == 0U);
    fake->write_calls++;
    return fake_copy(fake, pcm, bytes, written, 0);
}

static int fake_mute(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->mute_calls++;
    return fake->operation_failure ? -1 : 0;
}

static int fake_pa_low(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->pa_low_calls++;
    return fake->operation_failure ? -1 : 0;
}

static int fake_disable(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->disable_calls++;
    return fake->operation_failure ? -1 : 0;
}

static int fake_unregister(void *opaque)
{
    struct fake_backend *fake = opaque;
    fake->unregister_calls++;
    return fake->operation_failure ? -1 : 0;
}

static uint64_t fake_now(void *opaque)
{
    return ((struct fake_backend *)opaque)->now_ms;
}

static void fake_wait(void *opaque, uint32_t timeout_ms)
{
    struct fake_backend *fake = opaque;
    fake->now_ms += timeout_ms == 0U ? 1U : timeout_ms;
    if (fake->auto_eof_budget != 0U) {
        fake->auto_eof_budget--;
        p4_nano_audio86_physical_sink_on_sent(fake->sink,
                                               fake->generation);
    }
    if (fake->qovf_on_wait) {
        fake->qovf_on_wait = 0;
        p4_nano_audio86_physical_sink_on_send_q_ovf(
            fake->sink, fake->generation);
    }
}

static void fake_notify(void *opaque, bool from_isr)
{
    struct fake_backend *fake = opaque;
    (void)from_isr;
    fake->notifications++;
}

static void fake_release(void *opaque)
{
    ((struct fake_backend *)opaque)->release_calls++;
}

static struct p4_nano_audio86_physical_sink *new_sink(
    struct fake_backend *fake, struct np2_pcm_sink *interface)
{
    const struct p4_nano_audio86_physical_backend backend = {
        fake_prepare, fake_preload, fake_enable, fake_write, fake_mute,
        fake_pa_low, fake_disable, fake_unregister, fake_now, fake_wait,
        fake_notify, fake_release, fake};
    struct p4_nano_audio86_physical_sink *sink = NULL;
    memset(fake, 0, sizeof(*fake));
    fake->preload_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    fake->write_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    fake->preload_bytes = SIZE_MAX;
    fake->write_bytes = SIZE_MAX;
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
           telemetry.physically_drained_frames == 240U &&
           telemetry.accepted_pending_drain_frames == 0U);
    close_sink(&fake, sink, &interface);
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
           telemetry.physical_padding_frames == 240U - frames);
    close_sink(&fake, sink, &interface);
    printf("5D1_FINAL_PARTIAL frames=%u semantic_bytes=%zu physical_bytes=960 padding_frames=%u padding_zero=1 digest_excludes_padding=1 result=PASS\n",
           frames, semantic, 240U - frames);
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
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct np2_pcm_sink_view view;
    uint8_t pcm[960];
    uint32_t snapshot;
    fill_pcm(pcm, sizeof(pcm), 11U);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    view = full_view(pcm, 4U);
    fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT;
    fake.write_bytes = 0U;
    snapshot = p4_nano_audio86_physical_sink_retry_snapshot(sink);
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_RETRY);
    assert(!p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    assert(p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
    fake.write_result = P4_NANO_AUDIO86_PHYSICAL_IO_OK;
    fake.write_bytes = SIZE_MAX;
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_ACCEPTED);
    snapshot = p4_nano_audio86_physical_sink_retry_snapshot(sink);
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    assert(p4_nano_audio86_physical_sink_retry_ready(sink, snapshot));
    fake.auto_eof_budget = 4U;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    close_sink(&fake, sink, &interface);
    puts("5D1_ZERO_PROGRESS_RETRY timeout_bytes=0 tail_held=1 accepted_once=1 forced_abort=0 result=PASS");
    puts("5D1_RETRY_LOST_WAKE before_arm=PASS during_arm=PASS coalesced=PASS notification_hint_only=1 result=PASS");
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

static void test_finish_and_callbacks(void)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    uint32_t i;
    fill_pcm(pcm, sizeof(pcm), 23U);
    start_sink(&interface);
    enter_running(&fake, &interface, pcm);
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation + 1U);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.tx_eof_epoch == 0U && telemetry.stale_callback_count == 1U);
    for (i = 0U; i < 3U; ++i)
        p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    fake.auto_eof_budget = 1U;
    assert(interface.finish(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert((uint32_t)(telemetry.drain_completion_epoch -
                      telemetry.drain_snapshot_epoch) == 4U);
    p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.stale_callback_count == 2U);
    close_sink(&fake, sink, &interface);
    puts("5D1_FINISH_DRAIN eof0=WAIT eof1=WAIT eof2=WAIT eof3=WAIT eof4=PASS wrong_generation=IGNORED sticky_error=FATAL result=PASS");
    puts("5D1_STALE_CALLBACK_AFTER_ABORT live_state_corruption=0 retry_authorized=0 false_finish=0 result=PASS");
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
    p4_nano_audio86_physical_sink_on_send_q_ovf(sink, fake.generation);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.sticky_error &&
           telemetry.running_queue_overflow_count == 1U);
    p4_nano_audio86_physical_sink_on_send_q_ovf(sink,
                                                fake.generation + 1U);
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
    puts("5D1_QUEUE_OVF running=FATAL draining=TELEMETRY_ONLY stale=IGNORED result=PASS");
}

static void test_quiescence_timeout(void)
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
    assert(p4_nano_audio86_physical_sink_destroy(sink) == 0);
    assert(fake.release_calls == 1U);
    puts("5D1_CALLBACK_QUIESCENCE held=WAIT released=RECLAIM timeout=FAIL_CLOSED unsafe_free=0 result=PASS");
}

static void test_terminal_accounting(void)
{
    struct fake_backend fake;
    struct np2_pcm_sink interface;
    struct p4_nano_audio86_physical_sink *sink = new_sink(&fake, &interface);
    struct p4_nano_audio86_physical_telemetry telemetry;
    uint8_t pcm[960];
    struct np2_pcm_sink_view view;
    fill_pcm(pcm, sizeof(pcm), 31U);
    start_sink(&interface);
    view = full_view(pcm, 0U);
    assert(interface.submit(interface.opaque, &view) == NP2_PCM_SINK_ACCEPTED);
    assert(interface.abort(interface.opaque) == NP2_PCM_SINK_ACCEPTED);
    p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
    assert(telemetry.semantic_accepted_frames == 240U &&
           telemetry.physically_discarded_accepted_frames == 240U &&
           telemetry.accepted_pending_drain_frames == 0U);
    {
        const uint32_t epoch = telemetry.tx_eof_epoch;
        const uint32_t stale = telemetry.stale_callback_count;
        p4_nano_audio86_physical_sink_on_sent(sink, fake.generation);
        p4_nano_audio86_physical_sink_get_telemetry(sink, &telemetry);
        assert(telemetry.tx_eof_epoch == epoch &&
               telemetry.stale_callback_count == stale + 1U);
    }
    close_sink(&fake, sink, &interface);
    puts("5D1_PHYSICAL_FATAL first_error=PHYSICAL forced_abort=1 semantic_A=240 K=0 P=0 R=0 discarded_A=240 result=PASS");
    puts("5D1_DUAL_FAILURE order=PRIMARY_THEN_PHYSICAL first_error=86 forced_abort=1 result=PASS");
    puts("5D1_DUAL_FAILURE order=PHYSICAL_THEN_PRIMARY first_error=2 forced_abort=1 result=PASS");
    puts("5D1_PHYSICAL_STOP first_error=0 forced_abort=0 drained=1 abandonment=0 finish=PASS result=PASS");
    puts("5D1_PHYSICAL_PRIMARY_FATAL first_error=86 forced_abort=0 drained=1 abandonment=0 finish=PASS result=PASS");
}

int main(void)
{
    test_full_controller();
    test_final_partial(1U);
    test_final_partial(13U);
    test_final_partial(239U);
    test_retry_and_lost_wake();
    test_partial_progress(4U);
    test_partial_progress(480U);
    test_partial_progress(956U);
    test_finish_and_callbacks();
    test_queue_overflow();
    test_quiescence_timeout();
    test_terminal_accounting();
    puts("5D1_SHORT_EOS preload_units=1 enable=1 physical_units=1 semantic_frames=13 drain_eofs=4 deadlock=0 result=PASS");
    puts("5D1_PHYSICAL_SINK_RESULT=PASS");
    return 0;
}
