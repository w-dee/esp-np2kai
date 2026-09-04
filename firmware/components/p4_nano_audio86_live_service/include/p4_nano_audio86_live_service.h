#ifndef P4_NANO_AUDIO86_LIVE_SERVICE_H
#define P4_NANO_AUDIO86_LIVE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <atomic>
#define P4_NANO_AUDIO86_LIVE_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define P4_NANO_AUDIO86_LIVE_ATOMIC(type) _Atomic type
#endif

#include "np2audio86_core.h"
#include "np2audio86_guest_adapter.h"
#include "np2audio86_runtime_transport.h"
#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One state word is authoritative.  In particular, no combination of
 * independent started/stopped/failed booleans can describe the lifecycle. */
enum p4_nano_audio86_live_state {
    P4_NANO_AUDIO86_LIVE_UNINITIALIZED = 0,
    P4_NANO_AUDIO86_LIVE_READY,
    P4_NANO_AUDIO86_LIVE_STARTING,
    P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED,
    P4_NANO_AUDIO86_LIVE_RUNNING,
    P4_NANO_AUDIO86_LIVE_STOP_REQUESTED,
    P4_NANO_AUDIO86_LIVE_DRAINING,
    P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT,
    P4_NANO_AUDIO86_LIVE_FAILING,
    P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT,
    P4_NANO_AUDIO86_LIVE_FAILED_NOT_QUIESCENT,
    P4_NANO_AUDIO86_LIVE_DESTROYED,
};

enum p4_nano_audio86_live_result {
    P4_NANO_AUDIO86_LIVE_OK = 0,
    P4_NANO_AUDIO86_LIVE_ALREADY,
    P4_NANO_AUDIO86_LIVE_TIMEOUT,
    P4_NANO_AUDIO86_LIVE_FAILED,
    P4_NANO_AUDIO86_LIVE_NOT_QUIESCENT,
    P4_NANO_AUDIO86_LIVE_ARGUMENT,
    P4_NANO_AUDIO86_LIVE_STATE_ERROR,
    P4_NANO_AUDIO86_LIVE_OWNER_ERROR,
};

enum p4_nano_audio86_live_failure_category {
    P4_NANO_AUDIO86_LIVE_FAILURE_NONE = 0,
    P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER,
    P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
    P4_NANO_AUDIO86_LIVE_FAILURE_RENDERER,
    P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
    P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT,
    P4_NANO_AUDIO86_LIVE_FAILURE_CLEANUP,
};

enum p4_nano_audio86_live_failure_origin {
    P4_NANO_AUDIO86_LIVE_ORIGIN_NONE = 0,
    P4_NANO_AUDIO86_LIVE_ORIGIN_INIT,
    P4_NANO_AUDIO86_LIVE_ORIGIN_START,
    P4_NANO_AUDIO86_LIVE_ORIGIN_ATTACH,
    P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
    P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
    P4_NANO_AUDIO86_LIVE_ORIGIN_RENDER,
    P4_NANO_AUDIO86_LIVE_ORIGIN_PCM_OUTPUT,
    P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH,
    P4_NANO_AUDIO86_LIVE_ORIGIN_ABORT_BARRIER,
};

enum p4_nano_audio86_live_cleanup {
    P4_NANO_AUDIO86_LIVE_CLEANUP_NOT_ATTEMPTED = 0,
    P4_NANO_AUDIO86_LIVE_CLEANUP_QUIESCENT,
    P4_NANO_AUDIO86_LIVE_CLEANUP_NOT_QUIESCENT,
};

struct p4_nano_audio86_live_status {
    enum p4_nano_audio86_live_state state;
    enum p4_nano_audio86_live_failure_category category;
    enum p4_nano_audio86_live_failure_origin origin;
    enum p4_nano_audio86_live_cleanup cleanup;
    uint32_t subcode;
    uint32_t first_error_sequence;
    uint64_t rendered_frames;
    uint64_t final_horizon;
    uint64_t accepted_frames;
    uint32_t producer_done;
    uint32_t guest_attached;
    uint32_t sink_reachable;
};

struct p4_nano_audio86_live_config {
    /* Borrowed, exclusively for the interval from successful start through
     * strong quiescence.  The service copies the callback table but never
     * destroys the caller's sink object or its opaque storage.
     *
     * Existing np2_pcm_sink semantics are the callback lifetime contract:
     * ACCEPTED from finish/abort means no new backend callback can begin and
     * all in-flight callbacks have left.  RETRY means the barrier is not yet
     * complete; FATAL means it cannot be proven. */
    const struct np2_pcm_sink *borrowed_sink;
};

/* Cross-core 64-bit observations use a 32-bit sequence lock.  This preserves
 * the established P4 rule that only naturally aligned 32-bit atomics cross
 * cores; the authoritative producer/worker counters themselves stay local. */
struct p4_nano_audio86_live_u64_snapshot {
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) sequence;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) low;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) high;
};

#define P4_NANO_AUDIO86_LIVE_WORKER_STACK_BYTES 6144U
#define P4_NANO_AUDIO86_LIVE_MAGIC UINT32_C(0x4138364c)

/* Caller-owned, fixed-size service storage.  Runtime memory is O(1): two
 * bounded guest rings, the fixed q240 ring, one renderer and one worker. */
struct p4_nano_audio86_live_service {
    uint32_t magic;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) state;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) stop_intent;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) first_error_latched;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) failure_category;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) failure_origin;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) failure_subcode;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) first_error_sequence;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) cleanup;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) guest_attached;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) sink_reachable;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) worker_safe;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) producer_closing;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) owner_finalizing;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) transaction_gate;
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) owner_valid;
#if defined(ESP_PLATFORM)
    P4_NANO_AUDIO86_LIVE_ATOMIC(uint32_t) worker_handle_ready;
#endif
    struct p4_nano_audio86_live_u64_snapshot final_horizon;
    struct p4_nano_audio86_live_u64_snapshot rendered_frames_published;
    struct p4_nano_audio86_live_u64_snapshot accepted_frames_published;

    struct np2audio86_event_ring events;
    struct np2audio86_byte_ring bytes;
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer_clock;
    struct np2audio86_runtime_consumer_clock consumer_clock;
    struct np2audio86_render_state render;
    struct np2opngen_pcm_ring pcm_ring;
    struct np2_pcm_output_controller output;
    const struct np2_pcm_sink *borrowed_sink;
    np2audio86_guest_sink_t guest_sink;

    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN];
    int32_t mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    uint8_t canonical[NP2_OPNGEN_PCM_RING_SLOT_BYTES];
    uint64_t rendered_frame;
    uint64_t worker_byte_offset;
    uint64_t next_sequence;
    uint64_t expected_sequence;
    uint32_t reset_ordinal;
    uint32_t reserved_events;
    uint32_t reserved_bytes;
    uint32_t transaction_kind;
    uint32_t transaction_generation;
    uint32_t active_generation;
    uint8_t transaction_active;
    uint8_t horizon_owned;
    uint8_t event_committed;
    uint8_t run_committed;
    uint8_t worker_joined;

#if defined(ESP_PLATFORM)
    TaskHandle_t owner_task;
    TaskHandle_t worker_task;
    StaticTask_t worker_tcb;
    StackType_t worker_stack[
        P4_NANO_AUDIO86_LIVE_WORKER_STACK_BYTES / sizeof(StackType_t)];
#else
    pthread_t owner_thread;
    pthread_t worker_thread;
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_cond;
#endif

#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_TESTING)
    void (*authorization_hook)(
        struct p4_nano_audio86_live_service *service, void *opaque);
    void *authorization_hook_opaque;
    void (*before_clean_terminal_hook)(
        struct p4_nano_audio86_live_service *service, void *opaque);
    void *before_clean_terminal_hook_opaque;
#endif
};

/* init: single controller, zeroed or previously DESTROYED caller storage.
 * Invalid arguments return ARGUMENT; live storage returns STATE_ERROR.  It is
 * nonblocking except for the process's first cold OPN initialization and is
 * allocation-free on the ESP target.  It retains the sink callback table but
 * calls no callback; the caller keeps sink/opaque storage alive through
 * destroy.  Later init calls are idempotent at the process-global table layer. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_init(
    struct p4_nano_audio86_live_service *service,
    const struct p4_nano_audio86_live_config *config);

/* start: single controller task, READY only; other states return STATE_ERROR.
 * It blocks while the borrowed sink reports RETRY, then returns after Core-0
 * acquires it or reaches a terminal state.  ESP uses an embedded static task
 * stack and allocates no service storage.  On OK the sink is exclusively
 * reachable by the worker until strong quiescence; a failed start reports the
 * cleanup outcome and may retain it only as FAILED_NOT_QUIESCENT requires. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_start(
    struct p4_nano_audio86_live_service *service);

/* attach/checkpoint: existing PC-98 guest owner task (Core 1 on P4) only and
 * allocation-free.  attach is STARTED_UNATTACHED-only; other states return
 * STATE_ERROR, and OK binds the singleton guest adapter publication seam.
 * checkpoint requires that same owner (otherwise OWNER_ERROR), is legal in
 * RUNNING/STOP_REQUESTED/FAILING, and may block on bounded transport/output
 * backpressure.  A normal RUNNING return leaves guest/sink reachability live;
 * clean finalization returns only after guest publication has detached, while
 * sink reachability remains with the worker until its terminal barrier. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_attach_guest(
    struct p4_nano_audio86_live_service *service);
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_owner_checkpoint(
    struct p4_nano_audio86_live_service *service);

/* request_stop: any normal task context, STARTED_UNATTACHED or RUNNING only,
 * asynchronous, nonblocking and allocation-free.  Other states return
 * STATE_ERROR; a repeated in-progress stop returns ALREADY.  It publishes only
 * intent and wakeups, never syncs guest time, emits RESET, or accepts an
 * externally selected horizon.  Guest/sink reachability is not released by
 * this call. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_request_stop(
    struct p4_nano_audio86_live_service *service);

/* Producer failure is owner-domain, nonblocking and allocation-free.  It is
 * legal in RUNNING/STOP_REQUESTED/DRAINING/FAILING; wrong-owner and other-state
 * calls return OWNER_ERROR and STATE_ERROR respectively.  The first fatal
 * wins, detaches guest publication before return, and retains category/origin/
 * subcode without calling np2runtime policy.  Sink reachability remains until
 * the worker's finish/abort barrier. */
enum p4_nano_audio86_live_result
p4_nano_audio86_live_service_report_producer_failure(
    struct p4_nano_audio86_live_service *service, uint32_t subcode);

void p4_nano_audio86_live_service_status(
    const struct p4_nano_audio86_live_service *service,
    struct p4_nano_audio86_live_status *status);

/* status is a nonblocking, allocation-free observer callable from normal task
 * context in every state.  It never transfers ownership, invokes callbacks or
 * mutates lifecycle state.
 *
 * join is legal in STOP_REQUESTED/DRAINING/FAILING and every terminal state;
 * other states return STATE_ERROR.  It waits/observes only, allocates nothing,
 * and never unregisters callbacks, creates a barrier, frees storage or turns a
 * caller timeout into a lifecycle fault.  Terminal FAILED_NOT_QUIESCENT leaves
 * the borrowed sink reachable and returns NOT_QUIESCENT. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_join(
    struct p4_nano_audio86_live_service *service, uint32_t timeout_ms,
    struct p4_nano_audio86_live_status *status);

/* destroy is single-controller, allocation-free on ESP and legal only from
 * READY, STOPPED_QUIESCENT or FAILED_QUIESCENT; other states return STATE_ERROR
 * (or NOT_QUIESCENT if an expected proof is absent).  It reclaims only
 * service-owned resources, clears all retained callback/opaque references and
 * never destroys caller-owned sink storage.  No service callback is reachable
 * after OK. */
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_destroy(
    struct p4_nano_audio86_live_service *service);

#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_TESTING)
void p4_nano_audio86_live_service_test_set_authorization_hook(
    struct p4_nano_audio86_live_service *service,
    void (*hook)(struct p4_nano_audio86_live_service *, void *), void *opaque);
void p4_nano_audio86_live_service_test_set_before_clean_terminal_hook(
    struct p4_nano_audio86_live_service *service,
    void (*hook)(struct p4_nano_audio86_live_service *, void *), void *opaque);
enum p4_nano_audio86_live_result p4_nano_audio86_live_service_test_fail(
    struct p4_nano_audio86_live_service *service,
    enum p4_nano_audio86_live_failure_category category,
    enum p4_nano_audio86_live_failure_origin origin, uint32_t subcode);
#endif

#ifdef __cplusplus
}
#endif

#undef P4_NANO_AUDIO86_LIVE_ATOMIC

#endif /* P4_NANO_AUDIO86_LIVE_SERVICE_H */
