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
    _Atomic bool producer_terminal;
    _Atomic bool worker_terminal;
    _Atomic bool coordinator_terminal;
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

static int wait_error(const _Atomic int *value)
{
    for (;;) {
        if (atomic_load_explicit(value, memory_order_acquire) != ASYNC_ERROR_NONE) {
            return 0;
        }
        sched_yield();
    }
}

static int wait_u32_nonzero(const _Atomic uint32_t *value)
{
    for (;;) {
        if (atomic_load_explicit(value, memory_order_acquire) != 0U) return 0;
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
    atomic_init(&g_async_hardening_live.event_full_sample_ready, false);
    atomic_init(&g_async_hardening_live.release_worker, false);
    atomic_init(&g_async_hardening_live.hold_reset, hold_reset != 0);
    atomic_init(&g_async_hardening_live.reset_gate_reached, false);
    atomic_init(&g_async_hardening_live.release_reset, false);
    atomic_init(&g_async_hardening_live.inject_fatal, false);
    atomic_init(&g_async_hardening_live.abort, false);
    atomic_init(&g_async_hardening_live.worker_fatal_requested, false);
    atomic_init(&g_async_hardening_live.producer_fatal_requested, false);
    atomic_init(&g_async_hardening_live.hold_producer, false);
    atomic_init(&g_async_hardening_live.producer_gate_reached, false);
    atomic_init(&g_async_hardening_live.release_producer, false);
    atomic_init(&g_async_hardening_live.worker_waiting_for_action, false);
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
    atomic_init(&g_async_hardening_live.event_wrap_count, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_target, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_release, false);
    atomic_init(&g_async_hardening_live.cutpoint_fatal, false);
    for (unsigned point = 0U; point < NP2_AUDIO86_ASYNC_CP_COUNT; ++point) {
        atomic_init(&g_async_hardening_live.cutpoint_entered[point], 0U);
    }
    atomic_init(&g_async_hardening_live.cutpoint_event_head, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_event_tail, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_byte_head, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_byte_tail, 0U);
    atomic_init(&g_async_hardening_live.cutpoint_auxiliary, 0U);
    atomic_init(&g_async_hardening_live.schedule_mode, 0U);
    atomic_init(&g_async_hardening_live.schedule_turn, 0U);
    atomic_init(&g_async_hardening_live.schedule_handoffs, 0U);
    atomic_init(&g_async_hardening_live.first_error_seen, ASYNC_ERROR_NONE);
    atomic_init(&g_async_hardening_live.oracle_ready, false);
    g_async_hardening_live.byte_empty_offset = byte_offset;
}

static int hardening_oracle_ready(void)
{
    return atomic_load_explicit(&g_async_hardening_live.oracle_ready,
                                memory_order_acquire);
}

static void print_hardening_digest(const char *name,
                                   const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    printf("%s=", name);
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
    putchar('\n');
}

static void print_pressure_digests(void)
{
    print_hardening_digest("AUDIO86_GUEST_ASYNC_PRESSURE_WORKER_TRACE_SHA256",
                           g_async_hardening_live.oracle_worker_sha);
    print_hardening_digest("AUDIO86_GUEST_ASYNC_PRESSURE_PRE_RESET_PCM_SHA256",
                           g_async_hardening_live.oracle_pre_sha);
    print_hardening_digest("AUDIO86_GUEST_ASYNC_PRESSURE_FULL_PCM_SHA256",
                           g_async_hardening_live.oracle_full_sha);
    print_hardening_digest("AUDIO86_GUEST_ASYNC_PRESSURE_BYTE_SHA256",
                           g_async_hardening_live.oracle_byte_sha);
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
        atomic_load_explicit(&g_async_hardening_live.position_before,
                             memory_order_acquire) !=
            np2audio86_guest_host_current_cpu_position()) goto done;
    if (wait_bool(&g_async_hardening_live.event_full_sample_ready) != 0 ||
        atomic_load_explicit(&g_async_hardening_live.position_changed,
                             memory_order_acquire) ||
        atomic_load_explicit(&g_async_hardening_live.position_during,
                             memory_order_acquire) !=
            atomic_load_explicit(&g_async_hardening_live.position_before,
                                 memory_order_acquire)) goto done;
    if (force_failure) {
        /* The producer is still in its actual event-ring FULL loop. */
        atomic_store_explicit(&g_async_hardening_live.inject_fatal, true,
                              memory_order_release);
        if (wait_error(&g_async_hardening_live.first_error_seen) != 0) goto done;
        /* This is cleanup only: the preceding acquire observes the producer's
         * native fatal first-error, so the held worker cannot be mistaken for
         * the source of the wake. */
        hardening_abort_all_gates();
        result = join_live_driver(&thread, &thread_created, &driver, 1);
        return result;
    }
    atomic_store_explicit(&g_async_hardening_live.release_worker, true,
                          memory_order_release);
    normal_release = 1;
    result = join_live_driver(&thread, &thread_created, &driver, 0);
    if (result == 0 && (!hardening_oracle_ready() ||
                        atomic_load_explicit(&g_async_hardening_live.byte_wrap_count,
                                             memory_order_acquire) == 0U ||
                        atomic_load_explicit(&g_async_hardening_live.event_wrap_count,
                                             memory_order_acquire) == 0U)) result = -1;
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

static int run_live_schedule(uint32_t mode)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int created = 0;
    int result = -1;
    hardening_control_init(NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 4U, 0, 0, 0);
    atomic_store_explicit(&g_async_hardening_live.schedule_mode, mode,
                          memory_order_release);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    created = 1;
    result = join_live_driver(&thread, &created, &driver, 0);
    if (result == 0 && (!hardening_oracle_ready() ||
                        (mode == 3U &&
                         atomic_load_explicit(&g_async_hardening_live.schedule_handoffs,
                                              memory_order_acquire) == 0U))) result = -1;
done:
    if (created) {
        hardening_abort_all_gates();
        if (join_live_driver(&thread, &created, &driver, 1) != 0) result = -1;
    }
    return result;
}

static int validate_cutpoint_snapshot(
    enum np2audio86_async_hardening_cutpoint point)
{
    const uint32_t event_head = atomic_load_explicit(
        &g_async_hardening_live.cutpoint_event_head, memory_order_acquire);
    const uint32_t event_tail = atomic_load_explicit(
        &g_async_hardening_live.cutpoint_event_tail, memory_order_acquire);
    const uint32_t byte_head = atomic_load_explicit(
        &g_async_hardening_live.cutpoint_byte_head, memory_order_acquire);
    const uint32_t byte_tail = atomic_load_explicit(
        &g_async_hardening_live.cutpoint_byte_tail, memory_order_acquire);
    const uint64_t auxiliary = atomic_load_explicit(
        &g_async_hardening_live.cutpoint_auxiliary, memory_order_acquire);
    const uint32_t event_occupancy = event_head - event_tail;
    const uint32_t byte_occupancy = byte_head - byte_tail;
    switch (point) {
    case NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_HEAD:
        return byte_occupancy == 0U && auxiliary == 1U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_BYTE_HEAD_BEFORE_DATA_RUN:
        return byte_occupancy == auxiliary && auxiliary == 8U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_EVENT_SLOT_BEFORE_HEAD:
        return event_occupancy < NP2_AUDIO86_ASYNC_EVENT_CAPACITY ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_DATA_RUN_BEFORE_BYTE_COPY:
    case NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_TAIL:
        return byte_occupancy >= auxiliary && auxiliary == 8U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_EVENT_BEFORE_TAIL:
        return event_occupancy != 0U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_RESET_BEFORE_APPLY:
    case NP2_AUDIO86_ASYNC_CP_RESET_AFTER_APPLY:
        return auxiliary == 18U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_ACK_BEFORE_PRODUCER_RESUME:
        return auxiliary == 18U ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_PRODUCER_DONE_BEFORE_RELEASE:
        /* The reset ACK precedes event-tail retirement.  Therefore its final
         * descriptor may still be visible here, but no byte payload remains
         * and producer_done itself has not been release-published. */
        return auxiliary == 19U && event_occupancy <= 1U && byte_occupancy == 0U
                   ? 0 : -1;
    case NP2_AUDIO86_ASYNC_CP_PRODUCER_DONE_BEFORE_TAIL_RENDER:
        return auxiliary == 19U && event_occupancy == 0U && byte_occupancy == 0U
                   ? 0 : -1;
    default:
        return -1;
    }
}

static int run_live_cutpoint(enum np2audio86_async_hardening_cutpoint point,
                             int fatal)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int created = 0;
    int result = -1;
    hardening_control_init(NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 4U, 0, 0, 0);
    atomic_store_explicit(&g_async_hardening_live.cutpoint_target, point,
                          memory_order_release);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    created = 1;
    if (wait_u32_nonzero(&g_async_hardening_live.cutpoint_entered[point]) != 0 ||
        atomic_load_explicit(&g_async_hardening_live.cutpoint_entered[point],
                             memory_order_acquire) != 1U ||
        validate_cutpoint_snapshot(point) != 0) goto done;
    if (fatal) {
        atomic_store_explicit(&g_async_hardening_live.cutpoint_fatal, true,
                              memory_order_release);
        result = join_live_driver(&thread, &created, &driver, 1);
        if (result == 0) {
            const int expected = (point == NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_HEAD ||
                                  point == NP2_AUDIO86_ASYNC_CP_EVENT_SLOT_BEFORE_HEAD)
                                     ? ASYNC_ERROR_TRANSPORT : ASYNC_ERROR_STOP;
            if (atomic_load_explicit(&g_async_hardening_live.first_error_seen,
                                     memory_order_acquire) != expected) result = -1;
        }
        return result;
    }
    atomic_store_explicit(&g_async_hardening_live.cutpoint_release, true,
                          memory_order_release);
    result = join_live_driver(&thread, &created, &driver, 0);
    if (result == 0 && !hardening_oracle_ready()) result = -1;
done:
    if (created) {
        hardening_abort_all_gates();
        if (join_live_driver(&thread, &created, &driver, 1) != 0) result = -1;
    }
    return result;
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
    /* The live worker owns this internal reset gate.  It now takes its
     * native RENDER first-error path while the real guest producer remains
     * blocked in publish_event's reset-ACK loop. */
    atomic_store_explicit(&g_async_hardening_live.worker_fatal_requested, true,
                          memory_order_release);
    result = join_live_driver(&thread, &thread_created, &driver, 1);
    if (result == 0 && atomic_load_explicit(&g_async_hardening_live.first_error_seen,
                                            memory_order_acquire) !=
                           ASYNC_ERROR_RENDER) result = -1;
done:
    if (thread_created) {
        hardening_abort_all_gates();
        if (join_live_driver(&thread, &thread_created, &driver, 1) != 0) result = -1;
    }
    return result;
}

static int run_live_producer_fatal_worker_wake(void)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int created = 0;
    int result = -1;
    hardening_control_init(0U, 0, 0, 0);
    atomic_store_explicit(&g_async_hardening_live.hold_producer, true,
                          memory_order_release);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    created = 1;
    if (wait_bool(&g_async_hardening_live.worker_waiting_for_action) != 0 ||
        wait_bool(&g_async_hardening_live.producer_gate_reached) != 0) goto done;
    atomic_store_explicit(&g_async_hardening_live.producer_fatal_requested, true,
                          memory_order_release);
    atomic_store_explicit(&g_async_hardening_live.release_producer, true,
                          memory_order_release);
    if (wait_error(&g_async_hardening_live.first_error_seen) != 0) goto done;
    hardening_abort_all_gates();
    result = join_live_driver(&thread, &created, &driver, 1);
    if (result == 0 && atomic_load_explicit(&g_async_hardening_live.first_error_seen,
                                            memory_order_acquire) !=
                           ASYNC_ERROR_DISPATCH) result = -1;
done:
    if (created) {
        hardening_abort_all_gates();
        (void)join_live_driver(&thread, &created, &driver, 1);
    }
    return result;
}

static int run_live_worker_fatal_coordinator_wake(void)
{
    struct live_driver driver = {0};
    pthread_t thread;
    int created = 0;
    int result = -1;
    hardening_control_init(0U, 1, 0, 0);
    if (pthread_create(&thread, NULL, live_driver_thread, &driver) != 0) goto done;
    created = 1;
    if (wait_bool(&g_async_hardening_live.worker_gate_reached) != 0) goto done;
    atomic_store_explicit(&g_async_hardening_live.worker_fatal_requested, true,
                          memory_order_release);
    if (wait_error(&g_async_hardening_live.first_error_seen) != 0) goto done;
    hardening_abort_all_gates();
    result = join_live_driver(&thread, &created, &driver, 1);
    if (result == 0 && atomic_load_explicit(&g_async_hardening_live.first_error_seen,
                                            memory_order_acquire) !=
                           ASYNC_ERROR_RENDER) result = -1;
done:
    if (created) {
        hardening_abort_all_gates();
        (void)join_live_driver(&thread, &created, &driver, 1);
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
    atomic_init(&context->producer_terminal, false);
    atomic_init(&context->worker_terminal, false);
    atomic_init(&context->coordinator_terminal, false);
}

static int supplemental_failed(const struct supplemental_context *context)
{
    return atomic_load_explicit(&context->first_error, memory_order_acquire) !=
           ASYNC_ERROR_NONE;
}

static int supplemental_fail(struct supplemental_context *context,
                             enum async_error error)
{
    return first_error_publish(&context->first_error, error);
}

static int supplemental_join_abort(struct supplemental_context *context,
                                   pthread_t *thread, int *created)
{
    if (!*created) return 0;
    (void)supplemental_fail(context, ASYNC_ERROR_STOP);
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
    atomic_store_explicit(&request->context->producer_terminal, true,
                          memory_order_release);
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
    (void)supplemental_fail(&context, ASYNC_ERROR_STOP);
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
    (void)supplemental_fail(&context, ASYNC_ERROR_STOP);
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

static void *split_pressure_worker_thread(void *opaque)
{
    struct supplemental_context *context = opaque;
    /* The worker is deliberately held with the event ring genuinely full;
     * the producer has committed bytes but cannot publish its descriptor. */
    atomic_store_explicit(&context->consumer_waiting, true, memory_order_release);
    while (!supplemental_failed(context)) sched_yield();
    atomic_store_explicit(&context->worker_terminal, true, memory_order_release);
    return NULL;
}

static void *split_pressure_coordinator_thread(void *opaque)
{
    struct supplemental_context *context = opaque;
    while (!atomic_load_explicit(&context->producer_done, memory_order_acquire) ||
           !atomic_load_explicit(&context->worker_terminal, memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&context->coordinator_terminal, true,
                          memory_order_release);
    return NULL;
}

static int run_split_pressure_fatal_lifecycle(void)
{
    static const uint8_t payload[8] = {0xa1, 0xb2, 0xc3, 0xd4,
                                       0xe5, 0xf6, 0x17, 0x28};
    struct supplemental_context context;
    struct run_request request;
    pthread_t producer;
    pthread_t worker;
    pthread_t coordinator;
    unsigned i;
    int producer_created = 0;
    int worker_created = 0;
    int coordinator_created = 0;
    int result = -1;
    supplemental_init(&context);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i) {
        if (supplemental_push_event(&context, i, NP2AUDIO86_TRACE_OPNA_REGISTER,
                                    i) != 0) goto done;
    }
    if (pthread_create(&worker, NULL, split_pressure_worker_thread, &context) != 0)
        goto done;
    worker_created = 1;
    if (pthread_create(&coordinator, NULL, split_pressure_coordinator_thread,
                       &context) != 0) goto done;
    coordinator_created = 1;
    request = (struct run_request){&context, NP2_AUDIO86_ASYNC_EVENT_CAPACITY,
                                   payload, sizeof(payload), -1};
    if (pthread_create(&producer, NULL, supplemental_run_thread, &request) != 0)
        goto done;
    producer_created = 1;
    if (wait_bool(&context.consumer_waiting) != 0 ||
        wait_bool(&context.event_full) != 0 ||
        np2audio86_byte_ring_occupancy(&context.bytes) != sizeof(payload) ||
        np2audio86_event_ring_occupancy(&context.events) !=
            NP2_AUDIO86_ASYNC_EVENT_CAPACITY) goto done;
    if (!supplemental_fail(&context, ASYNC_ERROR_DISPATCH)) goto done;
    if (supplemental_fail(&context, ASYNC_ERROR_STOP)) goto done;
    if (pthread_join(producer, NULL) != 0) goto done;
    producer_created = 0;
    if (pthread_join(worker, NULL) != 0) goto done;
    worker_created = 0;
    if (pthread_join(coordinator, NULL) != 0) goto done;
    coordinator_created = 0;
    result = request.result != 0 &&
             atomic_load_explicit(&context.producer_terminal, memory_order_acquire) &&
             atomic_load_explicit(&context.worker_terminal, memory_order_acquire) &&
             atomic_load_explicit(&context.coordinator_terminal,
                                  memory_order_acquire) &&
             atomic_load_explicit(&context.first_error, memory_order_acquire) ==
                 ASYNC_ERROR_DISPATCH &&
             np2audio86_event_ring_occupancy(&context.events) ==
                 NP2_AUDIO86_ASYNC_EVENT_CAPACITY &&
             np2audio86_byte_ring_occupancy(&context.bytes) == sizeof(payload) ? 0 : -1;
done:
    (void)supplemental_fail(&context, ASYNC_ERROR_STOP);
    if (producer_created && pthread_join(producer, NULL) != 0) result = -1;
    if (worker_created && pthread_join(worker, NULL) != 0) result = -1;
    if (coordinator_created && pthread_join(coordinator, NULL) != 0) result = -1;
    return result;
}

struct first_error_competitor {
    _Atomic int *first_error;
    _Atomic bool ready;
    _Atomic bool release;
    _Atomic bool published;
};

static void *first_error_competitor_thread(void *opaque)
{
    struct first_error_competitor *competitor = opaque;
    atomic_store_explicit(&competitor->ready, true, memory_order_release);
    while (!atomic_load_explicit(&competitor->release, memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&competitor->published,
                          first_error_publish(competitor->first_error,
                                              ASYNC_ERROR_STOP),
                          memory_order_release);
    return NULL;
}

static int run_first_error_immutability(void)
{
    struct supplemental_context context;
    struct first_error_competitor competitor;
    pthread_t thread;
    int created = 0;
    int result = -1;
    supplemental_init(&context);
    competitor.first_error = &context.first_error;
    atomic_init(&competitor.ready, false);
    atomic_init(&competitor.release, false);
    atomic_init(&competitor.published, false);
    if (pthread_create(&thread, NULL, first_error_competitor_thread, &competitor) != 0)
        goto done;
    created = 1;
    if (wait_bool(&competitor.ready) != 0 ||
        !supplemental_fail(&context, ASYNC_ERROR_DISPATCH)) goto done;
    atomic_store_explicit(&competitor.release, true, memory_order_release);
    if (pthread_join(thread, NULL) != 0) goto done;
    created = 0;
    result = !atomic_load_explicit(&competitor.published, memory_order_acquire) &&
             atomic_load_explicit(&context.first_error, memory_order_acquire) ==
                 ASYNC_ERROR_DISPATCH ? 0 : -1;
done:
    atomic_store_explicit(&competitor.release, true, memory_order_release);
    if (created && pthread_join(thread, NULL) != 0) result = -1;
    (void)supplemental_fail(&context, ASYNC_ERROR_STOP);
    return result;
}

/* This intentionally small three-event path is a real producer -> SPSC
 * transport -> worker pipeline.  It proves the reset ACK ordering which the
 * canonical trace itself cannot expose because RESET is its final action. */
struct continuation_context {
    struct np2audio86_event_ring events;
    struct np2audio86_render_state worker;
    uint8_t source[ASYNC_SOURCE_BYTES];
    _Atomic int first_error;
    _Atomic bool producer_done;
    _Atomic bool producer_terminal;
    _Atomic bool worker_terminal;
    _Atomic bool reset_published;
    _Atomic bool reset_received;
    _Atomic bool worker_held_before_ack;
    _Atomic bool release_reset;
    _Atomic bool producer_waiting_ack;
    _Atomic bool producer_observed_ack;
    _Atomic bool post_published;
    _Atomic bool post_consumed;
    _Atomic bool pre_mutated_domain_a;
    _Atomic bool reset_baseline_domain_a;
    _Atomic bool post_mutated_domain_a;
    _Atomic uint64_t reset_ack_plus_one;
    uint64_t producer_next;
    uint64_t worker_next;
};

static void continuation_fail(struct continuation_context *context,
                              enum async_error error)
{
    int expected = ASYNC_ERROR_NONE;
    (void)atomic_compare_exchange_strong_explicit(&context->first_error, &expected,
                                                   error, memory_order_acq_rel,
                                                   memory_order_acquire);
}

static int continuation_failed(const struct continuation_context *context)
{
    return atomic_load_explicit(&context->first_error, memory_order_acquire) !=
           ASYNC_ERROR_NONE;
}

static int continuation_push(struct continuation_context *context,
                             uint64_t sequence, uint32_t opcode, uint32_t payload)
{
    const struct np2audio86_event event = {sequence, sequence, opcode, payload};
    for (;;) {
        const int status = np2audio86_event_ring_enqueue(&context->events, &event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) return 0;
        if (status != NP2_AUDIO86_TRANSPORT_FULL || continuation_failed(context)) {
            return -1;
        }
        sched_yield();
    }
}

static void *continuation_producer(void *opaque)
{
    struct continuation_context *context = opaque;
    if (continuation_push(context, 0U, NP2AUDIO86_TRACE_OPNA_REGISTER,
                          UINT32_C(0x307f)) != 0 ||
        continuation_push(context, 1U, NP2AUDIO86_TRACE_RESET_BARRIER, 0U) != 0) {
        continuation_fail(context, ASYNC_ERROR_TRANSPORT);
        atomic_store_explicit(&context->producer_done, true, memory_order_release);
        return NULL;
    }
    context->producer_next = 2U;
    atomic_store_explicit(&context->reset_published, true, memory_order_release);
    atomic_store_explicit(&context->producer_waiting_ack, true, memory_order_release);
    while (atomic_load_explicit(&context->reset_ack_plus_one,
                                memory_order_acquire) != 2U) {
        if (continuation_failed(context)) {
            atomic_store_explicit(&context->producer_waiting_ack, false,
                                  memory_order_release);
            atomic_store_explicit(&context->producer_done, true, memory_order_release);
            return NULL;
        }
        sched_yield();
    }
    atomic_store_explicit(&context->producer_waiting_ack, false, memory_order_release);
    atomic_store_explicit(&context->producer_observed_ack, true, memory_order_release);
    if (continuation_push(context, 2U, NP2AUDIO86_TRACE_OPNA_REGISTER,
                          UINT32_C(0x307e)) != 0) {
        continuation_fail(context, ASYNC_ERROR_TRANSPORT);
        atomic_store_explicit(&context->producer_done, true, memory_order_release);
        return NULL;
    }
    context->producer_next = 3U;
    atomic_store_explicit(&context->post_published, true, memory_order_release);
    atomic_store_explicit(&context->producer_terminal, true, memory_order_release);
    atomic_store_explicit(&context->producer_done, true, memory_order_release);
    return NULL;
}

static int continuation_apply(struct continuation_context *context,
                              const struct np2audio86_event *event,
                              const _OPNGEN *baseline_fm)
{
    struct np2audio86_guest_action action;
    uint8_t kind;
    if (event == NULL || event->sequence != context->worker_next ||
        np2audio86_guest_action_kind_for_opcode(event->opcode, &kind) != 0) {
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.frame_timestamp = event->frame_timestamp;
    action.sequence = event->sequence;
    action.opcode = event->opcode;
    action.payload = event->payload;
    action.kind = kind;
    if (kind == NP2_AUDIO86_GUEST_ACTION_RESET) {
        atomic_store_explicit(&context->reset_received, true, memory_order_release);
        atomic_store_explicit(&context->worker_held_before_ack, true,
                              memory_order_release);
        while (!atomic_load_explicit(&context->release_reset, memory_order_acquire)) {
            if (continuation_failed(context)) return -1;
            sched_yield();
        }
    }
    if (np2audio86_guest_action_apply(&context->worker, &action, NULL, 0U,
                                      context->source, sizeof(context->source)) != 0) {
        return -1;
    }
    if (event->sequence == 0U) {
        atomic_store_explicit(&context->pre_mutated_domain_a,
                              memcmp(&context->worker.fm, baseline_fm,
                                     sizeof(context->worker.fm)) != 0,
                              memory_order_release);
    } else if (kind == NP2_AUDIO86_GUEST_ACTION_RESET) {
        atomic_store_explicit(&context->reset_baseline_domain_a,
                              memcmp(&context->worker.fm, baseline_fm,
                                     sizeof(context->worker.fm)) == 0,
                              memory_order_release);
        atomic_store_explicit(&context->reset_ack_plus_one, event->sequence + 1U,
                              memory_order_release);
    } else if (event->sequence == 2U) {
        atomic_store_explicit(&context->post_mutated_domain_a,
                              memcmp(&context->worker.fm, baseline_fm,
                                     sizeof(context->worker.fm)) != 0,
                              memory_order_release);
        atomic_store_explicit(&context->post_consumed, true, memory_order_release);
    }
    if (np2audio86_event_ring_consume(&context->events) != NP2_AUDIO86_TRANSPORT_OK) {
        return -1;
    }
    ++context->worker_next;
    return 0;
}

static void *continuation_worker(void *opaque)
{
    struct continuation_context *context = opaque;
    _OPNGEN baseline_fm;
    if (np2audio86_guest_action_prime_worker(&context->worker, context->source,
                                             sizeof(context->source)) != 0) {
        continuation_fail(context, ASYNC_ERROR_RENDER);
        return NULL;
    }
    memcpy(&baseline_fm, &context->worker.fm, sizeof(baseline_fm));
    for (;;) {
        const struct np2audio86_event *event = NULL;
        const int status = np2audio86_event_ring_peek(&context->events, &event);
        if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
            if (atomic_load_explicit(&context->producer_done, memory_order_acquire)) {
                break;
            }
            if (continuation_failed(context)) return NULL;
            sched_yield();
            continue;
        }
        if (status != NP2_AUDIO86_TRANSPORT_OK ||
            continuation_apply(context, event, &baseline_fm) != 0) {
            continuation_fail(context, ASYNC_ERROR_DISPATCH);
            return NULL;
        }
    }
    if (!continuation_failed(context) && context->worker_next == 3U &&
        np2audio86_event_ring_occupancy(&context->events) == 0U) {
        atomic_store_explicit(&context->worker_terminal, true, memory_order_release);
    } else {
        continuation_fail(context, ASYNC_ERROR_COMPLETION);
    }
    return NULL;
}

static int run_reset_continuation(void)
{
    struct continuation_context context;
    pthread_t producer;
    pthread_t worker;
    int producer_created = 0;
    int worker_created = 0;
    int result = -1;
    memset(&context, 0, sizeof(context));
    np2audio86_event_ring_init(&context.events);
    atomic_init(&context.first_error, ASYNC_ERROR_NONE);
    atomic_init(&context.producer_done, false);
    atomic_init(&context.producer_terminal, false);
    atomic_init(&context.worker_terminal, false);
    atomic_init(&context.reset_published, false);
    atomic_init(&context.reset_received, false);
    atomic_init(&context.worker_held_before_ack, false);
    atomic_init(&context.release_reset, false);
    atomic_init(&context.producer_waiting_ack, false);
    atomic_init(&context.producer_observed_ack, false);
    atomic_init(&context.post_published, false);
    atomic_init(&context.post_consumed, false);
    atomic_init(&context.pre_mutated_domain_a, false);
    atomic_init(&context.reset_baseline_domain_a, false);
    atomic_init(&context.post_mutated_domain_a, false);
    atomic_init(&context.reset_ack_plus_one, 0U);
    if (pthread_create(&worker, NULL, continuation_worker, &context) != 0) goto done;
    worker_created = 1;
    if (pthread_create(&producer, NULL, continuation_producer, &context) != 0) goto done;
    producer_created = 1;
    if (wait_bool(&context.worker_held_before_ack) != 0 ||
        !atomic_load_explicit(&context.reset_published, memory_order_acquire) ||
        !atomic_load_explicit(&context.reset_received, memory_order_acquire) ||
        !atomic_load_explicit(&context.producer_waiting_ack, memory_order_acquire) ||
        atomic_load_explicit(&context.post_published, memory_order_acquire)) goto done;
    atomic_store_explicit(&context.release_reset, true, memory_order_release);
    if (pthread_join(producer, NULL) != 0) goto done;
    producer_created = 0;
    if (pthread_join(worker, NULL) != 0) goto done;
    worker_created = 0;
    result = !continuation_failed(&context) && context.producer_next == 3U &&
             context.worker_next == 3U &&
             atomic_load_explicit(&context.producer_observed_ack,
                                  memory_order_acquire) &&
             atomic_load_explicit(&context.post_published, memory_order_acquire) &&
             atomic_load_explicit(&context.post_consumed, memory_order_acquire) &&
             atomic_load_explicit(&context.pre_mutated_domain_a,
                                  memory_order_acquire) &&
             atomic_load_explicit(&context.reset_baseline_domain_a,
                                  memory_order_acquire) &&
             atomic_load_explicit(&context.post_mutated_domain_a,
                                  memory_order_acquire) ? 0 : -1;
    if (result != 0) {
        fprintf(stderr,
                "CONTINUATION_FAIL error=%d producer=%" PRIu64 " worker=%" PRIu64
                " ack=%d post_pub=%d post_cons=%d pre=%d baseline=%d post=%d\n",
                atomic_load_explicit(&context.first_error, memory_order_acquire),
                context.producer_next, context.worker_next,
                atomic_load_explicit(&context.producer_observed_ack,
                                     memory_order_acquire),
                atomic_load_explicit(&context.post_published, memory_order_acquire),
                atomic_load_explicit(&context.post_consumed, memory_order_acquire),
                atomic_load_explicit(&context.pre_mutated_domain_a,
                                     memory_order_acquire),
                atomic_load_explicit(&context.reset_baseline_domain_a,
                                     memory_order_acquire),
                atomic_load_explicit(&context.post_mutated_domain_a,
                                     memory_order_acquire));
    }
done:
    atomic_store_explicit(&context.release_reset, true, memory_order_release);
    continuation_fail(&context, ASYNC_ERROR_STOP);
    if (producer_created && pthread_join(producer, NULL) != 0) result = -1;
    if (worker_created && pthread_join(worker, NULL) != 0) result = -1;
    return result;
}

static int run_case(const char *name)
{
    unsigned point;
    if (strcmp(name, "claim-publication") == 0) {
        if (run_claim_publication_regression() != 0) return -1;
        printf("TEST_FIRST_REPRODUCTION=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "live-event-pressure") == 0) {
        if (run_live_event_pressure(1U) != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_DEFAULT_RING_ABI=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_FULL=PASS\n");
        printf("EVENT_FULL_SAMPLE_PUBLICATION=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_FULL_GUEST_TIME_PAUSED=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_EVENT_WRAP_COUNT=%" PRIu32 "\n",
               atomic_load_explicit(&g_async_hardening_live.event_wrap_count,
                                    memory_order_acquire));
        printf("AUDIO86_GUEST_ASYNC_BYTE_WRAP_COUNT=%" PRIu32 "\n",
               atomic_load_explicit(&g_async_hardening_live.byte_wrap_count,
                                    memory_order_acquire));
        print_pressure_digests();
    } else if (strcmp(name, "event-full-fatal") == 0) {
        if (run_failure_cleanup_regression() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_EVENT_PRODUCER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "reset-fatal") == 0) {
        if (run_reset_wait_fatal() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_RESET_PRODUCER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "consumer-wake") == 0) {
        if (run_live_producer_fatal_worker_wake() != 0 ||
            run_live_worker_fatal_coordinator_wake() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_CONSUMERS=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_QUIESCENT=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_HARDENING_SINK_LIFETIME=PASS\n");
    } else if (strcmp(name, "producer-wake") == 0) {
        if (run_live_producer_fatal_worker_wake() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_PRODUCER_TO_WORKER=PASS\n");
    } else if (strcmp(name, "coordinator-wake") == 0) {
        if (run_live_worker_fatal_coordinator_wake() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_FATAL_WAKE_WORKER_TO_COORDINATOR=PASS\n");
    } else if (strcmp(name, "split-pressure-fatal") == 0) {
        if (run_split_pressure_fatal_lifecycle() != 0) return -1;
        printf("DATA_RUN_SPLIT_PRESSURE_FATAL=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_SPLIT_FATAL_FIRST_ERROR=PASS\n");
    } else if (strcmp(name, "byte-full") == 0) {
        if (run_byte_full_cases() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_BYTE_FULL=PASS\n");
    } else if (strcmp(name, "split-pressure") == 0) {
        if (run_split_pressure() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_DATA_RUN_SPLIT_PRESSURE=PASS\n");
    } else if (strcmp(name, "continuation") == 0) {
        if (run_reset_continuation() != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_POST_RESET_CONTINUATION=PASS\n");
    } else if (strcmp(name, "supplemental") == 0) {
        if (run_byte_full_cases() != 0 || run_split_pressure() != 0 ||
            run_reset_continuation() != 0 || run_first_error_immutability() != 0)
            return -1;
        printf("AUDIO86_GUEST_ASYNC_BYTE_FULL=PASS\n");
        printf("DATA_RUN_RESERVATION_ORDER=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_DATA_RUN_SPLIT_PRESSURE=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_POST_RESET_CONTINUATION=PASS\n");
        printf("POST_RESET_STATE=PASS\n");
        printf("AUDIO86_GUEST_ASYNC_FIRST_ERROR_IMMUTABLE=PASS\n");
    } else if (strcmp(name, "schedule-a") == 0 ||
               strcmp(name, "schedule-b") == 0 ||
               strcmp(name, "schedule-c") == 0 ||
               strcmp(name, "schedule-d") == 0) {
        const uint32_t mode = (uint32_t)(name[9] - 'a');
        if (run_live_schedule(mode) != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_SCHEDULE_MODE=%c\n", name[9]);
        printf("AUDIO86_GUEST_ASYNC_SCHEDULE_HANDOFFS=%" PRIu32 "\n",
               atomic_load_explicit(&g_async_hardening_live.schedule_handoffs,
                                    memory_order_acquire));
        print_pressure_digests();
    } else if (sscanf(name, "cutpoint-success-%u", &point) == 1 &&
               point > 0U && point < NP2_AUDIO86_ASYNC_CP_COUNT) {
        if (run_live_cutpoint((enum np2audio86_async_hardening_cutpoint)point,
                               0) != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_CUTPOINT=%u entered=1 invariant=PASS fatal=NA\n",
               point);
    } else if (sscanf(name, "cutpoint-fatal-%u", &point) == 1 &&
               point > 0U && point < NP2_AUDIO86_ASYNC_CP_COUNT) {
        if (run_live_cutpoint((enum np2audio86_async_hardening_cutpoint)point,
                               1) != 0) return -1;
        printf("AUDIO86_GUEST_ASYNC_CUTPOINT=%u entered=1 invariant=PASS fatal=PASS\n",
               point);
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
