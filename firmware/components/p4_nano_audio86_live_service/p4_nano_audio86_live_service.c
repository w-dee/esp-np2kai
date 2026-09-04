#include "p4_nano_audio86_live_service.h"
#include "p4_nano_audio86_live_service_fixture.h"

#include <limits.h>
#include <string.h>

#include "np2opngen_pcm_canonical.h"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <errno.h>
#include <time.h>
#endif

enum {
    LIVE_EVENT_OPNA_REGISTER = 0x100U,
    LIVE_EVENT_OPNA_CSM = 0x101U,
    LIVE_EVENT_PCM_CONTROL = 0x102U,
    LIVE_SUBCODE_THREAD_CREATE = 1U,
    LIVE_SUBCODE_SINK_START = 2U,
    LIVE_SUBCODE_GUEST_CONTRACT = 3U,
    LIVE_SUBCODE_HORIZON = 4U,
    LIVE_SUBCODE_EVENT = 5U,
    LIVE_SUBCODE_RENDER = 6U,
    LIVE_SUBCODE_PCM_RING = 7U,
    LIVE_SUBCODE_PCM_OUTPUT = 8U,
    LIVE_SUBCODE_FINISH = 9U,
    LIVE_SUBCODE_ABORT_BARRIER = 10U,
    LIVE_SUBCODE_5D3_TERMINAL = 11U,
    LIVE_TRANSACTION_GATE_OPEN = 0U,
    LIVE_TRANSACTION_GATE_ACTIVE = 1U,
    LIVE_TRANSACTION_GATE_CLOSED = 2U,
};

static void fixture_terminal_point(
    struct p4_nano_audio86_live_service *service,
    enum p4_nano_audio86_5d3_terminal_point point)
{
    if (service->fixture_observe_terminal_point != NULL)
        service->fixture_observe_terminal_point(service->fixture_opaque,
                                                (uint32_t)point);
}

static bool state_terminal(enum p4_nano_audio86_live_state state)
{
    return state == P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT ||
           state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT ||
           state == P4_NANO_AUDIO86_LIVE_FAILED_NOT_QUIESCENT;
}

static enum p4_nano_audio86_live_state service_state(
    const struct p4_nano_audio86_live_service *service)
{
    return (enum p4_nano_audio86_live_state)atomic_load_explicit(
        &service->state, memory_order_acquire);
}

static void snapshot_u64_publish(
    struct p4_nano_audio86_live_u64_snapshot *snapshot, uint64_t value)
{
    uint32_t sequence = atomic_load_explicit(&snapshot->sequence,
                                              memory_order_relaxed);
    atomic_store_explicit(&snapshot->sequence, sequence + 1U,
                          memory_order_release);
    atomic_store_explicit(&snapshot->low, (uint32_t)value,
                          memory_order_relaxed);
    atomic_store_explicit(&snapshot->high, (uint32_t)(value >> 32U),
                          memory_order_relaxed);
    atomic_store_explicit(&snapshot->sequence, sequence + 2U,
                          memory_order_release);
}

static uint64_t snapshot_u64_read(
    const struct p4_nano_audio86_live_u64_snapshot *snapshot)
{
    for (;;) {
        uint32_t before = atomic_load_explicit(&snapshot->sequence,
                                               memory_order_acquire);
        uint32_t low;
        uint32_t high;
        uint32_t after;
        if ((before & 1U) != 0U)
            continue;
        low = atomic_load_explicit(&snapshot->low, memory_order_relaxed);
        high = atomic_load_explicit(&snapshot->high, memory_order_relaxed);
        after = atomic_load_explicit(&snapshot->sequence,
                                     memory_order_acquire);
        if (before == after)
            return ((uint64_t)high << 32U) | low;
    }
}

static void wake_waiters(struct p4_nano_audio86_live_service *service)
{
#if defined(ESP_PLATFORM)
    if (atomic_load_explicit(&service->worker_handle_ready,
                             memory_order_acquire) != 0U)
        xTaskNotifyGiveIndexed(service->worker_task, 0U);
    if (atomic_load_explicit(&service->owner_valid,
                             memory_order_acquire) != 0U)
        xTaskNotifyGiveIndexed(service->owner_task, 1U);
#else
    (void)pthread_cond_broadcast(&service->wait_cond);
#endif
}

static void worker_wait(struct p4_nano_audio86_live_service *service,
                        uint32_t timeout_ms)
{
#if defined(ESP_PLATFORM)
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0U)
        ticks = 1U;
    (void)ulTaskNotifyTakeIndexed(0U, pdTRUE, ticks);
#else
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    deadline.tv_sec += (time_t)(timeout_ms / 1000U) +
                       deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    (void)pthread_mutex_lock(&service->wait_mutex);
    (void)pthread_cond_timedwait(&service->wait_cond, &service->wait_mutex,
                                 &deadline);
    (void)pthread_mutex_unlock(&service->wait_mutex);
#endif
}

static void owner_wait(struct p4_nano_audio86_live_service *service)
{
#if defined(ESP_PLATFORM)
    TickType_t ticks = pdMS_TO_TICKS(1U);
    if (ticks == 0U)
        ticks = 1U;
    (void)ulTaskNotifyTakeIndexed(1U, pdTRUE, ticks);
#else
    worker_wait(service, 1U);
#endif
}

static bool owner_matches(const struct p4_nano_audio86_live_service *service)
{
    if (atomic_load_explicit(&service->owner_valid,
                             memory_order_acquire) == 0U)
        return false;
#if defined(ESP_PLATFORM)
    return service->owner_task == xTaskGetCurrentTaskHandle();
#else
    return pthread_equal(service->owner_thread, pthread_self()) != 0;
#endif
}

static bool first_fatal(
    struct p4_nano_audio86_live_service *service,
    enum p4_nano_audio86_live_failure_category category,
    enum p4_nano_audio86_live_failure_origin origin, uint32_t subcode)
{
    uint32_t expected_error = 0U;
    uint32_t expected_state;
    enum p4_nano_audio86_live_state state;

    if (service == NULL || category == P4_NANO_AUDIO86_LIVE_FAILURE_NONE)
        return false;
    for (;;) {
        state = service_state(service);
        if (state_terminal(state) ||
            state == P4_NANO_AUDIO86_LIVE_DESTROYED ||
            state == P4_NANO_AUDIO86_LIVE_UNINITIALIZED ||
            state == P4_NANO_AUDIO86_LIVE_FAILING)
            return false;
        expected_state = (uint32_t)state;
        if (atomic_compare_exchange_weak_explicit(
                &service->state, &expected_state,
                P4_NANO_AUDIO86_LIVE_FAILING, memory_order_acq_rel,
                memory_order_acquire))
            break;
    }

    /* The state CAS serializes fatal with clean-terminal publication.  This
     * CAS remains the immutable first-error latch/publication barrier. */
    atomic_store_explicit(&service->failure_category, (uint32_t)category,
                          memory_order_relaxed);
    atomic_store_explicit(&service->failure_origin, (uint32_t)origin,
                          memory_order_relaxed);
    atomic_store_explicit(&service->failure_subcode, subcode,
                          memory_order_relaxed);
    atomic_store_explicit(&service->first_error_sequence, 1U,
                          memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(
            &service->first_error_latched, &expected_error, 1U,
            memory_order_acq_rel, memory_order_acquire))
        return false;
    atomic_store_explicit(&service->stop_intent, 1U, memory_order_release);
    atomic_store_explicit(&service->producer_closing, 1U,
                          memory_order_release);
    atomic_store_explicit(&service->transaction_gate,
                          LIVE_TRANSACTION_GATE_CLOSED,
                          memory_order_release);
    (void)np2audio86_runtime_first_error_publish(&service->control,
                                                  subcode == 0U ? 1U : subcode);
    np2audio86_runtime_stop_publish(&service->control);
    wake_waiters(service);
    return true;
}

static void token_set(struct p4_nano_audio86_live_service *service,
                      np2audio86_guest_transaction_t *token, uint32_t kind)
{
    memset(token, 0, sizeof(*token));
    token->opaque[0] = (uintptr_t)service;
    token->opaque[1] = service->active_generation;
    token->opaque[2] = kind;
    token->opaque[3] = 1U;
}

static bool token_matches(
    const struct p4_nano_audio86_live_service *service,
    const np2audio86_guest_transaction_t *token, uint32_t kind)
{
    return service->transaction_active != 0U && token != NULL &&
           token->opaque[0] == (uintptr_t)service &&
           token->opaque[1] == service->active_generation &&
           token->opaque[2] == kind && token->opaque[3] == 1U &&
           service->transaction_kind == kind;
}

static bool producer_terminated(
    const struct p4_nano_audio86_live_service *service)
{
    return atomic_load_explicit(&service->stop_intent,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&service->first_error_latched,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&service->producer_closing,
                                memory_order_acquire) != 0U ||
           service_state(service) != P4_NANO_AUDIO86_LIVE_RUNNING;
}

static int wait_for_capacity(struct p4_nano_audio86_live_service *service,
                             size_t bytes)
{
    for (;;) {
        bool event_space;
        bool byte_space;
        bool horizon_empty;
        if (producer_terminated(service))
            return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
        event_space =
            np2audio86_event_ring_occupancy(&service->events) +
                service->reserved_events <
            NP2_AUDIO86_ASYNC_EVENT_CAPACITY;
        byte_space =
            (uint64_t)np2audio86_byte_ring_occupancy(&service->bytes) +
                service->reserved_bytes + bytes <=
            NP2_AUDIO86_ASYNC_BYTE_CAPACITY;
        horizon_empty =
            !np2audio86_runtime_horizon_pending(&service->control) &&
            service->horizon_owned == 0U;
        if (event_space && byte_space && horizon_empty)
            return NP2AUDIO86_GUEST_TRANSACTION_OK;
        wake_waiters(service);
        owner_wait(service);
    }
}

static int guest_reserve_checked(void *opaque, uint32_t kind, size_t bytes,
                                 np2audio86_guest_transaction_t *token)
{
    struct p4_nano_audio86_live_service *service = opaque;
    uint32_t gate = LIVE_TRANSACTION_GATE_OPEN;
    int result;
    if (service == NULL || token == NULL ||
        service->transaction_active != 0U ||
        (kind != NP2AUDIO86_GUEST_TRANSACTION_EVENT &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_RESET) ||
        (kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN ? bytes != 1U
                                                       : bytes != 0U)) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    result = wait_for_capacity(service, bytes);
    if (result != NP2AUDIO86_GUEST_TRANSACTION_OK)
        return result;
    /* The gate CAS and the following lifecycle check form the
     * transaction/STOP linearization boundary.  STOP closes the gate.  If
     * authorization wins, STOP may close it behind this transaction but
     * cannot revoke the already-authorized semantic unit. */
    if (!atomic_compare_exchange_strong_explicit(
            &service->transaction_gate, &gate,
            LIVE_TRANSACTION_GATE_ACTIVE, memory_order_acq_rel,
            memory_order_acquire))
        return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
    if (service_state(service) != P4_NANO_AUDIO86_LIVE_RUNNING) {
        atomic_store_explicit(&service->transaction_gate,
                              LIVE_TRANSACTION_GATE_CLOSED,
                              memory_order_release);
        return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
    }
    service->reserved_events = 1U;
    service->reserved_bytes = (uint32_t)bytes;
    service->transaction_kind = kind;
    service->transaction_active = 1U;
    service->horizon_owned = 1U;
    service->event_committed = 0U;
    service->run_committed = 0U;
    service->active_generation = ++service->transaction_generation;
    token_set(service, token, kind);
#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_TESTING)
    if (service->authorization_hook != NULL)
        service->authorization_hook(service,
                                    service->authorization_hook_opaque);
#endif
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static int guest_extend_checked(void *opaque,
                                np2audio86_guest_transaction_t *token,
                                size_t bytes)
{
    struct p4_nano_audio86_live_service *service = opaque;
    if (service == NULL ||
        !token_matches(service, token,
                       NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        bytes != 1U || service->run_committed != 0U) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    /* Stop after authorization cannot split a DATA_RUN.  Capacity remains
     * bounded and the worker is kept awake until the reserved run closes. */
    while ((uint64_t)np2audio86_byte_ring_occupancy(&service->bytes) +
               service->reserved_bytes >=
           NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        if (atomic_load_explicit(&service->first_error_latched,
                                 memory_order_acquire) != 0U)
            return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
        wake_waiters(service);
        owner_wait(service);
    }
    ++service->reserved_bytes;
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static void guest_commit_pcm_byte(void *opaque,
                                  np2audio86_guest_transaction_t *token,
                                  uint64_t frame, uint64_t sequence,
                                  uint8_t value)
{
    struct p4_nano_audio86_live_service *service = opaque;
    (void)frame;
    if (service == NULL ||
        !token_matches(service, token,
                       NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        service->reserved_bytes == 0U || sequence != service->next_sequence ||
        np2audio86_byte_ring_push(&service->bytes, &value, 1U) !=
            NP2_AUDIO86_TRANSPORT_OK) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return;
    }
    --service->reserved_bytes;
    wake_waiters(service);
}

static void guest_commit_data_run(void *opaque,
                                  np2audio86_guest_transaction_t *token,
                                  const np2audio86_guest_data_run_t *run)
{
    struct p4_nano_audio86_live_service *service = opaque;
    struct np2audio86_event event;
    if (service == NULL || run == NULL ||
        !token_matches(service, token,
                       NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        service->reserved_events != 1U || service->reserved_bytes != 0U ||
        run->sequence != service->next_sequence || run->count == 0U) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return;
    }
    event.frame_timestamp = run->frame_timestamp;
    event.sequence = run->sequence;
    event.opcode = NP2_AUDIO86_EVENT_PCM86_DATA_RUN;
    event.payload = run->count;
    if (np2audio86_event_ring_enqueue(&service->events, &event) !=
        NP2_AUDIO86_TRANSPORT_OK) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                          LIVE_SUBCODE_EVENT);
        return;
    }
    service->reserved_events = 0U;
    service->run_committed = 1U;
    ++service->next_sequence;
    wake_waiters(service);
}

static void guest_commit_event(void *opaque,
                               np2audio86_guest_transaction_t *token,
                               const np2audio86_guest_event_t *guest)
{
    struct p4_nano_audio86_live_service *service = opaque;
    struct np2audio86_event event;
    bool reset;
    int result;
    if (service == NULL || guest == NULL ||
        (!token_matches(service, token,
                        NP2AUDIO86_GUEST_TRANSACTION_EVENT) &&
         !token_matches(service, token,
                        NP2AUDIO86_GUEST_TRANSACTION_RESET)) ||
        service->reserved_events != 1U ||
        guest->sequence != service->next_sequence) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return;
    }
    event.frame_timestamp = guest->frame_timestamp;
    event.sequence = guest->sequence;
    event.payload = guest->payload;
    if (guest->opcode == NP2AUDIO86_TRACE_OPNA_REGISTER)
        event.opcode = LIVE_EVENT_OPNA_REGISTER;
    else if (guest->opcode == NP2AUDIO86_TRACE_OPNA_CSM)
        event.opcode = LIVE_EVENT_OPNA_CSM;
    else if (guest->opcode == NP2AUDIO86_TRACE_PCM_CONTROL)
        event.opcode = LIVE_EVENT_PCM_CONTROL;
    else
        event.opcode = guest->opcode;
    reset = event.opcode == NP2_AUDIO86_EVENT_RESET_BARRIER;
    result = reset
                 ? np2audio86_reset_event_ring_enqueue(
                       &service->events, &event, &service->reset_ordinal)
                 : np2audio86_event_ring_enqueue(&service->events, &event);
    if (result != NP2_AUDIO86_TRANSPORT_OK) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                          LIVE_SUBCODE_EVENT);
        return;
    }
    service->reserved_events = 0U;
    service->event_committed = 1U;
    ++service->next_sequence;
    if (reset && service->fixture_enabled != 0U &&
        atomic_load_explicit(&service->fixture_terminal_armed,
                             memory_order_acquire) != 0U) {
        const struct np2audio86_event *published = NULL;
        service->fixture_terminal_reset_ordinal = service->reset_ordinal;
        atomic_store_explicit(&service->fixture_terminal_deferred, 1U,
                              memory_order_release);
        service->fixture_reset_event_before_horizon =
            event.frame_timestamp == P4_NANO_AUDIO86_5D3_RESET_FRAME &&
                    np2audio86_event_ring_peek(&service->events,
                                               &published) ==
                        NP2_AUDIO86_TRANSPORT_OK &&
                    published != NULL &&
                    published->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
                    published->payload == service->fixture_terminal_reset_ordinal &&
                    !np2audio86_runtime_horizon_pending(&service->control)
                ? 1U
                : 0U;
        /* Deliberately no wake here.  The terminal horizon release below is
         * the next publication and its following wake is the sole hint. */
        return;
    }
    wake_waiters(service);
}

static void transaction_close(struct p4_nano_audio86_live_service *service)
{
    service->transaction_active = 0U;
    service->horizon_owned = 0U;
    service->transaction_kind = 0U;
    service->event_committed = 0U;
    service->run_committed = 0U;
    atomic_store_explicit(
        &service->transaction_gate,
        producer_terminated(service) ? LIVE_TRANSACTION_GATE_CLOSED
                                     : LIVE_TRANSACTION_GATE_OPEN,
        memory_order_release);
}

static void guest_commit_horizon(void *opaque,
                                 np2audio86_guest_transaction_t *token,
                                 uint64_t frame)
{
    struct p4_nano_audio86_live_service *service = opaque;
    bool reset;
    int result;
    if (service == NULL || token == NULL ||
        service->transaction_active == 0U ||
        service->horizon_owned == 0U ||
        (service->event_committed == 0U && service->run_committed == 0U) ||
        token->opaque[1] != service->active_generation) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return;
    }
    reset = service->transaction_kind ==
            NP2AUDIO86_GUEST_TRANSACTION_RESET;
    if (reset && service->fixture_enabled != 0U &&
        atomic_load_explicit(&service->fixture_terminal_deferred,
                             memory_order_acquire) != 0U) {
        if (frame != P4_NANO_AUDIO86_5D3_RESET_FRAME ||
            service->fixture_terminal_reset_ordinal == 0U) {
            result = NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
        } else if (service->fixture_terminal_test_mode == 2U) {
            result = NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
        } else {
            result = np2audio86_runtime_terminal_horizon_publish(
                &service->control, &service->producer_clock,
                P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON,
                P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON,
                service->fixture_terminal_reset_ordinal);
        }
    } else {
        result = np2audio86_runtime_horizon_publish(
            &service->control, &service->producer_clock, frame);
    }
    transaction_close(service);
    if (result != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        if (reset && service->fixture_enabled != 0U &&
            atomic_load_explicit(&service->fixture_terminal_deferred,
                                 memory_order_acquire) != 0U) {
            const struct np2audio86_event *published = NULL;
            service->fixture_partial_failure_event_visible =
                np2audio86_event_ring_peek(&service->events, &published) ==
                        NP2_AUDIO86_TRANSPORT_OK &&
                    published != NULL &&
                    published->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER
                ? 1U
                : 0U;
            atomic_store_explicit(&service->fixture_terminal_armed, 0U,
                                  memory_order_release);
            atomic_store_explicit(&service->fixture_terminal_deferred, 0U,
                                  memory_order_release);
            atomic_store_explicit(&service->fixture_worker_hold, 0U,
                                  memory_order_release);
        }
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                          reset && service->fixture_enabled != 0U
                              ? LIVE_SUBCODE_5D3_TERMINAL
                              : LIVE_SUBCODE_HORIZON);
        if (reset && service->fixture_enabled != 0U)
            service->fixture_partial_failure_wake_issued = 1U;
        return;
    }
    if (reset && service->fixture_enabled != 0U &&
        atomic_load_explicit(&service->fixture_terminal_deferred,
                             memory_order_acquire) != 0U) {
        atomic_store_explicit(&service->fixture_terminal_horizon_published,
                              1U, memory_order_release);
        atomic_store_explicit(&service->fixture_terminal_armed, 0U,
                              memory_order_release);
        atomic_store_explicit(&service->fixture_terminal_deferred, 0U,
                              memory_order_release);
        atomic_store_explicit(&service->fixture_worker_hold, 0U,
                              memory_order_release);
    }
    wake_waiters(service);
    if (reset) {
        uint32_t ordinal = service->reset_ordinal;
        while (atomic_load_explicit(&service->first_error_latched,
                                    memory_order_acquire) == 0U &&
               np2audio86_runtime_reset_ack(&service->control) < ordinal)
            owner_wait(service);
        if (service->fixture_enabled != 0U) {
            while (atomic_load_explicit(&service->first_error_latched,
                                        memory_order_acquire) == 0U &&
                   atomic_load_explicit(&service->fixture_terminal_pcm_ready,
                                        memory_order_acquire) == 0U)
                owner_wait(service);
        }
    }
}

static int guest_publish_progress_checked(void *opaque, uint64_t frame)
{
    struct p4_nano_audio86_live_service *service = opaque;
    int result;
    if (service == NULL || service->transaction_active != 0U ||
        service->horizon_owned != 0U) {
        if (service != NULL)
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_GUEST_CONTRACT);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    if (producer_terminated(service) &&
        atomic_load_explicit(&service->owner_finalizing,
                             memory_order_acquire) == 0U)
        return NP2AUDIO86_GUEST_TRANSACTION_TERMINATED;
    result = np2audio86_runtime_horizon_publish(
        &service->control, &service->producer_clock, frame);
    if (result == NP2_AUDIO86_RUNTIME_HORIZON_RETRY)
        return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    if (result != NP2_AUDIO86_RUNTIME_HORIZON_OK) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                          LIVE_SUBCODE_HORIZON);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    wake_waiters(service);
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static bool drain_output(struct p4_nano_audio86_live_service *service,
                         bool *retry)
{
    enum np2_pcm_output_status status;
    *retry = false;
    for (;;) {
        if (service_state(service) == P4_NANO_AUDIO86_LIVE_FAILING ||
            atomic_load_explicit(&service->first_error_latched,
                                 memory_order_acquire) != 0U)
            return false;
        status = np2_pcm_output_step(&service->output);
        if (status == NP2_PCM_OUTPUT_CONSUMED) {
            snapshot_u64_publish(&service->accepted_frames_published,
                                 service->output.accepted_frames);
            continue;
        }
        if (status == NP2_PCM_OUTPUT_EMPTY)
            return true;
        if (status == NP2_PCM_OUTPUT_RETRY) {
            *retry = true;
            return true;
        }
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_PCM_OUTPUT,
                          LIVE_SUBCODE_PCM_OUTPUT);
        return false;
    }
}

static bool append_pcm(struct p4_nano_audio86_live_service *service,
                       const uint8_t *pcm, size_t frames,
                       uint64_t frame_offset)
{
    size_t appended = 0U;
    while (appended < frames) {
        size_t consumed = 0U;
        bool retry = false;
        int result = np2opngen_pcm_ring_append(
            &service->pcm_ring, pcm + appended * 4U, frames - appended,
            frame_offset + appended, &consumed);
        appended += consumed;
        if (service->fixture_observe_ring != NULL)
            service->fixture_observe_ring(
                service->fixture_opaque,
                np2opngen_pcm_ring_occupancy(&service->pcm_ring),
                service->pcm_ring.next_sequence,
                service->pcm_ring.next_frame_offset);
        if (service->fixture_enabled != 0U &&
            service->pcm_ring.next_sequence == 400U &&
            service->pcm_ring.next_frame_offset ==
                P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON &&
            np2opngen_pcm_ring_producer_partial_valid_frames(
                &service->pcm_ring) == 0U &&
            service->fixture_q399_published == 0U) {
            service->fixture_q399_published = 1U;
            fixture_terminal_point(service,
                                   P4_NANO_AUDIO86_5D3_T9_Q399_PUBLISHED);
        }
        if (!drain_output(service, &retry))
            return false;
        if (result == NP2_OPNGEN_PCM_RING_OK)
            continue;
        if (result != NP2_OPNGEN_PCM_RING_FULL) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_PCM_OUTPUT,
                              LIVE_SUBCODE_PCM_RING);
            return false;
        }
        if (retry || np2opngen_pcm_ring_occupancy(&service->pcm_ring) ==
                         NP2_OPNGEN_PCM_RING_CAPACITY)
            worker_wait(service, 1U);
        if (atomic_load_explicit(&service->first_error_latched,
                                 memory_order_acquire) != 0U)
            return false;
    }
    return true;
}

static bool render_until(struct p4_nano_audio86_live_service *service,
                         uint64_t target)
{
    if (target < service->rendered_frame)
        return false;
    while (service->rendered_frame < target) {
        struct np2audio86_core_mix_result mix_result;
        struct np2opngen_pcm_stats pcm_stats;
        uint64_t remaining = target - service->rendered_frame;
        size_t frames = remaining > NP2_AUDIO86_QUANTUM_FRAMES
                            ? NP2_AUDIO86_QUANTUM_FRAMES
                            : (size_t)remaining;
        if (atomic_load_explicit(&service->first_error_latched,
                                 memory_order_acquire) != 0U)
            return false;
        memset(service->mix, 0, sizeof(service->mix));
        memset(&mix_result, 0, sizeof(mix_result));
        if (np2audio86_core_render_span(&service->render, service->mix,
                                        frames, &mix_result) != 0 ||
            mix_result.arithmetic_error != 0U ||
            np2opngen_pcm_canonicalize_s16le(
                service->mix, frames, 2U, service->canonical, frames * 4U,
                &pcm_stats) != 0) {
            if (atomic_load_explicit(&service->first_error_latched,
                                     memory_order_acquire) == 0U)
                (void)first_fatal(service,
                                  P4_NANO_AUDIO86_LIVE_FAILURE_RENDERER,
                                  P4_NANO_AUDIO86_LIVE_ORIGIN_RENDER,
                                  LIVE_SUBCODE_RENDER);
            return false;
        }
        if (service->fixture_observe_rendered_pcm != NULL &&
            service->fixture_observe_rendered_pcm(
                service->fixture_opaque, service->canonical,
                (uint16_t)frames, service->rendered_frame) != 0) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_RENDERER,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_RENDER,
                              LIVE_SUBCODE_RENDER);
            return false;
        }
        if (service->fixture_enabled != 0U &&
            atomic_load_explicit(&service->fixture_reset_applied_ordinal,
                                 memory_order_acquire) != 0U &&
            service->rendered_frame + frames ==
                P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON)
            fixture_terminal_point(
                service,
                P4_NANO_AUDIO86_5D3_T8_POST_RESET_SYNTHESIS_COMPLETE);
        if (!append_pcm(service, service->canonical, frames,
                        service->rendered_frame))
            return false;
        service->rendered_frame += frames;
        snapshot_u64_publish(&service->rendered_frames_published,
                             service->rendered_frame);
    }
    return true;
}

static bool apply_event(struct p4_nano_audio86_live_service *service,
                        const struct np2audio86_event *event)
{
    struct np2audio86_core_guest_action action;
    const uint8_t *data = NULL;
    uint32_t data_count = 0U;
    uint32_t reset_ordinal = 0U;
    int result;

    if (event == NULL || event->sequence != service->expected_sequence ||
        !render_until(service, event->frame_timestamp))
        return false;
    memset(&action, 0, sizeof(action));
    action.frame_timestamp = event->frame_timestamp;
    action.sequence = event->sequence;
    action.payload = event->payload;
    if (event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
        if (event->payload == 0U ||
            event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            np2audio86_byte_ring_pop(&service->bytes, service->worker_run,
                                     event->payload) !=
                NP2_AUDIO86_TRANSPORT_OK)
            return false;
        action.opcode = NP2AUDIO86_TRACE_PCM_CONTROL;
        action.kind = NP2_AUDIO86_CORE_ACTION_DATA_RUN;
        action.payload = 0U;
        action.byte_offset = service->worker_byte_offset;
        action.byte_count = event->payload;
        data = service->worker_run;
        data_count = event->payload;
        service->worker_byte_offset += event->payload;
        result = np2audio86_core_guest_action_apply(
            &service->render, &action, data, data_count);
    } else if (event->opcode == LIVE_EVENT_OPNA_REGISTER) {
        action.opcode = NP2AUDIO86_TRACE_OPNA_REGISTER;
        action.kind = NP2_AUDIO86_CORE_ACTION_OPNA_REGISTER;
        result = np2audio86_core_guest_action_apply(
            &service->render, &action, NULL, 0U);
    } else if (event->opcode == LIVE_EVENT_OPNA_CSM) {
        action.opcode = NP2AUDIO86_TRACE_OPNA_CSM;
        action.kind = NP2_AUDIO86_CORE_ACTION_OPNA_CSM;
        result = np2audio86_core_guest_action_apply(
            &service->render, &action, NULL, 0U);
    } else if (event->opcode == LIVE_EVENT_PCM_CONTROL) {
        action.opcode = NP2AUDIO86_TRACE_PCM_CONTROL;
        action.kind = NP2_AUDIO86_CORE_ACTION_PCM_CONTROL;
        result = np2audio86_core_guest_action_apply(
            &service->render, &action, NULL, 0U);
    } else if (event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER) {
        if (event->payload == 0U)
            return false;
        reset_ordinal = event->payload;
        action.opcode = NP2AUDIO86_TRACE_RESET_BARRIER;
        action.kind = NP2_AUDIO86_CORE_ACTION_RESET;
        /* The ordinal is transport metadata, not the canonical guest RESET
         * payload.  Preserve the frozen action record's zero payload. */
        action.payload = 0U;
        if (service->fixture_enabled != 0U) {
            if (event->frame_timestamp != P4_NANO_AUDIO86_5D3_RESET_FRAME ||
                atomic_load_explicit(
                    &service->fixture_terminal_horizon_observed,
                    memory_order_acquire) == 0U ||
                reset_ordinal != service->fixture_terminal_reset_ordinal)
                return false;
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T1_PRE_RESET_RENDER_COMPLETE);
            fixture_terminal_point(service,
                                   P4_NANO_AUDIO86_5D3_T2_RESET_ACTION_BEGIN);
        }
        result = np2audio86_core_render_reset(&service->render);
        if (result == 0 && service->fixture_decorate_render != NULL)
            result = service->fixture_decorate_render(
                service->fixture_opaque, &service->render, 1U);
        if (service->fixture_enabled != 0U)
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T3_RESET_ACTION_COMPLETE);
    } else {
        return false;
    }
    if (result == 0 && service->fixture_observe_applied_action != NULL)
        result = service->fixture_observe_applied_action(
            service->fixture_opaque, &action, reset_ordinal,
            service->pcm_ring.next_frame_offset);
    if (service->fixture_enabled != 0U && reset_ordinal != 0U)
        fixture_terminal_point(
            service, P4_NANO_AUDIO86_5D3_T4_RESET_EVIDENCE_COMPLETE);
    if (result != 0 ||
        np2audio86_event_ring_consume(&service->events) !=
            NP2_AUDIO86_TRANSPORT_OK)
        return false;
    ++service->expected_sequence;
    if (reset_ordinal != 0U) {
        atomic_store_explicit(&service->fixture_reset_applied_ordinal,
                              reset_ordinal, memory_order_release);
        np2audio86_runtime_reset_ack_publish(&service->control,
                                             reset_ordinal);
        if (service->fixture_enabled != 0U)
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T5_RESET_ACK_PUBLISHED);
    }
    wake_waiters(service);
    return true;
}

static bool close_pcm_ring(struct p4_nano_audio86_live_service *service)
{
    for (;;) {
        bool retry = false;
        int status = np2opngen_pcm_ring_finish(&service->pcm_ring,
                                                service->rendered_frame);
        if (status == NP2_OPNGEN_PCM_RING_OK)
            return true;
        if (status != NP2_OPNGEN_PCM_RING_FULL) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH,
                              LIVE_SUBCODE_PCM_RING);
            return false;
        }
        if (!drain_output(service, &retry))
            return false;
        if (retry || np2opngen_pcm_ring_occupancy(&service->pcm_ring) ==
                         NP2_OPNGEN_PCM_RING_CAPACITY)
            worker_wait(service, 1U);
    }
}

static bool finish_output(struct p4_nano_audio86_live_service *service)
{
    for (;;) {
        bool retry = false;
        enum np2_pcm_output_status status;
        if (!drain_output(service, &retry))
            return false;
        if (retry) {
            worker_wait(service, 1U);
            continue;
        }
        status = np2_pcm_output_finish(&service->output);
        if (status == NP2_PCM_OUTPUT_OK)
            return true;
        if (status == NP2_PCM_OUTPUT_RETRY) {
            if (atomic_load_explicit(&service->first_error_latched,
                                     memory_order_acquire) != 0U)
                return false;
            worker_wait(service, 1U);
            continue;
        }
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH,
                          LIVE_SUBCODE_FINISH);
        return false;
    }
}

static bool abort_output_barrier(
    struct p4_nano_audio86_live_service *service)
{
    if (service->output.state == NP2_PCM_OUTPUT_INITIAL ||
        service->output.state == NP2_PCM_OUTPUT_ABORTED)
        return true;
    if (service->output.state == NP2_PCM_OUTPUT_FINISHED)
        return true;
    for (;;) {
        enum np2_pcm_output_status status = np2_pcm_output_abort(
            &service->output);
        if (status == NP2_PCM_OUTPUT_OK)
            return true;
        if (status == NP2_PCM_OUTPUT_RETRY) {
            worker_wait(service, 1U);
            continue;
        }
        /* Cleanup outcome is separate from the immutable first fatal.  A
         * failed barrier changes quiescence, never its initiating error. */
        return false;
    }
}

static void release_borrow(struct p4_nano_audio86_live_service *service)
{
    memset(&service->output.sink, 0, sizeof(service->output.sink));
    service->borrowed_sink = NULL;
    atomic_store_explicit(&service->sink_reachable, 0U,
                          memory_order_release);
}

static void publish_terminal_state(
    struct p4_nano_audio86_live_service *service,
    enum p4_nano_audio86_live_state state,
    enum p4_nano_audio86_live_cleanup cleanup)
{
    atomic_store_explicit(&service->cleanup, (uint32_t)cleanup,
                          memory_order_relaxed);
    atomic_store_explicit(&service->worker_safe, 1U, memory_order_release);
    /* This is the worker's last service-memory access.  The platform wrapper
     * only returns/suspends after this release publication. */
    atomic_store_explicit(&service->state, (uint32_t)state,
                          memory_order_release);
}

static bool publish_clean_terminal(
    struct p4_nano_audio86_live_service *service)
{
    uint32_t expected = P4_NANO_AUDIO86_LIVE_DRAINING;
    atomic_store_explicit(&service->cleanup,
                          P4_NANO_AUDIO86_LIVE_CLEANUP_QUIESCENT,
                          memory_order_relaxed);
    atomic_store_explicit(&service->worker_safe, 1U, memory_order_release);
    if (atomic_compare_exchange_strong_explicit(
            &service->state, &expected,
            P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT, memory_order_release,
            memory_order_acquire)) {
        /* The successful terminal CAS is the worker's last service access. */
        return true;
    }
    /* Fatal won before terminal publication.  The worker is still active and
     * must not leave a transient reclamation proof behind. */
    atomic_store_explicit(&service->worker_safe, 0U, memory_order_release);
    return false;
}

static void worker_run(struct p4_nano_audio86_live_service *service)
{
    enum np2_pcm_output_status start_status;
    uint64_t authority = 0U;
    uint64_t fixture_terminal_horizon = 0U;
    uint32_t fixture_terminal_reset_ordinal = 0U;
    bool clean_barrier = false;

    do {
        start_status = np2_pcm_output_start(&service->output);
        if (start_status == NP2_PCM_OUTPUT_RETRY)
            worker_wait(service, 1U);
    } while (start_status == NP2_PCM_OUTPUT_RETRY &&
             atomic_load_explicit(&service->first_error_latched,
                                  memory_order_acquire) == 0U);
    if (start_status != NP2_PCM_OUTPUT_OK) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_OUTPUT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_START,
                          LIVE_SUBCODE_SINK_START);
    } else {
        uint32_t expected = P4_NANO_AUDIO86_LIVE_STARTING;
        if (atomic_compare_exchange_strong_explicit(
                &service->state, &expected,
                P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED,
                memory_order_acq_rel, memory_order_acquire)) {
            wake_waiters(service);
        }
    }

    while (atomic_load_explicit(&service->first_error_latched,
                                memory_order_acquire) == 0U) {
        struct np2audio86_runtime_horizon_observation observation;
        const struct np2audio86_event *event = NULL;
        int horizon_status;
        int event_status;
        bool retry = false;
        bool progressed = false;

        if (atomic_load_explicit(&service->fixture_worker_hold,
                                 memory_order_acquire) != 0U) {
            atomic_store_explicit(&service->fixture_worker_hold_ack, 1U,
                                  memory_order_release);
            wake_waiters(service);
            worker_wait(service, 1U);
            continue;
        }

        horizon_status = np2audio86_runtime_horizon_try_observe_detail(
            &service->control, &service->consumer_clock, &observation);
        if (horizon_status == NP2_AUDIO86_RUNTIME_HORIZON_OK) {
            if (observation.flags ==
                    NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL &&
                service->fixture_enabled != 0U &&
                fixture_terminal_horizon == 0U &&
                observation.frame ==
                    P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON &&
                observation.terminal_reset_ordinal != 0U) {
                fixture_terminal_horizon = observation.frame;
                fixture_terminal_reset_ordinal =
                    observation.terminal_reset_ordinal;
                atomic_store_explicit(
                    &service->fixture_terminal_horizon_observed, 1U,
                    memory_order_release);
            } else if (observation.flags !=
                       NP2_AUDIO86_RUNTIME_HORIZON_FLAG_NONE) {
                (void)first_fatal(
                    service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                    P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                    LIVE_SUBCODE_HORIZON);
                break;
            }
            authority = observation.frame;
            progressed = true;
            wake_waiters(service);
        } else if (horizon_status != NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_HORIZON);
            break;
        }

        event_status = np2audio86_event_ring_peek(&service->events, &event);
        if (fixture_terminal_horizon != 0U &&
            service->fixture_worker_observed_pair == 0U &&
            event_status == NP2_AUDIO86_TRANSPORT_OK && event != NULL &&
            event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
            event->frame_timestamp == P4_NANO_AUDIO86_5D3_RESET_FRAME &&
            event->payload == fixture_terminal_reset_ordinal) {
            service->fixture_worker_observed_pair = 1U;
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T0_TERMINAL_PAIR_OBSERVED);
        }
        if (service->fixture_enabled != 0U &&
            event_status == NP2_AUDIO86_TRANSPORT_OK && event != NULL &&
            event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER &&
            fixture_terminal_horizon == 0U) {
            /* The event release intentionally precedes the terminal mailbox.
             * A timeout/poll is not authority to consume the pair halfway. */
            worker_wait(service, 1U);
            continue;
        }
        if (event_status == NP2_AUDIO86_TRANSPORT_OK && event != NULL &&
            event->frame_timestamp <= authority) {
            if (!apply_event(service, event)) {
                if (atomic_load_explicit(&service->first_error_latched,
                                         memory_order_acquire) == 0U)
                    (void)first_fatal(
                        service, P4_NANO_AUDIO86_LIVE_FAILURE_RENDERER,
                        P4_NANO_AUDIO86_LIVE_ORIGIN_RENDER,
                        LIVE_SUBCODE_EVENT);
                break;
            }
            progressed = true;
            continue;
        }
        if (event_status != NP2_AUDIO86_TRANSPORT_EMPTY &&
            event_status != NP2_AUDIO86_TRANSPORT_OK) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_GUEST_TRANSPORT,
                              LIVE_SUBCODE_EVENT);
            break;
        }
        if (fixture_terminal_horizon != 0U &&
            service->fixture_output_finished == 0U &&
            atomic_load_explicit(&service->fixture_reset_applied_ordinal,
                                 memory_order_acquire) ==
                fixture_terminal_reset_ordinal &&
            np2audio86_event_ring_occupancy(&service->events) == 0U &&
            np2audio86_byte_ring_occupancy(&service->bytes) == 0U &&
            !np2audio86_runtime_horizon_pending(&service->control)) {
            fixture_terminal_point(
                service,
                P4_NANO_AUDIO86_5D3_T6_TERMINAL_PREDICATE_READY);
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T7_POST_RESET_RENDER_BEGIN);
            service->fixture_reset_before_remainder =
                service->rendered_frame == P4_NANO_AUDIO86_5D3_RESET_FRAME
                    ? 1U
                    : 0U;
            if (!render_until(service, fixture_terminal_horizon) ||
                !close_pcm_ring(service) || !finish_output(service))
                break;
            service->fixture_output_finished = 1U;
            fixture_terminal_point(
                service, P4_NANO_AUDIO86_5D3_T10_PCM_FINISH_COMPLETE);
            atomic_store_explicit(
                &service->fixture_terminal_pcm_before_done,
                np2audio86_runtime_producer_done(&service->control) ? 0U : 1U,
                memory_order_release);
            atomic_store_explicit(&service->fixture_terminal_pcm_ready, 1U,
                                  memory_order_release);
            wake_waiters(service);
            progressed = true;
        }
        if (service->fixture_output_finished == 0U &&
            service->rendered_frame < authority) {
            if (!render_until(service, authority))
                break;
            progressed = true;
        }
        if (service->fixture_output_finished == 0U &&
            !drain_output(service, &retry))
            break;
        if (retry)
            progressed = false;

        if (np2audio86_runtime_producer_done(&service->control)) {
            uint64_t final_horizon = snapshot_u64_read(
                &service->final_horizon);
            if (!np2audio86_runtime_horizon_pending(&service->control) &&
                np2audio86_event_ring_occupancy(&service->events) == 0U &&
                np2audio86_byte_ring_occupancy(&service->bytes) == 0U) {
                if (authority != final_horizon ||
                    service->rendered_frame != final_horizon) {
                    (void)first_fatal(
                        service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                        P4_NANO_AUDIO86_LIVE_ORIGIN_FINISH,
                        LIVE_SUBCODE_HORIZON);
                    break;
                }
                if (service->fixture_output_finished == 0U) {
                    if (!close_pcm_ring(service) || !finish_output(service))
                        break;
                }
                clean_barrier = true;
                release_borrow(service);
#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_TESTING)
                if (service->before_clean_terminal_hook != NULL)
                    service->before_clean_terminal_hook(
                        service,
                        service->before_clean_terminal_hook_opaque);
#endif
                if (publish_clean_terminal(service))
                    return;
                break;
            }
        }
        if (!progressed)
            worker_wait(service, 1U);
    }

    if (!clean_barrier) {
        bool barrier = abort_output_barrier(service);
        if (!barrier) {
            publish_terminal_state(
                service, P4_NANO_AUDIO86_LIVE_FAILED_NOT_QUIESCENT,
                P4_NANO_AUDIO86_LIVE_CLEANUP_NOT_QUIESCENT);
            return;
        }
        release_borrow(service);
    }
    while (atomic_load_explicit(&service->guest_attached,
                                memory_order_acquire) != 0U)
        worker_wait(service, 1U);
    if (service->fixture_enabled != 0U) {
        struct np2audio86_runtime_horizon_observation discarded_horizon;
        while (np2audio86_event_ring_occupancy(&service->events) != 0U)
            (void)np2audio86_event_ring_consume(&service->events);
        while (np2audio86_byte_ring_occupancy(&service->bytes) != 0U) {
            uint32_t bytes = np2audio86_byte_ring_occupancy(&service->bytes);
            if (bytes > sizeof(service->worker_run))
                bytes = sizeof(service->worker_run);
            if (np2audio86_byte_ring_pop(&service->bytes,
                                         service->worker_run, bytes) !=
                NP2_AUDIO86_TRANSPORT_OK)
                break;
        }
        while (np2audio86_runtime_horizon_pending(&service->control))
            (void)np2audio86_runtime_horizon_try_observe_detail(
                &service->control, &service->consumer_clock,
                &discarded_horizon);
    }
    publish_terminal_state(service, P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT,
                           P4_NANO_AUDIO86_LIVE_CLEANUP_QUIESCENT);
}

#if defined(ESP_PLATFORM)
static void worker_entry(void *opaque)
{
    struct p4_nano_audio86_live_service *service = opaque;
    worker_run(service);
    vTaskSuspend(NULL);
}
#else
static void *worker_entry(void *opaque)
{
    worker_run(opaque);
    return NULL;
}
#endif

static void init_guest_sink(struct p4_nano_audio86_live_service *service)
{
    service->guest_sink.opaque = service;
    service->guest_sink.reserve_checked = guest_reserve_checked;
    service->guest_sink.extend_checked = guest_extend_checked;
    service->guest_sink.commit_event = guest_commit_event;
    service->guest_sink.commit_pcm_byte = guest_commit_pcm_byte;
    service->guest_sink.commit_data_run = guest_commit_data_run;
    service->guest_sink.commit_horizon = guest_commit_horizon;
    service->guest_sink.publish_progress_checked =
        guest_publish_progress_checked;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_init(
    struct p4_nano_audio86_live_service *service,
    const struct p4_nano_audio86_live_config *config)
{
    if (service == NULL || config == NULL || config->borrowed_sink == NULL ||
        config->borrowed_sink->start == NULL ||
        config->borrowed_sink->submit == NULL ||
        config->borrowed_sink->finish == NULL ||
        config->borrowed_sink->abort == NULL)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (service->magic == P4_NANO_AUDIO86_LIVE_MAGIC &&
        service_state(service) != P4_NANO_AUDIO86_LIVE_DESTROYED)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    memset(service, 0, sizeof(*service));
#if !defined(ESP_PLATFORM)
    if (pthread_mutex_init(&service->wait_mutex, NULL) != 0)
        return P4_NANO_AUDIO86_LIVE_FAILED;
    if (pthread_cond_init(&service->wait_cond, NULL) != 0) {
        (void)pthread_mutex_destroy(&service->wait_mutex);
        return P4_NANO_AUDIO86_LIVE_FAILED;
    }
#endif
    service->magic = P4_NANO_AUDIO86_LIVE_MAGIC;
    atomic_init(&service->state, P4_NANO_AUDIO86_LIVE_UNINITIALIZED);
    atomic_init(&service->stop_intent, 0U);
    atomic_init(&service->first_error_latched, 0U);
    atomic_init(&service->failure_category,
                P4_NANO_AUDIO86_LIVE_FAILURE_NONE);
    atomic_init(&service->failure_origin, P4_NANO_AUDIO86_LIVE_ORIGIN_NONE);
    atomic_init(&service->failure_subcode, 0U);
    atomic_init(&service->first_error_sequence, 0U);
    atomic_init(&service->cleanup,
                P4_NANO_AUDIO86_LIVE_CLEANUP_NOT_ATTEMPTED);
    atomic_init(&service->guest_attached, 0U);
    atomic_init(&service->sink_reachable, 0U);
    atomic_init(&service->worker_safe, 0U);
    atomic_init(&service->producer_closing, 0U);
    atomic_init(&service->owner_finalizing, 0U);
    atomic_init(&service->transaction_gate, LIVE_TRANSACTION_GATE_OPEN);
    atomic_init(&service->owner_valid, 0U);
#if defined(ESP_PLATFORM)
    atomic_init(&service->worker_handle_ready, 0U);
#endif
    atomic_init(&service->final_horizon.sequence, 0U);
    atomic_init(&service->final_horizon.low, 0U);
    atomic_init(&service->final_horizon.high, 0U);
    atomic_init(&service->rendered_frames_published.sequence, 0U);
    atomic_init(&service->rendered_frames_published.low, 0U);
    atomic_init(&service->rendered_frames_published.high, 0U);
    atomic_init(&service->accepted_frames_published.sequence, 0U);
    atomic_init(&service->accepted_frames_published.low, 0U);
    atomic_init(&service->accepted_frames_published.high, 0U);
    atomic_init(&service->fixture_terminal_armed, 0U);
    atomic_init(&service->fixture_terminal_deferred, 0U);
    atomic_init(&service->fixture_terminal_horizon_published, 0U);
    atomic_init(&service->fixture_terminal_horizon_observed, 0U);
    atomic_init(&service->fixture_terminal_pcm_ready, 0U);
    atomic_init(&service->fixture_terminal_pcm_before_done, 0U);
    atomic_init(&service->fixture_reset_applied_ordinal, 0U);
    atomic_init(&service->fixture_worker_hold, 0U);
    atomic_init(&service->fixture_worker_hold_ack, 0U);
    np2audio86_event_ring_init(&service->events);
    np2audio86_byte_ring_init(&service->bytes);
    np2audio86_runtime_control_init(&service->control);
    np2opngen_pcm_ring_init(&service->pcm_ring);
    if (np2audio86_core_render_init(&service->render) != 0 ||
        np2_pcm_output_controller_init(&service->output, &service->pcm_ring,
                                       config->borrowed_sink) != 0) {
#if !defined(ESP_PLATFORM)
        (void)pthread_cond_destroy(&service->wait_cond);
        (void)pthread_mutex_destroy(&service->wait_mutex);
#endif
        service->magic = 0U;
        return P4_NANO_AUDIO86_LIVE_FAILED;
    }
    service->borrowed_sink = config->borrowed_sink;
    init_guest_sink(service);
    atomic_store_explicit(&service->state, P4_NANO_AUDIO86_LIVE_READY,
                          memory_order_release);
    return P4_NANO_AUDIO86_LIVE_OK;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_start(
    struct p4_nano_audio86_live_service *service)
{
    uint32_t expected = P4_NANO_AUDIO86_LIVE_READY;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (!atomic_compare_exchange_strong_explicit(
            &service->state, &expected, P4_NANO_AUDIO86_LIVE_STARTING,
            memory_order_acq_rel, memory_order_acquire))
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    atomic_store_explicit(&service->sink_reachable, 1U,
                          memory_order_release);
#if defined(ESP_PLATFORM)
    service->worker_task = xTaskCreateStaticPinnedToCore(
        worker_entry, "audio86_live", P4_NANO_AUDIO86_LIVE_WORKER_STACK_BYTES /
                                          sizeof(StackType_t),
        service, tskIDLE_PRIORITY + 6U, service->worker_stack,
        &service->worker_tcb, 0);
    if (service->worker_task == NULL) {
#else
    if (pthread_create(&service->worker_thread, NULL, worker_entry, service) !=
        0) {
#endif
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_START,
                          LIVE_SUBCODE_THREAD_CREATE);
        release_borrow(service);
        atomic_store_explicit(&service->cleanup,
                              P4_NANO_AUDIO86_LIVE_CLEANUP_QUIESCENT,
                              memory_order_relaxed);
        atomic_store_explicit(&service->worker_safe, 1U,
                              memory_order_release);
        atomic_store_explicit(&service->state,
                              P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT,
                              memory_order_release);
        return P4_NANO_AUDIO86_LIVE_FAILED;
    }
#if defined(ESP_PLATFORM)
    atomic_store_explicit(&service->worker_handle_ready, 1U,
                          memory_order_release);
#endif
    while (service_state(service) == P4_NANO_AUDIO86_LIVE_STARTING)
        owner_wait(service);
    return service_state(service) ==
                   P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED
               ? P4_NANO_AUDIO86_LIVE_OK
               : P4_NANO_AUDIO86_LIVE_FAILED;
}

static enum p4_nano_audio86_live_result owner_detach_failure(
    struct p4_nano_audio86_live_service *service);

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_attach_guest(
    struct p4_nano_audio86_live_service *service)
{
    uint32_t expected = P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (service_state(service) !=
        P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
#if defined(ESP_PLATFORM)
    service->owner_task = xTaskGetCurrentTaskHandle();
#else
    service->owner_thread = pthread_self();
#endif
    atomic_store_explicit(&service->owner_valid, 1U, memory_order_release);
    if (!atomic_compare_exchange_strong_explicit(
            &service->state, &expected, P4_NANO_AUDIO86_LIVE_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&service->owner_valid, 0U,
                              memory_order_release);
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    }
    np2audio86_guest_sink_bind(&service->guest_sink);
    atomic_store_explicit(&service->guest_attached, 1U,
                          memory_order_release);
    if (np2audio86_guest_host_failed()) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_ATTACH,
                          LIVE_SUBCODE_GUEST_CONTRACT);
        return owner_detach_failure(service);
    }
    wake_waiters(service);
    return P4_NANO_AUDIO86_LIVE_OK;
}

static enum p4_nano_audio86_live_result owner_detach_failure(
    struct p4_nano_audio86_live_service *service)
{
    if (atomic_exchange_explicit(&service->guest_attached, 0U,
                                 memory_order_acq_rel) != 0U)
        np2audio86_guest_sink_unbind();
    np2audio86_runtime_producer_done_publish(&service->control);
    wake_waiters(service);
    return P4_NANO_AUDIO86_LIVE_FAILED;
}

static enum p4_nano_audio86_live_result owner_finalize_clean(
    struct p4_nano_audio86_live_service *service)
{
    np2audio86_guest_state_snapshot_t snapshot;
    int horizon_status;
    atomic_store_explicit(&service->producer_closing, 1U,
                          memory_order_release);
    atomic_store_explicit(&service->owner_finalizing, 1U,
                          memory_order_release);
    if (service->transaction_active != 0U) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
                          LIVE_SUBCODE_GUEST_CONTRACT);
        atomic_store_explicit(&service->owner_finalizing, 0U,
                              memory_order_release);
        return owner_detach_failure(service);
    }
    /* Owner-local authoritative finalization.  No RESET and no terminal
     * extension are synthesized. */
    np2audio86_guest_audio_sync();
    np2audio86_guest_host_flush_data_run();
    if (np2audio86_guest_host_failed()) {
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
                          LIVE_SUBCODE_GUEST_CONTRACT);
        atomic_store_explicit(&service->owner_finalizing, 0U,
                              memory_order_release);
        return owner_detach_failure(service);
    }
    np2audio86_guest_host_snapshot(&snapshot);
    for (;;) {
        if (atomic_load_explicit(&service->first_error_latched,
                                 memory_order_acquire) != 0U) {
            atomic_store_explicit(&service->owner_finalizing, 0U,
                                  memory_order_release);
            return owner_detach_failure(service);
        }
        horizon_status = np2audio86_runtime_horizon_publish(
            &service->control, &service->producer_clock,
            snapshot.frame_timestamp);
        if (horizon_status == NP2_AUDIO86_RUNTIME_HORIZON_OK)
            break;
        if (horizon_status != NP2_AUDIO86_RUNTIME_HORIZON_RETRY) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_TRANSPORT,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
                              LIVE_SUBCODE_HORIZON);
            atomic_store_explicit(&service->owner_finalizing, 0U,
                                  memory_order_release);
            return owner_detach_failure(service);
        }
        wake_waiters(service);
        owner_wait(service);
    }
    snapshot_u64_publish(&service->final_horizon, snapshot.frame_timestamp);
    np2audio86_guest_sink_unbind();
    atomic_store_explicit(&service->guest_attached, 0U,
                          memory_order_release);
    np2audio86_runtime_producer_done_publish(&service->control);
    atomic_store_explicit(&service->owner_finalizing, 0U,
                          memory_order_release);
    {
        uint32_t expected = P4_NANO_AUDIO86_LIVE_STOP_REQUESTED;
        if (!atomic_compare_exchange_strong_explicit(
                &service->state, &expected,
                P4_NANO_AUDIO86_LIVE_DRAINING, memory_order_acq_rel,
                memory_order_acquire)) {
            wake_waiters(service);
            return P4_NANO_AUDIO86_LIVE_FAILED;
        }
    }
    wake_waiters(service);
    return P4_NANO_AUDIO86_LIVE_OK;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_owner_checkpoint(
    struct p4_nano_audio86_live_service *service)
{
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (!owner_matches(service))
        return P4_NANO_AUDIO86_LIVE_OWNER_ERROR;
    for (;;) {
        enum p4_nano_audio86_live_state state = service_state(service);
        int result;
        if (state == P4_NANO_AUDIO86_LIVE_FAILING)
            return owner_detach_failure(service);
        if (state == P4_NANO_AUDIO86_LIVE_STOP_REQUESTED)
            return owner_finalize_clean(service);
        if (state != P4_NANO_AUDIO86_LIVE_RUNNING)
            return state_terminal(state) ? P4_NANO_AUDIO86_LIVE_FAILED
                                         : P4_NANO_AUDIO86_LIVE_STATE_ERROR;
        result = np2audio86_guest_progress_checkpoint();
        if (result == NP2AUDIO86_GUEST_TRANSACTION_OK)
            return P4_NANO_AUDIO86_LIVE_OK;
        if (result == NP2AUDIO86_GUEST_TRANSACTION_RETRY) {
            owner_wait(service);
            continue;
        }
        if (result == NP2AUDIO86_GUEST_TRANSACTION_TERMINATED &&
            service_state(service) == P4_NANO_AUDIO86_LIVE_STOP_REQUESTED)
            continue;
        (void)first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER,
                          P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
                          LIVE_SUBCODE_GUEST_CONTRACT);
        return owner_detach_failure(service);
    }
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_request_stop(
    struct p4_nano_audio86_live_service *service)
{
    uint32_t expected;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    for (;;) {
        enum p4_nano_audio86_live_state state = service_state(service);
        if (state == P4_NANO_AUDIO86_LIVE_STOP_REQUESTED ||
            state == P4_NANO_AUDIO86_LIVE_DRAINING)
            return P4_NANO_AUDIO86_LIVE_ALREADY;
        if (state != P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED &&
            state != P4_NANO_AUDIO86_LIVE_RUNNING)
            return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
        expected = (uint32_t)state;
        if (atomic_compare_exchange_weak_explicit(
                &service->state, &expected,
                P4_NANO_AUDIO86_LIVE_STOP_REQUESTED, memory_order_acq_rel,
                memory_order_acquire)) {
            atomic_store_explicit(&service->stop_intent, 1U,
                                  memory_order_release);
            atomic_store_explicit(&service->transaction_gate,
                                  LIVE_TRANSACTION_GATE_CLOSED,
                                  memory_order_release);
            np2audio86_runtime_stop_publish(&service->control);
            if (state == P4_NANO_AUDIO86_LIVE_STARTED_UNATTACHED) {
                /* No guest authority ever existed: the initialized zero
                 * horizon is final without fabricating a guest checkpoint. */
                atomic_store_explicit(&service->producer_closing, 1U,
                                      memory_order_release);
                snapshot_u64_publish(&service->final_horizon, 0U);
                np2audio86_runtime_producer_done_publish(&service->control);
                expected = P4_NANO_AUDIO86_LIVE_STOP_REQUESTED;
                (void)atomic_compare_exchange_strong_explicit(
                    &service->state, &expected,
                    P4_NANO_AUDIO86_LIVE_DRAINING, memory_order_acq_rel,
                    memory_order_acquire);
            }
            wake_waiters(service);
            return P4_NANO_AUDIO86_LIVE_OK;
        }
    }
}

enum p4_nano_audio86_live_result
p4_nano_audio86_live_service_report_producer_failure(
    struct p4_nano_audio86_live_service *service, uint32_t subcode)
{
    enum p4_nano_audio86_live_state state;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC ||
        subcode == 0U)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    state = service_state(service);
    if (state != P4_NANO_AUDIO86_LIVE_RUNNING &&
        state != P4_NANO_AUDIO86_LIVE_STOP_REQUESTED &&
        state != P4_NANO_AUDIO86_LIVE_DRAINING &&
        state != P4_NANO_AUDIO86_LIVE_FAILING)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    if (!owner_matches(service))
        return P4_NANO_AUDIO86_LIVE_OWNER_ERROR;
    if (!first_fatal(service, P4_NANO_AUDIO86_LIVE_FAILURE_PRODUCER,
                     P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT, subcode))
        return P4_NANO_AUDIO86_LIVE_ALREADY;
    return owner_detach_failure(service);
}

void p4_nano_audio86_live_service_status(
    const struct p4_nano_audio86_live_service *service,
    struct p4_nano_audio86_live_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC) {
        status->state = P4_NANO_AUDIO86_LIVE_UNINITIALIZED;
        return;
    }
    status->state = service_state(service);
    if (atomic_load_explicit(&service->first_error_latched,
                             memory_order_acquire) != 0U) {
        status->category = (enum p4_nano_audio86_live_failure_category)
            atomic_load_explicit(&service->failure_category,
                                 memory_order_relaxed);
        status->origin = (enum p4_nano_audio86_live_failure_origin)
            atomic_load_explicit(&service->failure_origin,
                                 memory_order_relaxed);
        status->subcode = atomic_load_explicit(&service->failure_subcode,
                                               memory_order_relaxed);
        status->first_error_sequence = atomic_load_explicit(
            &service->first_error_sequence, memory_order_relaxed);
    }
    status->cleanup = (enum p4_nano_audio86_live_cleanup)
        atomic_load_explicit(&service->cleanup, memory_order_acquire);
    status->rendered_frames = snapshot_u64_read(
        &service->rendered_frames_published);
    status->final_horizon = snapshot_u64_read(&service->final_horizon);
    status->accepted_frames = snapshot_u64_read(
        &service->accepted_frames_published);
    status->producer_done = np2audio86_runtime_producer_done(
                                &service->control)
                                ? 1U
                                : 0U;
    status->guest_attached = atomic_load_explicit(&service->guest_attached,
                                                  memory_order_acquire);
    status->sink_reachable = atomic_load_explicit(&service->sink_reachable,
                                                  memory_order_acquire);
}

static uint64_t monotonic_ms(void)
{
#if defined(ESP_PLATFORM)
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
#else
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
#endif
}

enum p4_nano_audio86_live_result p4_nano_audio86_5d3_fixture_configure(
    struct p4_nano_audio86_live_service *service,
    const struct p4_nano_audio86_5d3_hooks *hooks)
{
    if (service == NULL || hooks == NULL ||
        service->magic != P4_NANO_AUDIO86_LIVE_MAGIC ||
        hooks->decorate_render == NULL ||
        hooks->observe_rendered_pcm == NULL ||
        hooks->observe_applied_action == NULL ||
        hooks->terminal_publication_test_mode > 2U)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (service_state(service) != P4_NANO_AUDIO86_LIVE_READY)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    service->fixture_opaque = hooks->opaque;
    service->fixture_decorate_render = hooks->decorate_render;
    service->fixture_observe_rendered_pcm = hooks->observe_rendered_pcm;
    service->fixture_observe_applied_action =
        hooks->observe_applied_action;
    service->fixture_observe_ring = hooks->observe_ring;
    service->fixture_observe_terminal_point = hooks->observe_terminal_point;
    service->fixture_terminal_test_mode =
        hooks->terminal_publication_test_mode;
    if (service->fixture_decorate_render(service->fixture_opaque,
                                         &service->render, 0U) != 0) {
        service->fixture_opaque = NULL;
        service->fixture_decorate_render = NULL;
        service->fixture_observe_rendered_pcm = NULL;
        service->fixture_observe_applied_action = NULL;
        service->fixture_observe_ring = NULL;
        service->fixture_observe_terminal_point = NULL;
        return P4_NANO_AUDIO86_LIVE_FAILED;
    }
    service->fixture_enabled = 1U;
    return P4_NANO_AUDIO86_LIVE_OK;
}

enum p4_nano_audio86_live_result p4_nano_audio86_5d3_fixture_arm_terminal(
    struct p4_nano_audio86_live_service *service)
{
    uint64_t started;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (!owner_matches(service))
        return P4_NANO_AUDIO86_LIVE_OWNER_ERROR;
    if (service_state(service) != P4_NANO_AUDIO86_LIVE_RUNNING ||
        service->fixture_enabled == 0U || service->transaction_active != 0U ||
        atomic_load_explicit(&service->fixture_terminal_armed,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&service->fixture_terminal_horizon_published,
                             memory_order_acquire) != 0U)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    atomic_store_explicit(&service->fixture_terminal_armed, 1U,
                          memory_order_release);
    if (service->fixture_terminal_test_mode == 0U)
        return P4_NANO_AUDIO86_LIVE_OK;
    atomic_store_explicit(&service->fixture_worker_hold, 1U,
                          memory_order_release);
    wake_waiters(service);
    started = monotonic_ms();
    while (atomic_load_explicit(&service->fixture_worker_hold_ack,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&service->first_error_latched,
                                memory_order_acquire) == 0U) {
        if (monotonic_ms() - started >= 5000U) {
            (void)first_fatal(service,
                              P4_NANO_AUDIO86_LIVE_FAILURE_WORKER,
                              P4_NANO_AUDIO86_LIVE_ORIGIN_CHECKPOINT,
                              LIVE_SUBCODE_5D3_TERMINAL);
            return P4_NANO_AUDIO86_LIVE_FAILED;
        }
        owner_wait(service);
    }
    return atomic_load_explicit(&service->first_error_latched,
                                memory_order_acquire) == 0U
               ? P4_NANO_AUDIO86_LIVE_OK
               : P4_NANO_AUDIO86_LIVE_FAILED;
}

enum p4_nano_audio86_live_result
p4_nano_audio86_5d3_fixture_complete_producer(
    struct p4_nano_audio86_live_service *service)
{
    uint32_t expected = P4_NANO_AUDIO86_LIVE_RUNNING;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    if (!owner_matches(service))
        return P4_NANO_AUDIO86_LIVE_OWNER_ERROR;
    if (service_state(service) != P4_NANO_AUDIO86_LIVE_RUNNING ||
        service->fixture_enabled == 0U || service->transaction_active != 0U ||
        atomic_load_explicit(&service->fixture_terminal_pcm_ready,
                             memory_order_acquire) == 0U ||
        service->fixture_output_finished == 0U ||
        service->fixture_terminal_reset_ordinal == 0U ||
        atomic_load_explicit(&service->fixture_reset_applied_ordinal,
                             memory_order_acquire) !=
            service->fixture_terminal_reset_ordinal)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    atomic_store_explicit(&service->producer_closing, 1U,
                          memory_order_release);
    atomic_store_explicit(&service->transaction_gate,
                          LIVE_TRANSACTION_GATE_CLOSED,
                          memory_order_release);
    snapshot_u64_publish(&service->final_horizon,
                         P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON);
    np2audio86_guest_sink_unbind();
    atomic_store_explicit(&service->guest_attached, 0U,
                          memory_order_release);
    np2audio86_runtime_producer_done_publish(&service->control);
    if (!atomic_compare_exchange_strong_explicit(
            &service->state, &expected, P4_NANO_AUDIO86_LIVE_DRAINING,
            memory_order_acq_rel, memory_order_acquire)) {
        wake_waiters(service);
        return P4_NANO_AUDIO86_LIVE_FAILED;
    }
    wake_waiters(service);
    return P4_NANO_AUDIO86_LIVE_OK;
}

void p4_nano_audio86_5d3_fixture_snapshot(
    const struct p4_nano_audio86_live_service *service,
    struct p4_nano_audio86_5d3_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return;
    snapshot->rendered_frames = snapshot_u64_read(
        &service->rendered_frames_published);
    snapshot->accepted_frames = snapshot_u64_read(
        &service->accepted_frames_published);
    snapshot->final_horizon = snapshot_u64_read(&service->final_horizon);
    snapshot->ring_next_frame_offset = service->pcm_ring.next_frame_offset;
    snapshot->ring_next_sequence = service->pcm_ring.next_sequence;
    snapshot->ring_occupancy =
        np2opngen_pcm_ring_occupancy(&service->pcm_ring);
    snapshot->ring_partial_frames =
        np2opngen_pcm_ring_producer_partial_valid_frames(&service->pcm_ring);
    snapshot->reset_ordinal = service->reset_ordinal;
    snapshot->reset_applied_ordinal = atomic_load_explicit(
        &service->fixture_reset_applied_ordinal, memory_order_acquire);
    snapshot->terminal_horizon_published = atomic_load_explicit(
        &service->fixture_terminal_horizon_published, memory_order_acquire);
    snapshot->terminal_horizon_observed = atomic_load_explicit(
        &service->fixture_terminal_horizon_observed, memory_order_acquire);
    snapshot->terminal_pcm_ready = atomic_load_explicit(
        &service->fixture_terminal_pcm_ready, memory_order_acquire);
    snapshot->terminal_pcm_before_producer_done = atomic_load_explicit(
        &service->fixture_terminal_pcm_before_done, memory_order_acquire);
    snapshot->reset_event_before_terminal_horizon =
        service->fixture_reset_event_before_horizon;
    snapshot->worker_observed_matching_pair =
        service->fixture_worker_observed_pair;
    snapshot->reset_before_post_reset_render =
        service->fixture_reset_before_remainder;
    snapshot->q399_published = service->fixture_q399_published;
    snapshot->output_finished = service->fixture_output_finished;
    snapshot->producer_done =
        np2audio86_runtime_producer_done(&service->control) ? 1U : 0U;
    snapshot->guest_attached = atomic_load_explicit(
        &service->guest_attached, memory_order_acquire);
    snapshot->first_error = atomic_load_explicit(
        &service->first_error_latched, memory_order_acquire);
    snapshot->transport_residual =
        np2audio86_event_ring_occupancy(&service->events) +
        np2audio86_byte_ring_occupancy(&service->bytes) +
        (np2audio86_runtime_horizon_pending(&service->control) ? 1U : 0U);
    snapshot->worker_hold_ack = atomic_load_explicit(
        &service->fixture_worker_hold_ack, memory_order_acquire);
    snapshot->partial_failure_event_visible =
        service->fixture_partial_failure_event_visible;
    snapshot->partial_failure_wake_issued =
        service->fixture_partial_failure_wake_issued;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_join(
    struct p4_nano_audio86_live_service *service, uint32_t timeout_ms,
    struct p4_nano_audio86_live_status *status)
{
    uint64_t started;
    enum p4_nano_audio86_live_state state;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    state = service_state(service);
    if (state != P4_NANO_AUDIO86_LIVE_STOP_REQUESTED &&
        state != P4_NANO_AUDIO86_LIVE_DRAINING &&
        state != P4_NANO_AUDIO86_LIVE_FAILING && !state_terminal(state))
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    started = monotonic_ms();
    while (!state_terminal(state)) {
        if (monotonic_ms() - started >= timeout_ms) {
            p4_nano_audio86_live_service_status(service, status);
            return P4_NANO_AUDIO86_LIVE_TIMEOUT;
        }
        owner_wait(service);
        state = service_state(service);
    }
#if !defined(ESP_PLATFORM)
    if (service->worker_joined == 0U) {
        (void)pthread_join(service->worker_thread, NULL);
        service->worker_joined = 1U;
    }
#endif
    p4_nano_audio86_live_service_status(service, status);
    if (state == P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT)
        return P4_NANO_AUDIO86_LIVE_OK;
    return state == P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT
               ? P4_NANO_AUDIO86_LIVE_FAILED
               : P4_NANO_AUDIO86_LIVE_NOT_QUIESCENT;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_destroy(
    struct p4_nano_audio86_live_service *service)
{
    enum p4_nano_audio86_live_state state;
    if (service == NULL || service->magic != P4_NANO_AUDIO86_LIVE_MAGIC)
        return P4_NANO_AUDIO86_LIVE_ARGUMENT;
    state = service_state(service);
    if (state != P4_NANO_AUDIO86_LIVE_READY &&
        state != P4_NANO_AUDIO86_LIVE_STOPPED_QUIESCENT &&
        state != P4_NANO_AUDIO86_LIVE_FAILED_QUIESCENT)
        return P4_NANO_AUDIO86_LIVE_STATE_ERROR;
    if (state != P4_NANO_AUDIO86_LIVE_READY &&
        atomic_load_explicit(&service->worker_safe,
                             memory_order_acquire) == 0U)
        return P4_NANO_AUDIO86_LIVE_NOT_QUIESCENT;
    if (atomic_load_explicit(&service->guest_attached,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&service->sink_reachable,
                             memory_order_acquire) != 0U)
        return P4_NANO_AUDIO86_LIVE_NOT_QUIESCENT;
#if defined(ESP_PLATFORM)
    if (service->worker_task != NULL) {
        vTaskDelete(service->worker_task);
        service->worker_task = NULL;
        atomic_store_explicit(&service->worker_handle_ready, 0U,
                              memory_order_release);
    }
#else
    if (state != P4_NANO_AUDIO86_LIVE_READY && service->worker_joined == 0U) {
        (void)pthread_join(service->worker_thread, NULL);
        service->worker_joined = 1U;
    }
#endif
    np2audio86_core_render_destroy(&service->render);
    release_borrow(service);
    memset(&service->guest_sink, 0, sizeof(service->guest_sink));
    atomic_store_explicit(&service->state, P4_NANO_AUDIO86_LIVE_DESTROYED,
                          memory_order_release);
#if !defined(ESP_PLATFORM)
    (void)pthread_cond_destroy(&service->wait_cond);
    (void)pthread_mutex_destroy(&service->wait_mutex);
#endif
    return P4_NANO_AUDIO86_LIVE_OK;
}

#if defined(P4_NANO_AUDIO86_LIVE_SERVICE_TESTING)
void p4_nano_audio86_live_service_test_set_authorization_hook(
    struct p4_nano_audio86_live_service *service,
    void (*hook)(struct p4_nano_audio86_live_service *, void *), void *opaque)
{
    if (service == NULL)
        return;
    service->authorization_hook = hook;
    service->authorization_hook_opaque = opaque;
}

void p4_nano_audio86_live_service_test_set_before_clean_terminal_hook(
    struct p4_nano_audio86_live_service *service,
    void (*hook)(struct p4_nano_audio86_live_service *, void *), void *opaque)
{
    if (service == NULL)
        return;
    service->before_clean_terminal_hook = hook;
    service->before_clean_terminal_hook_opaque = opaque;
}

enum p4_nano_audio86_live_result p4_nano_audio86_live_service_test_fail(
    struct p4_nano_audio86_live_service *service,
    enum p4_nano_audio86_live_failure_category category,
    enum p4_nano_audio86_live_failure_origin origin, uint32_t subcode)
{
    return first_fatal(service, category, origin, subcode)
               ? P4_NANO_AUDIO86_LIVE_OK
               : P4_NANO_AUDIO86_LIVE_ALREADY;
}
#endif
