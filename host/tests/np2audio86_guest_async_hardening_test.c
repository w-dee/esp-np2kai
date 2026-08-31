/* Host-only 86R.4B transport hardening over the real 86R.4A execution. */
#define main np2audio86_guest_async_4a_main
#include "np2audio86_guest_async_test.c"
#undef main

struct live_driver { int result; };

struct supplemental_context {
    struct np2audio86_event_ring events;
    struct np2audio86_byte_ring bytes;
    _Atomic int first_error;
    _Atomic bool byte_full;
    _Atomic bool event_full;
    _Atomic bool consumer_waiting;
    _Atomic bool producer_done;
    uint8_t output[64];
    size_t output_count;
};

struct run_request {
    struct supplemental_context *context;
    uint64_t sequence;
    const uint8_t *bytes;
    size_t count;
    int result;
};

struct old_publication_probe {
    _Atomic bool claimed;
    _Atomic bool reached;
    _Atomic bool paused;
    _Atomic bool release;
};

static int wait_bool(const _Atomic bool *value)
{
    for (;;) {
        if (atomic_load_explicit(value, memory_order_acquire)) return 0;
        sched_yield();
    }
}

static void *old_publication_probe_thread(void *opaque)
{
    struct old_publication_probe *probe = opaque;
    bool expected = false;
    (void)atomic_compare_exchange_strong_explicit(&probe->reached, &expected, true,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
    atomic_store_explicit(&probe->claimed, true, memory_order_release);
    atomic_store_explicit(&probe->paused, true, memory_order_release);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) sched_yield();
    return NULL;
}

/* This is the pre-fix protocol in executable form: reached was the claim, so
 * a coordinator can observe it while publication remains paused. */
static int run_old_publication_reproduction(void)
{
    struct old_publication_probe probe;
    pthread_t thread;
    atomic_init(&probe.claimed, false);
    atomic_init(&probe.reached, false);
    atomic_init(&probe.paused, false);
    atomic_init(&probe.release, false);
    if (pthread_create(&thread, NULL, old_publication_probe_thread, &probe) != 0)
        return -1;
    if (wait_bool(&probe.paused) != 0 ||
        !atomic_load_explicit(&probe.claimed, memory_order_acquire) ||
        !atomic_load_explicit(&probe.reached, memory_order_acquire)) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        (void)pthread_join(thread, NULL);
        return -1;
    }
    atomic_store_explicit(&probe.release, true, memory_order_release);
    return pthread_join(thread, NULL) == 0 ? 0 : -1;
}

static void hardening_control_init(uint32_t byte_offset, int hold_worker,
                                   int hold_reset, int pause_after_claim)
{
    /* Call only after the preceding live driver has been joined. */
    memset(&g_async_hardening_live, 0, sizeof(g_async_hardening_live));
    atomic_init(&g_async_hardening_live.enabled, true);
    atomic_init(&g_async_hardening_live.hold_worker, hold_worker != 0);
    atomic_init(&g_async_hardening_live.worker_gate_reached, false);
    atomic_init(&g_async_hardening_live.event_full_claimed, false);
    atomic_init(&g_async_hardening_live.event_full_reached, false);
    atomic_init(&g_async_hardening_live.release_worker, false);
    atomic_init(&g_async_hardening_live.hold_reset, hold_reset != 0);
    atomic_init(&g_async_hardening_live.reset_gate_reached, false);
    atomic_init(&g_async_hardening_live.release_reset, false);
    atomic_init(&g_async_hardening_live.inject_fatal, false);
    atomic_init(&g_async_hardening_live.abort, false);
    atomic_init(&g_async_hardening_live.pause_after_event_full_claim,
                pause_after_claim != 0);
    atomic_init(&g_async_hardening_live.event_full_claim_pause_reached, false);
    atomic_init(&g_async_hardening_live.release_event_full_metadata, false);
    atomic_init(&g_async_hardening_live.position_changed, false);
    atomic_init(&g_async_hardening_live.event_occupancy, 0U);
    atomic_init(&g_async_hardening_live.event_tail, 0U);
    atomic_init(&g_async_hardening_live.blocked_sequence, UINT64_MAX);
    atomic_init(&g_async_hardening_live.position_before, 0U);
    atomic_init(&g_async_hardening_live.position_during, 0U);
    atomic_init(&g_async_hardening_live.byte_wrap_count, 0U);
    g_async_hardening_live.byte_empty_offset = byte_offset;
}

static void hardening_abort_all_gates(void)
{
    /* Abort is a distinct terminal gate state.  Do not also publish a normal
     * release: different atomics could otherwise let a waiter take the normal
     * path before it observes abort. */
    atomic_store_explicit(&g_async_hardening_live.abort, true,
                          memory_order_release);
}

static void *live_driver_thread(void *opaque)
{
    struct live_driver *driver = opaque;
    driver->result = np2audio86_guest_async_4a_main();
    return NULL;
}

static int join_live_driver(pthread_t *thread, int *created,
                            const struct live_driver *driver,
                            int expect_failure)
{
    if (!*created || pthread_join(*thread, NULL) != 0) {
        fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_JOIN=FAIL\n");
        return -1;
    }
    *created = 0;
    if (np2audio86_guest_host_sink_is_bound()) {
        fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_SINK=STILL_BOUND\n");
        return -1;
    }
    if (expect_failure ? driver->result == 0 : driver->result != 0) {
        fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_DRIVER_RESULT=%d expected_failure=%d\n",
                driver->result, expect_failure);
        return -1;
    }
    return 0;
}

/* The pause is after the one-shot claim but before final publication.  With
 * the old overloaded gate, reached would already be true at this cut point. */
static int run_live_event_case(int pause_after_claim, int force_failure)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int thread_created = 0;
    int normal_release = 0;
    int result = -1;

    hardening_control_init(NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 4U, 1, 0,
                           pause_after_claim);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    thread_created = 1;
    if (wait_bool(&g_async_hardening_live.worker_gate_reached) != 0) goto done;
    if (pause_after_claim) {
        if (wait_bool(&g_async_hardening_live.event_full_claim_pause_reached) != 0 ||
            !atomic_load_explicit(&g_async_hardening_live.event_full_claimed,
                                  memory_order_acquire) ||
            atomic_load_explicit(&g_async_hardening_live.event_full_reached,
                                 memory_order_acquire)) goto done;
        atomic_store_explicit(&g_async_hardening_live.release_event_full_metadata,
                              true, memory_order_release);
    }
    if (wait_bool(&g_async_hardening_live.event_full_reached) != 0 ||
        atomic_load_explicit(&g_async_hardening_live.event_occupancy,
                             memory_order_acquire) != NP2_AUDIO86_ASYNC_EVENT_CAPACITY ||
        atomic_load_explicit(&g_async_hardening_live.event_tail,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_async_hardening_live.blocked_sequence,
                             memory_order_acquire) != NP2_AUDIO86_ASYNC_EVENT_CAPACITY ||
        atomic_load_explicit(&g_async_hardening_live.position_changed,
                             memory_order_acquire)) goto done;
    if (force_failure) goto done;
    atomic_store_explicit(&g_async_hardening_live.release_worker, true,
                          memory_order_release);
    normal_release = 1;
    result = join_live_driver(&thread, &thread_created, &driver, 0);
    if (result == 0 && atomic_load_explicit(&g_async_hardening_live.byte_wrap_count,
                                            memory_order_acquire) == 0U) result = -1;
done:
    if (thread_created) {
        if (!normal_release) hardening_abort_all_gates();
        if (join_live_driver(&thread, &thread_created, &driver,
                             force_failure || !normal_release) != 0) {
            result = -1;
        } else if (force_failure) {
            result = 0;
        }
    }
    return result;
}

static int run_live_event_pressure(unsigned repetitions)
{
    unsigned i;
    for (i = 0U; i < repetitions; ++i)
        if (run_live_event_case(0, 0) != 0) return -1;
    return 0;
}

static int run_claim_publication_regression(void)
{
    return run_old_publication_reproduction() == 0 &&
                   run_live_event_case(1, 0) == 0 ? 0 : -1;
}

static int run_failure_cleanup_regression(void)
{ return run_live_event_case(0, 1); }

static int run_reset_wait_fatal(void)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int thread_created = 0;
    int result = -1;
    hardening_control_init(0U, 0, 1, 0);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    thread_created = 1;
    if (wait_bool(&g_async_hardening_live.reset_gate_reached) != 0) goto done;
    hardening_abort_all_gates();
    result = join_live_driver(&thread, &thread_created, &driver, 1);
done:
    if (thread_created) {
        hardening_abort_all_gates();
        if (join_live_driver(&thread, &thread_created, &driver, 1) != 0) result = -1;
    }
    return result;
}

static void supplemental_init(struct supplemental_context *context)
{
    memset(context, 0, sizeof(*context));
    np2audio86_event_ring_init(&context->events);
    np2audio86_byte_ring_init(&context->bytes);
    atomic_init(&context->first_error, ASYNC_ERROR_NONE);
    atomic_init(&context->byte_full, false);
    atomic_init(&context->event_full, false);
    atomic_init(&context->consumer_waiting, false);
    atomic_init(&context->producer_done, false);
}

static int supplemental_failed(const struct supplemental_context *context)
{
    return atomic_load_explicit(&context->first_error, memory_order_acquire) !=
           ASYNC_ERROR_NONE;
}

static int supplemental_join_abort(struct supplemental_context *context,
                                   pthread_t *thread, int *created)
{
    if (!*created) return 0;
    atomic_store_explicit(&context->first_error, ASYNC_ERROR_STOP,
                          memory_order_release);
    if (pthread_join(*thread, NULL) != 0) return -1;
    *created = 0;
    return 0;
}

static int supplemental_push_event(struct supplemental_context *context,
                                   uint64_t sequence, uint32_t opcode,
                                   uint32_t payload)
{
    const struct np2audio86_event event = {0U, sequence, opcode, payload};
    for (;;) {
        const int status = np2audio86_event_ring_enqueue(&context->events, &event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) return 0;
        if (status != NP2_AUDIO86_TRANSPORT_FULL) return -1;
        atomic_store_explicit(&context->event_full, true, memory_order_release);
        if (supplemental_failed(context)) return -1;
        sched_yield();
    }
}

static int supplemental_push_run(struct supplemental_context *context,
                                 uint64_t sequence, const uint8_t *bytes,
                                 size_t count)
{
    for (;;) {
        const int status = np2audio86_byte_ring_push(&context->bytes, bytes, count);
        if (status == NP2_AUDIO86_TRANSPORT_OK) break;
        if (status != NP2_AUDIO86_TRANSPORT_FULL) return -1;
        atomic_store_explicit(&context->byte_full, true, memory_order_release);
        if (supplemental_failed(context)) return -1;
        sched_yield();
    }
    return supplemental_push_event(context, sequence,
                                   NP2_AUDIO86_GUEST_TRANSPORT_DATA_RUN,
                                   (uint32_t)count);
}

static int supplemental_consume_one(struct supplemental_context *context)
{
    const struct np2audio86_event *event = NULL;
    uint8_t local[16];
    if (np2audio86_event_ring_peek(&context->events, &event) !=
        NP2_AUDIO86_TRANSPORT_OK) return -1;
    if (event->opcode == NP2_AUDIO86_GUEST_TRANSPORT_DATA_RUN) {
        if (event->payload == 0U || event->payload > sizeof(local) ||
            np2audio86_byte_ring_pop(&context->bytes, local, event->payload) !=
                NP2_AUDIO86_TRANSPORT_OK ||
            context->output_count + event->payload > sizeof(context->output)) return -1;
        memcpy(context->output + context->output_count, local, event->payload);
        context->output_count += event->payload;
    }
    return np2audio86_event_ring_consume(&context->events) ==
                   NP2_AUDIO86_TRANSPORT_OK ? 0 : -1;
}

static void *supplemental_run_thread(void *opaque)
{
    struct run_request *request = opaque;
    request->result = supplemental_push_run(request->context, request->sequence,
                                            request->bytes, request->count);
    atomic_store_explicit(&request->context->producer_done, true,
                          memory_order_release);
    return NULL;
}

static int run_byte_full_cases(void)
{
    static const uint8_t a[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint8_t b[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t c[4] = {16, 17, 18, 19};
    uint8_t expected[20];
    struct supplemental_context context;
    struct run_request request;
    pthread_t thread;
    int created = 0;
    int result = -1;
    memcpy(expected, a, sizeof(a));
    memcpy(expected + sizeof(a), b, sizeof(b));
    memcpy(expected + sizeof(a) + sizeof(b), c, sizeof(c));
    supplemental_init(&context);
    if (supplemental_push_run(&context, 0U, a, sizeof(a)) != 0 ||
        supplemental_push_run(&context, 1U, b, sizeof(b)) != 0) goto done;
    request = (struct run_request){&context, 2U, c, sizeof(c), -1};
    if (pthread_create(&thread, NULL, supplemental_run_thread, &request) != 0) goto done;
    created = 1;
    if (wait_bool(&context.byte_full) != 0 || supplemental_consume_one(&context) != 0 ||
        pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    if (request.result != 0 || supplemental_consume_one(&context) != 0 ||
        supplemental_consume_one(&context) != 0 || context.output_count != sizeof(expected) ||
        memcmp(context.output, expected, sizeof(expected)) != 0 ||
        np2audio86_event_ring_occupancy(&context.events) != 0U ||
        np2audio86_byte_ring_occupancy(&context.bytes) != 0U) goto done;
    supplemental_init(&context);
    if (supplemental_push_run(&context, 0U, a, sizeof(a)) != 0 ||
        supplemental_push_run(&context, 1U, b, sizeof(b)) != 0) goto done;
    request = (struct run_request){&context, 2U, c, sizeof(c), -1};
    if (pthread_create(&thread, NULL, supplemental_run_thread, &request) != 0) goto done;
    created = 1;
    if (wait_bool(&context.byte_full) != 0) goto done;
    atomic_store_explicit(&context.first_error, ASYNC_ERROR_STOP, memory_order_release);
    if (pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    result = request.result != 0 ? 0 : -1;
done:
    if (supplemental_join_abort(&context, &thread, &created) != 0) result = -1;
    return result;
}

static int run_split_pressure(void)
{
    static const uint8_t payload[8] = {0xa1, 0xb2, 0xc3, 0xd4,
                                       0xe5, 0xf6, 0x17, 0x28};
    struct supplemental_context context;
    struct run_request request;
    pthread_t thread;
    unsigned i;
    int created = 0;
    int result = -1;
    supplemental_init(&context);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i)
        if (supplemental_push_event(&context, i, NP2AUDIO86_TRACE_OPNA_REGISTER, i) != 0)
            goto done;
    request = (struct run_request){&context, NP2_AUDIO86_ASYNC_EVENT_CAPACITY,
                                   payload, sizeof(payload), -1};
    if (pthread_create(&thread, NULL, supplemental_run_thread, &request) != 0) goto done;
    created = 1;
    if (wait_bool(&context.event_full) != 0 ||
        np2audio86_byte_ring_occupancy(&context.bytes) != sizeof(payload) ||
        supplemental_consume_one(&context) != 0 || pthread_join(thread, NULL) != 0)
        goto done;
    created = 0;
    if (request.result != 0) goto done;
    while (np2audio86_event_ring_occupancy(&context.events) != 0U)
        if (supplemental_consume_one(&context) != 0) goto done;
    if (context.output_count != sizeof(payload) ||
        memcmp(context.output, payload, sizeof(payload)) != 0 ||
        np2audio86_byte_ring_occupancy(&context.bytes) != 0U) goto done;
    supplemental_init(&context);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i)
        if (supplemental_push_event(&context, i, NP2AUDIO86_TRACE_OPNA_REGISTER, i) != 0)
            goto done;
    request = (struct run_request){&context, NP2_AUDIO86_ASYNC_EVENT_CAPACITY,
                                   payload, sizeof(payload), -1};
    if (pthread_create(&thread, NULL, supplemental_run_thread, &request) != 0) goto done;
    created = 1;
    if (wait_bool(&context.event_full) != 0 ||
        np2audio86_byte_ring_occupancy(&context.bytes) != sizeof(payload)) goto done;
    atomic_store_explicit(&context.first_error, ASYNC_ERROR_STOP, memory_order_release);
    if (pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    result = request.result != 0 &&
             np2audio86_event_ring_occupancy(&context.events) ==
                 NP2_AUDIO86_ASYNC_EVENT_CAPACITY &&
             np2audio86_byte_ring_occupancy(&context.bytes) == sizeof(payload) ? 0 : -1;
done:
    if (supplemental_join_abort(&context, &thread, &created) != 0) result = -1;
    return result;
}

static int run_event_full_fatal(void)
{
    struct supplemental_context context;
    struct np2audio86_event event = {0U, NP2_AUDIO86_ASYNC_EVENT_CAPACITY,
                                      NP2AUDIO86_TRACE_OPNA_REGISTER, 0U};
    struct run_request request;
    pthread_t thread;
    unsigned i;
    int created = 0;
    int result = -1;
    supplemental_init(&context);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i)
        if (supplemental_push_event(&context, i, NP2AUDIO86_TRACE_OPNA_REGISTER, i) != 0)
            goto done;
    request = (struct run_request){&context, event.sequence,
                                   (const uint8_t *)&event, 0U, -1};
    if (pthread_create(&thread, NULL, supplemental_run_thread, &request) != 0) goto done;
    created = 1;
    if (wait_bool(&context.event_full) != 0) goto done;
    atomic_store_explicit(&context.first_error, ASYNC_ERROR_STOP, memory_order_release);
    if (pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    result = request.result != 0 &&
             np2audio86_event_ring_occupancy(&context.events) ==
                 NP2_AUDIO86_ASYNC_EVENT_CAPACITY ? 0 : -1;
done:
    if (supplemental_join_abort(&context, &thread, &created) != 0) result = -1;
    return result;
}

static void *supplemental_consumer_thread(void *opaque)
{
    struct supplemental_context *context = opaque;
    atomic_store_explicit(&context->consumer_waiting, true, memory_order_release);
    while (!atomic_load_explicit(&context->producer_done, memory_order_acquire)) {
        if (supplemental_failed(context)) return NULL;
        sched_yield();
    }
    return NULL;
}

static int run_consumer_and_first_error_cases(void)
{
    struct supplemental_context context;
    pthread_t thread;
    int expected = ASYNC_ERROR_NONE;
    int created = 0;
    int result = -1;
    supplemental_init(&context);
    if (pthread_create(&thread, NULL, supplemental_consumer_thread, &context) != 0)
        goto done;
    created = 1;
    if (wait_bool(&context.consumer_waiting) != 0 ||
        !atomic_compare_exchange_strong_explicit(&context.first_error, &expected,
                                                  ASYNC_ERROR_STOP,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire) ||
        pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    expected = ASYNC_ERROR_NONE;
    (void)atomic_compare_exchange_strong_explicit(&context.first_error, &expected,
                                                   ASYNC_ERROR_RENDER,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
    result = atomic_load_explicit(&context.first_error, memory_order_acquire) ==
                     ASYNC_ERROR_STOP ? 0 : -1;
done:
    if (supplemental_join_abort(&context, &thread, &created) != 0) result = -1;
    return result;
}

static int run_reset_continuation(void)
{
    struct np2audio86_render_state worker;
    uint8_t source[ASYNC_SOURCE_BYTES];
    struct np2audio86_guest_action pre = {0U, 0U, NP2AUDIO86_TRACE_OPNA_REGISTER,
                                           UINT32_C(0x2208), 0U, 0U,
                                           NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER};
    struct np2audio86_guest_action reset = {1U, 1U, NP2AUDIO86_TRACE_RESET_BARRIER,
                                             0U, 0U, 0U,
                                             NP2_AUDIO86_GUEST_ACTION_RESET};
    struct np2audio86_guest_action post = {2U, 2U, NP2AUDIO86_TRACE_OPNA_REGISTER,
                                            UINT32_C(0x2209), 0U, 0U,
                                            NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER};
    return np2audio86_guest_action_prime_worker(&worker, source, sizeof(source)) == 0 &&
                   np2audio86_guest_action_apply(&worker, &pre, NULL, 0U, source,
                                                  sizeof(source)) == 0 &&
                   np2audio86_guest_action_apply(&worker, &reset, NULL, 0U, source,
                                                  sizeof(source)) == 0 &&
                   np2audio86_guest_action_apply(&worker, &post, NULL, 0U, source,
                                                  sizeof(source)) == 0 ? 0 : -1;
}

static int run_case(const char *name)
{
    if (strcmp(name, "claim-publication") == 0) {
        if (run_claim_publication_regression() != 0) return -1;
        printf("TEST_FIRST_REPRODUCTION=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_CUTPOINT_SUCCESS=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "live-event-pressure") == 0) {
        if (run_live_event_pressure(1U) != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_DEFAULT_RING_ABI=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_FULL=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_FULL_GUEST_TIME_PAUSED=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_WRAP=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_BYTE_WRAP=PASS\n");
    } else if (strcmp(name, "failure-cleanup") == 0) {
        if (run_failure_cleanup_regression() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_CUTPOINT_FAILURE=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FAILURE_CLEANUP=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "reset-fatal") == 0) {
        if (run_reset_wait_fatal() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_RESET_PRODUCER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "supplemental") == 0) {
        if (run_byte_full_cases() != 0 || run_split_pressure() != 0 ||
            run_reset_continuation() != 0 || run_event_full_fatal() != 0 ||
            run_consumer_and_first_error_cases() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_BYTE_FULL=PASS\n");
        printf("DATA_RUN_RESERVATION_ORDER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_DATA_RUN_SPLIT_PRESSURE=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_POST_RESET_CONTINUATION=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_EVENT_PRODUCER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_BYTE_PRODUCER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_CONSUMERS=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FIRST_ERROR_IMMUTABLE=PASS\n");
    } else {
        return -1;
    }
    printf("AUDIO86_GUEST_ASYNC_HARDENING_CASE=%s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = "live-event-pressure";
    if (argc == 3 && strcmp(argv[1], "--case") == 0) name = argv[2];
    else if (argc != 1) return 2;
    return run_case(name) == 0 ? 0 : 1;
}
