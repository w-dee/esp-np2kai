#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2audio86_fixture.h"

struct run_call {
    enum np2audio86_async_mode mode;
    struct np2audio86_async_test_control *control;
    struct np2audio86_async_test_observer *observer;
    struct np2audio86_async_result *result;
    int status;
};

static void *run_call_thread(void *opaque)
{
    struct run_call *call = opaque;
    call->status = np2audio86_async_run_with_test_control(
        call->mode, call->control, call->observer, call->result);
    return NULL;
}

static int run_control(enum np2audio86_async_mode mode,
                       struct np2audio86_async_test_control *control,
                       struct np2audio86_async_test_observer *observer,
                       struct np2audio86_async_result *result)
{
    struct run_call call = {mode, control, observer, result, -1};
    pthread_t thread;
    if (control->gate == NP2_AUDIO86_TEST_GATE_NONE) {
        return np2audio86_async_run_with_test_control(
            mode, control, observer, result);
    }
    assert(pthread_create(&thread, NULL, run_call_thread, &call) == 0);
    while (!atomic_load_explicit(&control->gate_reached,
                                 memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&control->gate_release, true, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    return call.status;
}

static void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

static const char *terminal_name(uint32_t terminal)
{
    switch (terminal) {
    case NP2_AUDIO86_TEST_TERMINAL_NOT_STARTED: return "NOT_STARTED";
    case NP2_AUDIO86_TEST_TERMINAL_COMPLETED: return "COMPLETED";
    case NP2_AUDIO86_TEST_TERMINAL_ABORTED: return "ABORTED";
    }
    return "UNKNOWN";
}

static int run_pv(unsigned case_id)
{
    return np2audio86_async_test_prevalidate(case_id);
}

static int run_full(const char *id, enum np2audio86_async_mode mode,
                    uint32_t yield_flags)
{
    struct np2audio86_async_test_control control;
    struct np2audio86_async_test_observer observer;
    struct np2audio86_async_result result;
    int status;
    np2audio86_async_test_control_init(&control);
    control.yield_flags = yield_flags;
    status = run_control(mode, &control, &observer, &result);
    printf("AUDIO86_HARDENING_CASE id=%s pass=%u pcm_crc32=%08" PRIx32
           " pcm_sha256=", id,
           status == 0 && result.passed &&
               np2audio86_fixture_matches_golden(&result.oracle),
           result.oracle.pcm_crc32);
    print_sha256(result.oracle.pcm_sha256);
    printf(" producer_reaped=%u worker_reaped=%u\n",
           observer.producer_reaped, observer.worker_reaped);
    return status == 0 && result.passed &&
                   np2audio86_fixture_matches_golden(&result.oracle) &&
                   observer.producer_reaped && observer.worker_reaped
               ? 0
               : 1;
}

static int run_fault(const char *id, enum np2audio86_async_test_fault fault,
                     enum np2audio86_async_mode mode, uint32_t gate,
                     uint32_t target_event)
{
    struct np2audio86_async_test_control control;
    struct np2audio86_async_test_observer observer;
    struct np2audio86_async_result result;
    int status;
    np2audio86_async_test_control_init(&control);
    control.fault = fault;
    control.gate = gate;
    control.target_event = target_event;
    status = run_control(mode, &control, &observer, &result);
    printf("AUDIO86_FAULT_CASE id=%s injected=%u detected=%u first_error=%s"
           " producer_created=%u worker_created=%u producer_terminal=%s"
           " worker_terminal=%s producer_reaped=%u worker_reaped=%u"
           " later_error_attempted=%u"
           " workload_success=%u peer_unblocked=%u event_residual=%u"
           " byte_residual=%u pass=%u\n",
           id, observer.injected, observer.detected,
           np2audio86_async_error_name(observer.observed_error),
           observer.producer_created, observer.worker_created,
           terminal_name(observer.producer_terminal),
           terminal_name(observer.worker_terminal), observer.producer_reaped,
           observer.worker_reaped, observer.later_error_attempted,
           observer.workload_success,
           observer.peer_unblocked, observer.event_residual,
           observer.byte_residual,
           status != 0 && observer.injected && observer.detected &&
               !observer.workload_success &&
               (!observer.producer_created || observer.producer_reaped) &&
               (!observer.worker_created || observer.worker_reaped) &&
               (strcmp(id, "F22") != 0 || observer.later_error_attempted));
    return status != 0 && observer.injected && observer.detected &&
                   !observer.workload_success &&
                   (!observer.producer_created || observer.producer_reaped) &&
                   (!observer.worker_created || observer.worker_reaped) &&
                   (strcmp(id, "F22") != 0 || observer.later_error_attempted)
               ? 0
               : 1;
}

enum {
    BYTE_STRESS_CHUNK = 32768,
    BYTE_STRESS_CHUNKS = 256,
    EVENT_STRESS_EVENTS = 8192,
};

struct byte_stress_context {
    struct np2audio86_byte_ring ring;
    atomic_bool failed;
    atomic_bool full_seen;
    atomic_bool copy_reached;
    atomic_bool copy_release;
};

static void byte_stress_fill(uint8_t *bytes, uint32_t sequence)
{
    size_t i;
    for (i = 0U; i < BYTE_STRESS_CHUNK; ++i) {
        bytes[i] = (uint8_t)((sequence * 17U + i * 29U + 7U) & 0xffU);
    }
}

static void *byte_stress_producer(void *opaque)
{
    struct byte_stress_context *context = opaque;
    uint8_t bytes[BYTE_STRESS_CHUNK];
    uint32_t sequence;
    for (sequence = NP2_AUDIO86_ASYNC_BYTE_CAPACITY / BYTE_STRESS_CHUNK;
         sequence < BYTE_STRESS_CHUNKS &&
         !atomic_load_explicit(&context->failed, memory_order_acquire);
         ++sequence) {
        int status;
        byte_stress_fill(bytes, sequence);
        do {
            status = np2audio86_byte_ring_push(&context->ring, bytes,
                                               sizeof(bytes));
            if (status == NP2_AUDIO86_TRANSPORT_FULL) {
                atomic_store_explicit(&context->full_seen, true,
                                      memory_order_release);
                sched_yield();
            } else if (status != NP2_AUDIO86_TRANSPORT_OK) {
                atomic_store_explicit(&context->failed, true,
                                      memory_order_release);
                return NULL;
            }
        } while (status != NP2_AUDIO86_TRANSPORT_OK &&
                 !atomic_load_explicit(&context->failed, memory_order_acquire));
    }
    return NULL;
}

static void *byte_stress_consumer(void *opaque)
{
    struct byte_stress_context *context = opaque;
    uint8_t expected[BYTE_STRESS_CHUNK];
    uint8_t actual[BYTE_STRESS_CHUNK];
    uint32_t sequence;
    for (sequence = 0U; sequence < BYTE_STRESS_CHUNKS; ++sequence) {
        int status;
        if (sequence == 0U) {
            do {
                status = np2audio86_async_test_byte_copy(
                    &context->ring, actual, sizeof(actual));
                if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
                    sched_yield();
                } else if (status != NP2_AUDIO86_TRANSPORT_OK) {
                    atomic_store_explicit(&context->failed, true,
                                          memory_order_release);
                    return NULL;
                }
            } while (status != NP2_AUDIO86_TRANSPORT_OK &&
                     !atomic_load_explicit(&context->failed,
                                           memory_order_acquire));
            atomic_store_explicit(&context->copy_reached, true,
                                  memory_order_release);
            while (!atomic_load_explicit(&context->full_seen,
                                         memory_order_acquire) ||
                   !atomic_load_explicit(&context->copy_release,
                                         memory_order_acquire)) {
                sched_yield();
            }
            status = np2audio86_async_test_byte_consume(
                &context->ring, sizeof(actual));
            if (status != NP2_AUDIO86_TRANSPORT_OK) {
                atomic_store_explicit(&context->failed, true,
                                      memory_order_release);
                return NULL;
            }
        } else {
            do {
                status = np2audio86_byte_ring_pop(&context->ring, actual,
                                                  sizeof(actual));
                if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
                    sched_yield();
                } else if (status != NP2_AUDIO86_TRANSPORT_OK) {
                    atomic_store_explicit(&context->failed, true,
                                          memory_order_release);
                    return NULL;
                }
            } while (status != NP2_AUDIO86_TRANSPORT_OK &&
                     !atomic_load_explicit(&context->failed,
                                           memory_order_acquire));
        }
        byte_stress_fill(expected, sequence);
        if (memcmp(actual, expected, sizeof(actual)) != 0) {
            atomic_store_explicit(&context->failed, true, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static int ring_byte_stress(void)
{
    struct byte_stress_context context;
    pthread_t producer;
    pthread_t consumer;
    uint8_t bytes[BYTE_STRESS_CHUNK];
    uint32_t sequence;
    int producer_started;
    int consumer_started;
    np2audio86_byte_ring_init(&context.ring);
    atomic_store_explicit(&context.ring.head, UINT32_MAX - 16383U,
                          memory_order_relaxed);
    atomic_store_explicit(&context.ring.tail, UINT32_MAX - 16383U,
                          memory_order_relaxed);
    atomic_init(&context.failed, false);
    atomic_init(&context.full_seen, false);
    atomic_init(&context.copy_reached, false);
    atomic_init(&context.copy_release, false);
    for (sequence = 0U;
         sequence < NP2_AUDIO86_ASYNC_BYTE_CAPACITY / BYTE_STRESS_CHUNK;
         ++sequence) {
        byte_stress_fill(bytes, sequence);
        if (np2audio86_byte_ring_push(&context.ring, bytes, sizeof(bytes)) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            return 1;
        }
    }
    producer_started = pthread_create(&producer, NULL, byte_stress_producer,
                                      &context) == 0;
    consumer_started = pthread_create(&consumer, NULL, byte_stress_consumer,
                                      &context) == 0;
    if (!producer_started || !consumer_started) {
        atomic_store_explicit(&context.failed, true, memory_order_release);
    }
    while (consumer_started &&
           !atomic_load_explicit(&context.copy_reached, memory_order_acquire) &&
           !atomic_load_explicit(&context.failed, memory_order_acquire)) {
        sched_yield();
    }
    if (consumer_started) {
        atomic_store_explicit(&context.copy_release, true, memory_order_release);
    }
    if (producer_started) {
        (void)pthread_join(producer, NULL);
    }
    if (consumer_started) {
        (void)pthread_join(consumer, NULL);
    }
    return producer_started && consumer_started &&
                   !atomic_load_explicit(&context.failed, memory_order_acquire) &&
                   atomic_load_explicit(&context.full_seen, memory_order_acquire) &&
                   atomic_load_explicit(&context.copy_reached, memory_order_acquire) &&
                   np2audio86_byte_ring_occupancy(&context.ring) == 0U
               ? 0
               : 1;
}

struct event_stress_context {
    struct np2audio86_event_ring ring;
    atomic_bool failed;
    atomic_bool full_seen;
};

static struct np2audio86_event event_stress_make(uint32_t sequence)
{
    struct np2audio86_event event;
    event.frame_timestamp = (uint64_t)sequence * 240U;
    event.sequence = sequence;
    event.opcode = (sequence & 1U) == 0U ? NP2_AUDIO86_EVENT_FM_KEY
                                         : NP2_AUDIO86_EVENT_PSG_REGISTER;
    event.payload = sequence ^ 0x55aaU;
    return event;
}

static void *event_stress_producer(void *opaque)
{
    struct event_stress_context *context = opaque;
    uint32_t sequence;
    for (sequence = NP2_AUDIO86_ASYNC_EVENT_CAPACITY;
         sequence < EVENT_STRESS_EVENTS &&
         !atomic_load_explicit(&context->failed, memory_order_acquire);
         ++sequence) {
        const struct np2audio86_event event = event_stress_make(sequence);
        int status;
        do {
            status = np2audio86_event_ring_enqueue(&context->ring, &event);
            if (status == NP2_AUDIO86_TRANSPORT_FULL) {
                atomic_store_explicit(&context->full_seen, true,
                                      memory_order_release);
                sched_yield();
            } else if (status != NP2_AUDIO86_TRANSPORT_OK) {
                atomic_store_explicit(&context->failed, true,
                                      memory_order_release);
                return NULL;
            }
        } while (status != NP2_AUDIO86_TRANSPORT_OK &&
                 !atomic_load_explicit(&context->failed, memory_order_acquire));
    }
    return NULL;
}

static void *event_stress_consumer(void *opaque)
{
    struct event_stress_context *context = opaque;
    uint32_t sequence;
    for (sequence = 0U; sequence < EVENT_STRESS_EVENTS; ++sequence) {
        struct np2audio86_event actual;
        const struct np2audio86_event expected = event_stress_make(sequence);
        int status;
        do {
            status = np2audio86_event_ring_dequeue(&context->ring, &actual);
            if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
                sched_yield();
            } else if (status != NP2_AUDIO86_TRANSPORT_OK) {
                atomic_store_explicit(&context->failed, true,
                                      memory_order_release);
                return NULL;
            }
        } while (status != NP2_AUDIO86_TRANSPORT_OK &&
                 !atomic_load_explicit(&context->failed, memory_order_acquire));
        if (memcmp(&actual, &expected, sizeof(actual)) != 0) {
            atomic_store_explicit(&context->failed, true, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static int ring_event_stress(void)
{
    struct event_stress_context context;
    pthread_t producer;
    pthread_t consumer;
    uint32_t sequence;
    int producer_started;
    int consumer_started;
    np2audio86_event_ring_init(&context.ring);
    atomic_store_explicit(&context.ring.head, UINT32_MAX - 63U,
                          memory_order_relaxed);
    atomic_store_explicit(&context.ring.tail, UINT32_MAX - 63U,
                          memory_order_relaxed);
    atomic_init(&context.failed, false);
    atomic_init(&context.full_seen, false);
    for (sequence = 0U; sequence < NP2_AUDIO86_ASYNC_EVENT_CAPACITY;
         ++sequence) {
        const struct np2audio86_event event = event_stress_make(sequence);
        if (np2audio86_event_ring_enqueue(&context.ring, &event) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            return 1;
        }
    }
    producer_started = pthread_create(&producer, NULL, event_stress_producer,
                                      &context) == 0;
    consumer_started = pthread_create(&consumer, NULL, event_stress_consumer,
                                      &context) == 0;
    if (!producer_started || !consumer_started) {
        atomic_store_explicit(&context.failed, true, memory_order_release);
    }
    if (producer_started) {
        (void)pthread_join(producer, NULL);
    }
    if (consumer_started) {
        (void)pthread_join(consumer, NULL);
    }
    return producer_started && consumer_started &&
                   !atomic_load_explicit(&context.failed, memory_order_acquire) &&
                   atomic_load_explicit(&context.full_seen, memory_order_acquire) &&
                   np2audio86_event_ring_occupancy(&context.ring) == 0U
               ? 0
               : 1;
}

static int api_misuse(void)
{
    struct np2audio86_event_ring events;
    struct np2audio86_byte_ring bytes;
    struct np2audio86_event event;
    uint8_t data[4] = {0};
    uint8_t full_data[NP2_AUDIO86_ASYNC_BYTE_CAPACITY] = {0};
    np2audio86_event_ring_init(NULL);
    np2audio86_byte_ring_init(NULL);
    np2audio86_event_ring_init(&events);
    np2audio86_byte_ring_init(&bytes);
    memset(&event, 0, sizeof(event));
    if (np2audio86_event_ring_enqueue(NULL, &event) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_event_ring_enqueue(&events, NULL) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_byte_ring_push(NULL, data, sizeof(data)) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_byte_ring_push(&bytes, NULL, sizeof(data)) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_byte_ring_push(&bytes, data,
                                  NP2_AUDIO86_ASYNC_BYTE_CAPACITY + 1U) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_byte_ring_pop(&bytes, data,
                                 NP2_AUDIO86_ASYNC_BYTE_CAPACITY + 1U) !=
            NP2_AUDIO86_TRANSPORT_ARGUMENT ||
        np2audio86_byte_ring_pop(&bytes, data, sizeof(data)) !=
            NP2_AUDIO86_TRANSPORT_EMPTY ||
        np2audio86_event_ring_occupancy(NULL) != 0U ||
        np2audio86_byte_ring_occupancy(NULL) != 0U) {
        return 1;
    }
    if (np2audio86_byte_ring_push(&bytes, NULL, 0U) !=
        NP2_AUDIO86_TRANSPORT_OK) {
        return 1;
    }
    for (size_t i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i) {
        if (np2audio86_event_ring_enqueue(&events, &event) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            return 1;
        }
    }
    if (np2audio86_event_ring_enqueue(&events, &event) !=
        NP2_AUDIO86_TRANSPORT_FULL) {
        return 1;
    }
    np2audio86_event_ring_init(&events);
    if (np2audio86_byte_ring_push(&bytes, full_data, sizeof(full_data)) !=
            NP2_AUDIO86_TRANSPORT_OK ||
        np2audio86_byte_ring_push(&bytes, data, sizeof(data)) !=
            NP2_AUDIO86_TRANSPORT_FULL) {
        return 1;
    }
    np2audio86_byte_ring_init(&bytes);
    atomic_store_explicit(&events.head, NP2_AUDIO86_ASYNC_EVENT_CAPACITY + 1U,
                          memory_order_relaxed);
    atomic_store_explicit(&events.tail, 0U, memory_order_relaxed);
    atomic_store_explicit(&bytes.head, NP2_AUDIO86_ASYNC_BYTE_CAPACITY + 1U,
                          memory_order_relaxed);
    atomic_store_explicit(&bytes.tail, 0U, memory_order_relaxed);
    return np2audio86_event_ring_enqueue(&events, &event) ==
                       NP2_AUDIO86_TRANSPORT_INVARIANT &&
                   np2audio86_event_ring_dequeue(&events, &event) ==
                       NP2_AUDIO86_TRANSPORT_INVARIANT &&
                   np2audio86_byte_ring_push(&bytes, data, sizeof(data)) ==
                       NP2_AUDIO86_TRANSPORT_INVARIANT &&
                   np2audio86_byte_ring_pop(&bytes, data, sizeof(data)) ==
                       NP2_AUDIO86_TRANSPORT_INVARIANT
               ? 0
               : 1;
}

static int run_soak(void)
{
    unsigned repetition;
    uint32_t pcm_crc32 = 0U;
    uint8_t pcm_sha256[NP2_SHA256_DIGEST_SIZE] = {0};
    for (repetition = 0U; repetition < 25U; ++repetition) {
        struct np2audio86_async_test_control control;
        struct np2audio86_async_test_observer observer;
        struct np2audio86_async_result result;
        np2audio86_async_test_control_init(&control);
        if (run_control(NP2_AUDIO86_ASYNC_DETERMINISTIC_ALTERNATING,
                        &control, &observer, &result) != 0 ||
            !result.passed || !observer.producer_reaped ||
            !observer.worker_reaped ||
            !np2audio86_fixture_matches_golden(&result.oracle)) {
            return 1;
        }
        pcm_crc32 = result.oracle.pcm_crc32;
        memcpy(pcm_sha256, result.oracle.pcm_sha256, sizeof(pcm_sha256));
    }
    printf("AUDIO86_HARDENING_SOAK repetitions=25 pass=1 pcm_crc32=%08" PRIx32
           " pcm_sha256=", pcm_crc32);
    print_sha256(pcm_sha256);
    putchar('\n');
    return 0;
}

static int run_case(const char *id)
{
    if (strcmp(id, "PV01") == 0) return run_pv(1U);
    if (strcmp(id, "PV02") == 0) return run_pv(2U);
    if (strcmp(id, "PV03") == 0) return run_pv(3U);
    if (strcmp(id, "PV04") == 0) return run_pv(4U);
    if (strcmp(id, "PV05") == 0) return run_pv(5U);
    if (strcmp(id, "N01") == 0) {
        return run_full(id, NP2_AUDIO86_ASYNC_PRODUCER_FAST_WORKER_YIELD,
                        NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_PUBLICATION |
                            NP2_AUDIO86_TEST_YIELD_AFTER_EVENT_PUBLICATION |
                            NP2_AUDIO86_TEST_YIELD_BEFORE_WATERMARK |
                            NP2_AUDIO86_TEST_YIELD_AFTER_WATERMARK);
    }
    if (strcmp(id, "N02") == 0) {
        return run_full(id, NP2_AUDIO86_ASYNC_PRODUCER_YIELD_WORKER_FAST,
                        NP2_AUDIO86_TEST_YIELD_BEFORE_EVENT_TAIL |
                            NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_COPY |
                            NP2_AUDIO86_TEST_YIELD_WATERMARK_WAIT);
    }
    if (strcmp(id, "R01") == 0) return ring_byte_stress();
    if (strcmp(id, "R02") == 0) return ring_event_stress();
    if (strcmp(id, "R03") == 0) return api_misuse();
    if (strcmp(id, "F01") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DUPLICATE_SEQUENCE, 0, 0, 7U);
    if (strcmp(id, "F02") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_SKIPPED_SEQUENCE, 0, 0, 7U);
    if (strcmp(id, "F03") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_TIMESTAMP_BEHIND, 0, 0, 8U);
    if (strcmp(id, "F04") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_TIMESTAMP_END, 0, 0, 332U);
    if (strcmp(id, "F05") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_INVALID_OPCODE, 0, 0, 0U);
    if (strcmp(id, "F06") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_RESERVED_OPCODE, 0, 0, 0U);
    if (strcmp(id, "F07") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DATA_ZERO, 0, 0, 6U);
    if (strcmp(id, "F08") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DATA_UNALIGNED, 0, 0, 6U);
    if (strcmp(id, "F09") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DATA_OVERSIZE, 0, 0, 6U);
    if (strcmp(id, "F10") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DATA_UNAVAILABLE, 0, 0, 6U);
    if (strcmp(id, "F11") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_CUT_BEFORE_BYTES, 0, NP2_AUDIO86_TEST_GATE_BEFORE_WATERMARK, 6U);
    if (strcmp(id, "F12") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_BYTES, 0, NP2_AUDIO86_TEST_GATE_AFTER_BYTE_PUBLICATION, 6U);
    if (strcmp(id, "F13") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_EVENT, 0, NP2_AUDIO86_TEST_GATE_AFTER_EVENT_PUBLICATION, 6U);
    if (strcmp(id, "F14") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_WATERMARK, 0, NP2_AUDIO86_TEST_GATE_AFTER_WATERMARK, 0U);
    if (strcmp(id, "F15") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WATERMARK_REGRESSION, 0, 0, 1U);
    if (strcmp(id, "F16") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WATERMARK_OVER_END, 0, 0, 0U);
    if (strcmp(id, "F17") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_DONE_EARLY, 0, 0, 0U);
    if (strcmp(id, "F18") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WITHHOLD_FINAL, 0, 0, 332U);
    if (strcmp(id, "F19") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WATERMARK_PAST_EVENT, 0, 0, 7U);
    if (strcmp(id, "F20") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_INCOMPLETE_GROUP, 0, 0, 6U);
    if (strcmp(id, "F21") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WORKER, NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE, NP2_AUDIO86_TEST_GATE_PRODUCER_BYTE_FULL, 12U);
    if (strcmp(id, "F22") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY, NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE, 0, UINT32_MAX);
    if (strcmp(id, "F23") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_PRODUCER_CREATE, 0, 0, UINT32_MAX);
    if (strcmp(id, "F24") == 0) return run_fault(id, NP2_AUDIO86_TEST_FAULT_WORKER_CREATE, NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE, 0, UINT32_MAX);
    if (strcmp(id, "REC01") == 0) {
        return run_full(id, NP2_AUDIO86_ASYNC_DETERMINISTIC_ALTERNATING, 0U);
    }
    if (strcmp(id, "SOAK25") == 0) return run_soak();
    return 2;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--case") != 0) {
        fprintf(stderr, "usage: %s --case ID\n", argv[0]);
        return 2;
    }
    return run_case(argv[2]);
}
