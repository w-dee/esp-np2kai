#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "p4_nano_audio86_live_service.h"

enum fake_finish_mode {
    FAKE_FINISH_ACCEPT = 0,
    FAKE_FINISH_WAIT_CALLBACK,
    FAKE_FINISH_FATAL,
};

struct fake_sink {
    _Atomic uint32_t started;
    _Atomic uint32_t submissions;
    _Atomic uint64_t accepted_frames;
    _Atomic uint32_t finish_calls;
    _Atomic uint32_t abort_calls;
    _Atomic uint32_t callback_inflight;
    _Atomic uint32_t no_new_callbacks;
    _Atomic uint32_t start_retry;
    _Atomic uint32_t start_fatal;
    _Atomic uint32_t submit_retry;
    _Atomic uint32_t submit_fatal;
    _Atomic uint32_t finish_mode;
    _Atomic uint32_t abort_fatal;
    uint32_t next_sequence;
    uint64_t next_offset;
};

static void sleep_ms(uint32_t ms)
{
    struct timespec delay = {(time_t)(ms / 1000U),
                             (long)(ms % 1000U) * 1000000L};
    (void)nanosleep(&delay, NULL);
}

static enum np2_pcm_sink_result fake_start(void *opaque)
{
    struct fake_sink *sink = opaque;
    if (atomic_load_explicit(&sink->start_retry, memory_order_acquire) != 0U)
        return NP2_PCM_SINK_RETRY;
    if (atomic_load_explicit(&sink->start_fatal, memory_order_acquire) != 0U)
        return NP2_PCM_SINK_FATAL;
    atomic_store_explicit(&sink->started, 1U, memory_order_release);
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result fake_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    struct fake_sink *sink = opaque;
    if (atomic_load_explicit(&sink->submit_retry, memory_order_acquire) != 0U)
        return NP2_PCM_SINK_RETRY;
    if (atomic_load_explicit(&sink->submit_fatal, memory_order_acquire) != 0U)
        return NP2_PCM_SINK_FATAL;
    assert(view != NULL);
    assert(view->sequence == sink->next_sequence);
    assert(view->frame_offset == sink->next_offset);
    assert(view->valid_frames > 0U &&
           view->valid_frames <= NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
    sink->next_sequence++;
    sink->next_offset += view->valid_frames;
    atomic_fetch_add_explicit(&sink->submissions, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&sink->accepted_frames, view->valid_frames,
                              memory_order_relaxed);
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result fake_finish(void *opaque)
{
    struct fake_sink *sink = opaque;
    uint32_t mode;
    atomic_fetch_add_explicit(&sink->finish_calls, 1U, memory_order_relaxed);
    mode = atomic_load_explicit(&sink->finish_mode, memory_order_acquire);
    if (mode == FAKE_FINISH_FATAL)
        return NP2_PCM_SINK_FATAL;
    if (mode == FAKE_FINISH_WAIT_CALLBACK &&
        atomic_load_explicit(&sink->callback_inflight,
                             memory_order_acquire) != 0U)
        return NP2_PCM_SINK_RETRY;
    atomic_store_explicit(&sink->no_new_callbacks, 1U,
                          memory_order_release);
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result fake_abort(void *opaque)
{
    struct fake_sink *sink = opaque;
    atomic_fetch_add_explicit(&sink->abort_calls, 1U, memory_order_relaxed);
    if (atomic_load_explicit(&sink->abort_fatal, memory_order_acquire) != 0U)
        return NP2_PCM_SINK_FATAL;
    if (atomic_load_explicit(&sink->finish_mode, memory_order_acquire) ==
            FAKE_FINISH_WAIT_CALLBACK &&
        atomic_load_explicit(&sink->callback_inflight,
                             memory_order_acquire) != 0U)
        return NP2_PCM_SINK_RETRY;
    atomic_store_explicit(&sink->no_new_callbacks, 1U,
                          memory_order_release);
    return NP2_PCM_SINK_ACCEPTED;
}

static struct np2_pcm_sink make_pcm_sink(struct fake_sink *sink)
{
    const struct np2_pcm_sink result = {
        sink, fake_start, fake_submit, fake_finish, fake_abort};
    return result;
}

static void reset_guest(void)
{
    np2audio86_guest_sink_unbind();
    np2audio86_guest_opna_unbind();
    np2audio86_guest_host_test_seed(0U, 0U);
    np2audio86_guest_host_set_clock(49152000U, 1U);
    np2audio86_guest_host_set_cpumode(0U);
    np2audio86_guest_host_set_cpu_position(0U);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608, 0U, 1U, 2U);
    np2audio86_guest_opna_set_config(3U, 0U);
    np2audio86_guest_opna_bind();
    np2audio86_guest_pcm86_stream_bind();
}

static void service_init(struct p4_nano_audio86_live_service *service,
                         struct fake_sink *fake,
                         struct np2_pcm_sink *pcm_sink)
{
    struct p4_nano_audio86_live_config config;
    memset(service, 0, sizeof(*service));
    memset(fake, 0, sizeof(*fake));
    *pcm_sink = make_pcm_sink(fake);
    config.borrowed_sink = pcm_sink;
    assert(p4_nano_audio86_live_service_init(service, &config) ==
           P4_NANO_AUDIO86_LIVE_OK);
    {
        struct p4_nano_audio86_live_status status;
        p4_nano_audio86_live_service_status(service, &status);
        assert(status.state == P4_NANO_AUDIO86_LIVE_READY);
        assert(status.snapshot_coherent == 1U);
        assert(status.output_state == NP2_PCM_OUTPUT_INITIAL);
        assert(status.event_ring_occupancy == 0U);
        assert(status.byte_ring_occupancy == 0U);
        assert(status.q240_occupancy == 0U);
    }
}

static void service_start_attach(
    struct p4_nano_audio86_live_service *service, struct fake_sink *fake,
    struct np2_pcm_sink *pcm_sink)
{
    reset_guest();
    service_init(service, fake, pcm_sink);
    assert(p4_nano_audio86_live_service_owner_checkpoint(service) ==
           P4_NANO_AUDIO86_LIVE_OWNER_ERROR);
    assert(p4_nano_audio86_live_service_attach_guest(service) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    assert(p4_nano_audio86_live_service_start(service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    {
        struct p4_nano_audio86_live_status status;
        p4_nano_audio86_live_service_status(service, &status);
        assert(status.state == P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED);
    }
    assert(p4_nano_audio86_live_service_attach_guest(service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    {
        struct p4_nano_audio86_live_status status;
        p4_nano_audio86_live_service_status(service, &status);
        assert(status.state == P4_NANO_AUDIO86_LIVE_RUNNING);
        assert(status.snapshot_coherent == 1U);
        assert(status.output_state == NP2_PCM_OUTPUT_STARTED);
    }
    assert(p4_nano_audio86_live_service_attach_guest(service) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    assert(p4_nano_audio86_live_service_owner_checkpoint(service) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void assert_clean_terminal(
    struct p4_nano_audio86_live_service *service, struct fake_sink *sink,
    uint64_t frames, uint32_t expected_slots)
{
    struct p4_nano_audio86_live_status status;
    assert(p4_nano_audio86_live_service_join(service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(status.state == P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT);
    assert(status.cleanup == P4_NANO_AUDIO86_LIVE_CLEANUP_QUIESCENT);
    if (status.rendered_frames != frames || status.final_horizon != frames ||
        status.accepted_frames != frames)
        fprintf(stderr,
                "terminal mismatch expected=%llu rendered=%llu final=%llu accepted=%llu\n",
                (unsigned long long)frames,
                (unsigned long long)status.rendered_frames,
                (unsigned long long)status.final_horizon,
                (unsigned long long)status.accepted_frames);
    assert(status.rendered_frames == frames);
    assert(status.final_horizon == frames);
    assert(status.accepted_frames == frames);
    assert(status.producer_done == 1U);
    assert(status.guest_attached == 0U);
    assert(status.sink_reachable == 0U);
    assert(atomic_load_explicit(&sink->accepted_frames,
                                memory_order_acquire) == frames);
    assert(atomic_load_explicit(&sink->submissions,
                                memory_order_acquire) == expected_slots);
    assert(atomic_load_explicit(&sink->no_new_callbacks,
                                memory_order_acquire) == 1U);
    assert(p4_nano_audio86_live_service_join(service, 0U, &status) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_start(service) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    assert(p4_nano_audio86_live_service_report_producer_failure(service, 99U) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    assert(p4_nano_audio86_live_service_destroy(service) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void test_ready_destroy_reinit(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink = make_pcm_sink(&fake);
    struct p4_nano_audio86_live_config config = {&sink};
    struct p4_nano_audio86_live_status status;
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_UNINITIALIZED);
    assert(p4_nano_audio86_live_service_init(&service, &config) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_init(&service, &config) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_DESTROYED);
}

struct start_thread_args {
    struct p4_nano_audio86_live_service *service;
    enum p4_nano_audio86_live_result result;
};

static void *start_thread(void *opaque)
{
    struct start_thread_args *args = opaque;
    args->result = p4_nano_audio86_live_service_start(args->service);
    return NULL;
}

static void test_starting_and_attach_failure(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;
    struct start_thread_args args = {&service,
                                     P4_NANO_AUDIO86_LIVE_STATE_ERROR};
    pthread_t thread;

    service_init(&service, &fake, &sink);
    atomic_store_explicit(&fake.start_retry, 1U, memory_order_release);
    assert(pthread_create(&thread, NULL, start_thread, &args) == 0);
    do {
        p4_nano_audio86_live_service_status(&service, &status);
    } while (status.state == P4_NANO_AUDIO86_LIVE_READY);
    assert(status.state == P4_NANO_AUDIO86_LIVE_STARTING);
    {
        uint32_t attempts = 100U;
        do {
            p4_nano_audio86_live_service_status(&service, &status);
            if (status.worker_wait_reason ==
                P4_NANO_AUDIO86_LIVE_WAIT_SINK_START)
                break;
            sleep_ms(1U);
        } while (--attempts != 0U);
        assert(status.worker_wait_reason ==
               P4_NANO_AUDIO86_LIVE_WAIT_SINK_START);
    }
    atomic_store_explicit(&fake.start_retry, 0U, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    assert(args.result == P4_NANO_AUDIO86_LIVE_OK);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    reset_guest();
    service_init(&service, &fake, &sink);
    assert(p4_nano_audio86_live_service_start(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    np2audio86_guest_host_set_clock(1U, 1U);
    assert(np2audio86_guest_host_failed());
    assert(p4_nano_audio86_live_service_attach_guest(&service) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT);
    assert(status.origin == P4_NANO_AUDIO86_LIVE_ORIGIN_ATTACH);
    assert(status.guest_attached == 0U && status.sink_reachable == 0U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void test_stop_before_attach(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;
    service_init(&service, &fake, &sink);
    assert(p4_nano_audio86_live_service_start(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_DRAINING ||
           status.state == P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_ALREADY);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(status.final_horizon == 0U && status.accepted_frames == 0U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void test_clean_boundaries(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;

    service_start_attach(&service, &fake, &sink);
    np2audio86_guest_host_set_cpu_position(240U * 1024U);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.snapshot_coherent == 1U);
    assert(status.guest_authoritative_frame == 240U);
    assert(status.latest_published_horizon == 240U);
    assert(p4_nano_audio86_live_service_join(&service, 1U, &status) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_join(&service, 1U, &status) ==
           P4_NANO_AUDIO86_LIVE_TIMEOUT);
    assert(status.state == P4_NANO_AUDIO86_LIVE_STOP_REQUESTED);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert_clean_terminal(&service, &fake, 240U, 1U);

    service_start_attach(&service, &fake, &sink);
    np2audio86_guest_host_set_cpu_position(4U * 1024U);
    np2audio86_guest_opna_write_address_low(0x28U);
    np2audio86_guest_opna_write_data_low(0xf0U);
    np2audio86_guest_host_set_cpu_position(13U * 1024U);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert_clean_terminal(&service, &fake, 13U, 1U);
}

static void stop_after_authorization(
    struct p4_nano_audio86_live_service *service, void *opaque)
{
    uint32_t *calls = opaque;
    ++*calls;
    assert(p4_nano_audio86_live_service_request_stop(service) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void test_transaction_stop_boundary(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    np2audio86_guest_state_snapshot_t before;
    np2audio86_guest_state_snapshot_t after;
    uint32_t hook_calls = 0U;

    service_start_attach(&service, &fake, &sink);
    np2audio86_guest_host_snapshot(&before);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    np2audio86_guest_opna_write_address_low(0x28U);
    np2audio86_guest_opna_write_data_low(0xf0U);
    np2audio86_guest_host_snapshot(&after);
    assert(after.sequence == before.sequence);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert_clean_terminal(&service, &fake, 0U, 0U);

    service_start_attach(&service, &fake, &sink);
    np2audio86_guest_host_set_cpu_position(7U * 1024U);
    p4_nano_audio86_live_service_test_set_authorization_hook(
        &service, stop_after_authorization, &hook_calls);
    np2audio86_guest_opna_write_address_low(0x28U);
    np2audio86_guest_opna_write_data_low(0xf0U);
    p4_nano_audio86_live_service_test_set_authorization_hook(
        &service, NULL, NULL);
    np2audio86_guest_host_snapshot(&after);
    assert(hook_calls == 1U);
    assert(after.sequence == 1U);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert_clean_terminal(&service, &fake, 7U, 1U);
}

static void test_delayed_callback_barrier(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;
    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.finish_mode, FAKE_FINISH_WAIT_CALLBACK,
                          memory_order_release);
    atomic_store_explicit(&fake.callback_inflight, 1U, memory_order_release);
    np2audio86_guest_host_set_cpu_position(13U * 1024U);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    sleep_ms(5U);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_DRAINING);
    assert(status.sink_reachable == 1U);
    assert(status.snapshot_coherent == 1U);
    assert(status.worker_wait_reason == P4_NANO_AUDIO86_LIVE_WAIT_FINISH ||
           status.worker_wait_reason ==
               P4_NANO_AUDIO86_LIVE_WAIT_SINK_RETRY);
    assert(atomic_load_explicit(&fake.finish_calls, memory_order_acquire) > 0U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_STATE_ERROR);
    atomic_store_explicit(&fake.callback_inflight, 0U, memory_order_release);
    assert_clean_terminal(&service, &fake, 13U, 1U);
}

static void test_bounded_running_pressure_snapshot(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;
    uint32_t attempts = 2000U;

    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.submit_retry, 1U, memory_order_release);
    np2audio86_guest_host_set_cpu_position(2400U * 1024U);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    do {
        p4_nano_audio86_live_service_status(&service, &status);
        if (status.q240_occupancy == NP2_OPNGEN_PCM_RING_CAPACITY &&
            status.worker_wait_reason == P4_NANO_AUDIO86_LIVE_WAIT_Q240_SPACE)
            break;
        sleep_ms(1U);
    } while (--attempts != 0U);
    assert(status.state == P4_NANO_AUDIO86_LIVE_RUNNING);
    assert(status.snapshot_coherent == 1U);
    assert(status.guest_authoritative_frame == 2400U);
    assert(status.latest_published_horizon == 2400U);
    assert(status.q240_occupancy == NP2_OPNGEN_PCM_RING_CAPACITY);
    assert(status.q240_produced - status.q240_submitted ==
           status.q240_occupancy);
    assert(status.worker_wait_reason == P4_NANO_AUDIO86_LIVE_WAIT_Q240_SPACE);

    atomic_store_explicit(&fake.submit_retry, 0U, memory_order_release);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert_clean_terminal(&service, &fake, 2400U, 10U);
}

static void fatal_at_clean_terminal(
    struct p4_nano_audio86_live_service *service, void *opaque)
{
    uint32_t *calls = opaque;
    ++*calls;
    assert(p4_nano_audio86_live_service_test_fail(
               service, P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
               P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH, 0x71U) ==
           P4_NANO_AUDIO86_LIVE_OK);
}

static void test_fatal_paths(void)
{
    struct p4_nano_audio86_live_service service = {0};
    struct fake_sink fake = {0};
    struct np2_pcm_sink sink;
    struct p4_nano_audio86_live_status status;

    /* Sink start failure still runs abort and proves quiescence. */
    service_init(&service, &fake, &sink);
    atomic_store_explicit(&fake.start_fatal, 1U, memory_order_release);
    assert(p4_nano_audio86_live_service_start(&service) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT);
    assert(atomic_load_explicit(&fake.abort_calls, memory_order_acquire) == 1U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* Fatal wins the state CAS even after finish establishes its callback
     * barrier but before clean terminal publication. */
    {
        uint32_t hook_calls = 0U;
        service_start_attach(&service, &fake, &sink);
        p4_nano_audio86_live_service_test_set_before_clean_terminal_hook(
            &service, fatal_at_clean_terminal, &hook_calls);
        assert(p4_nano_audio86_live_service_request_stop(&service) ==
               P4_NANO_AUDIO86_LIVE_OK);
        assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
               P4_NANO_AUDIO86_LIVE_OK);
        assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
               P4_NANO_AUDIO86_LIVE_FAILED);
        assert(hook_calls == 1U);
        assert(status.state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT);
        assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_WORKER);
        assert(status.subcode == 0x71U);
        assert(status.sink_reachable == 0U);
        assert(atomic_load_explicit(&fake.finish_calls,
                                    memory_order_acquire) == 1U);
        assert(atomic_load_explicit(&fake.abort_calls,
                                    memory_order_acquire) == 0U);
        assert(p4_nano_audio86_live_service_destroy(&service) ==
               P4_NANO_AUDIO86_LIVE_OK);
    }

    /* A producer fatal wins after STOP_REQUESTED. */
    service_start_attach(&service, &fake, &sink);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_report_producer_failure(&service,
                                                                0x44U) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER);
    assert(status.subcode == 0x44U && status.first_error_sequence == 1U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* finish FATAL becomes output fatal, then successful abort barrier. */
    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.finish_mode, FAKE_FINISH_FATAL,
                          memory_order_release);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT);
    assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT);
    assert(atomic_load_explicit(&fake.abort_calls, memory_order_acquire) == 1U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* A worker fatal races with RUNNING, detaches on the owner checkpoint,
     * and preserves its identity through successful abort cleanup. */
    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.finish_mode, FAKE_FINISH_WAIT_CALLBACK,
                          memory_order_release);
    atomic_store_explicit(&fake.callback_inflight, 1U, memory_order_release);
    assert(p4_nano_audio86_live_service_test_fail(
               &service, P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
               P4_NANO_AUDIO86_LIVE_ORIGIN_RENDER, 0x51U) ==
           P4_NANO_AUDIO86_LIVE_OK);
    p4_nano_audio86_live_service_status(&service, &status);
    assert(status.state == P4_NANO_AUDIO86_LIVE_FAILING);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    atomic_store_explicit(&fake.callback_inflight, 0U, memory_order_release);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_WORKER);
    assert(status.subcode == 0x51U && status.first_error_sequence == 1U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* Sink submit FATAL while draining PCM cannot become a clean finish. */
    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.submit_fatal, 1U, memory_order_release);
    np2audio86_guest_host_set_cpu_position(240U * 1024U);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT);
    assert(status.origin == P4_NANO_AUDIO86_LIVE_ORIGIN_PCM_OUTPUT);
    assert(atomic_load_explicit(&fake.finish_calls, memory_order_acquire) ==
           0U);
    assert(atomic_load_explicit(&fake.abort_calls, memory_order_acquire) ==
           1U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* A fatal injected after DRAINING begins wins while finish waits for an
     * in-flight callback; abort then waits for that same callback barrier. */
    service_start_attach(&service, &fake, &sink);
    atomic_store_explicit(&fake.finish_mode, FAKE_FINISH_WAIT_CALLBACK,
                          memory_order_release);
    atomic_store_explicit(&fake.callback_inflight, 1U, memory_order_release);
    assert(p4_nano_audio86_live_service_request_stop(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    assert(p4_nano_audio86_live_service_owner_checkpoint(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);
    sleep_ms(5U);
    assert(p4_nano_audio86_live_service_report_producer_failure(
               &service, 0x61U) == P4_NANO_AUDIO86_LIVE_FAILED);
    atomic_store_explicit(&fake.callback_inflight, 0U, memory_order_release);
    assert(p4_nano_audio86_live_service_join(&service, 2000U, &status) ==
           P4_NANO_AUDIO86_LIVE_FAILED);
    assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER);
    assert(status.subcode == 0x61U);
    assert(atomic_load_explicit(&fake.no_new_callbacks,
                                memory_order_acquire) == 1U);
    assert(atomic_load_explicit(&fake.finish_calls, memory_order_acquire) >
               0U ||
           atomic_load_explicit(&fake.abort_calls, memory_order_acquire) >
               0U);
    assert(p4_nano_audio86_live_service_destroy(&service) ==
           P4_NANO_AUDIO86_LIVE_OK);

    /* Failed abort barrier is explicitly not quiescent and not destroyable. */
    {
        static struct p4_nano_audio86_live_service retained_service;
        static struct fake_sink retained_fake;
        static struct np2_pcm_sink retained_sink;
        service_start_attach(&retained_service, &retained_fake,
                             &retained_sink);
        atomic_store_explicit(&retained_fake.finish_mode,
                              FAKE_FINISH_FATAL, memory_order_release);
        atomic_store_explicit(&retained_fake.abort_fatal, 1U,
                              memory_order_release);
        assert(p4_nano_audio86_live_service_request_stop(
                   &retained_service) == P4_NANO_AUDIO86_LIVE_OK);
        assert(p4_nano_audio86_live_service_owner_checkpoint(
                   &retained_service) == P4_NANO_AUDIO86_LIVE_OK);
        assert(p4_nano_audio86_live_service_join(
                   &retained_service, 2000U, &status) ==
               P4_NANO_AUDIO86_LIVE_NOT_QUIESCENT);
        assert(status.state ==
               P4_NANO_AUDIO86_LIVE_FAILED_NOT_QUIESCENT);
        assert(status.cleanup ==
               P4_NANO_AUDIO86_LIVE_CLEANUP_NOT_QUIESCENT);
        assert(status.category == P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT);
        assert(status.origin == P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH);
        assert(status.sink_reachable == 1U);
        assert(p4_nano_audio86_live_service_destroy(&retained_service) ==
               P4_NANO_AUDIO86_LIVE_STATE_ERROR);
        /* Deliberately retain static service and sink storage until process
         * exit: fail-closed means reachable storage is not reclaimed. */
    }
}

int main(void)
{
    np2audio86_test_opngen_initialize_reset();
    test_ready_destroy_reinit();
    test_starting_and_attach_failure();
    test_stop_before_attach();
    test_clean_boundaries();
    test_transaction_stop_boundary();
    test_delayed_callback_barrier();
    test_bounded_running_pressure_snapshot();
    test_fatal_paths();
    assert(np2audio86_test_opngen_initialize_call_count() == 1U);
    printf("AUDIO86_LIVE_SERVICE_STATE_MACHINE=PASS\n");
    printf("AUDIO86_LIVE_SERVICE_STOP_BOUNDARY=PASS\n");
    printf("AUDIO86_LIVE_SERVICE_CALLBACK_QUIESCENCE=PASS\n");
    printf("AUDIO86_LIVE_SERVICE_BOUNDED_SNAPSHOT=PASS\n");
    printf("AUDIO86_LIVE_SERVICE_Q240_PRESSURE_SNAPSHOT=PASS\n");
    printf("OPN_GLOBAL_INIT_PROCESS_LIFETIME_CALL_COUNT=1\n");
    printf("AUDIO86_LIVE_SERVICE_HOST_TEST=PASS\n");
    return 0;
}
