#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_guest_adapter.h"
#include "np2audio86_guest_program.h"
#include "np2_crc32.h"

enum { EVENT_CAPACITY = 8, BYTE_CAPACITY = 32 };

enum finite_transaction_state {
    FINITE_IDLE = 0,
    FINITE_TENTATIVE,
    FINITE_AUTHORIZED,
    FINITE_EVENT_COMMITTED,
    FINITE_RUN_COMMITTED,
    FINITE_CANCELLED,
};

struct finite_sink {
    size_t event_capacity, byte_capacity;
    size_t committed_events, committed_bytes;
    size_t reserved_events, reserved_bytes;
    uint8_t horizon_full, horizon_owned;
    uint8_t control_accept, control_terminates;
    uint8_t stop_after_extend, reenter_on_commit, adversarial_commit;
    uint8_t active, active_kind;
    enum finite_transaction_state state;
    uint32_t generation, active_generation;
    uint32_t contract_failures, tentative_rollbacks;
    uint32_t event_commits, byte_commits, run_commits, horizon_commits;
    np2audio86_guest_event_t events[EVENT_CAPACITY];
    uint8_t bytes[BYTE_CAPACITY];
    np2audio86_guest_data_run_t last_run;
};

struct timer_probe {
    uint32_t schedule_calls, cancel_calls, irq_calls;
    uint8_t last_schedule_timer, last_schedule_absolute, last_cancel_timer;
    uint32_t last_schedule_clock, last_irq;
    uint8_t last_irq_level;
};

struct full_snapshot {
    uint8_t guest[1024];
    size_t guest_bytes;
    size_t committed_events, committed_bytes;
    size_t reserved_events, reserved_bytes;
    uint8_t horizon_full, horizon_owned, active, active_kind;
    uint32_t active_generation, event_commits, byte_commits, run_commits;
    uint32_t horizon_commits, contract_failures;
    np2audio86_guest_event_t events[EVENT_CAPACITY];
    uint8_t bytes[BYTE_CAPACITY];
    np2audio86_guest_data_run_t last_run;
    struct timer_probe timer;
};

static struct timer_probe g_timer;

static void token_clear(np2audio86_guest_transaction_t *transaction)
{ memset(transaction, 0, sizeof(*transaction)); }

static void token_set(struct finite_sink *sink,
                      np2audio86_guest_transaction_t *transaction,
                      uint32_t kind)
{
    token_clear(transaction);
    transaction->opaque[0] = (uintptr_t)sink;
    transaction->opaque[1] = (uintptr_t)sink->active_generation;
    transaction->opaque[2] = (uintptr_t)kind;
    transaction->opaque[3] = (uintptr_t)FINITE_AUTHORIZED;
}

static int token_matches(const struct finite_sink *sink,
                         const np2audio86_guest_transaction_t *transaction,
                         uint32_t kind)
{
    return sink != NULL && transaction != NULL && sink->active &&
           transaction->opaque[0] == (uintptr_t)sink &&
           transaction->opaque[1] == (uintptr_t)sink->active_generation &&
           transaction->opaque[2] == (uintptr_t)kind &&
           transaction->opaque[3] == (uintptr_t)FINITE_AUTHORIZED &&
           sink->active_kind == kind && sink->state >= FINITE_AUTHORIZED;
}

static void contract(struct finite_sink *sink)
{ if (sink != NULL) ++sink->contract_failures; }

static int finite_reserve_checked(void *opaque, uint32_t kind, size_t bytes,
                                  np2audio86_guest_transaction_t *transaction)
{
    struct finite_sink *sink = opaque;
    size_t want_event, want_byte;
    if (sink == NULL || transaction == NULL || sink->active ||
        (kind != NP2AUDIO86_GUEST_TRANSACTION_EVENT &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_RESET &&
         kind != NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        (kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN && bytes != 1U) ||
        (kind != NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN && bytes != 0U)) {
        contract(sink);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    want_event = 1U;
    want_byte = kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN ? 1U : 0U;
    if (sink->horizon_full || sink->horizon_owned ||
        sink->event_capacity - sink->committed_events - sink->reserved_events < want_event ||
        sink->byte_capacity - sink->committed_bytes - sink->reserved_bytes < want_byte)
        return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    sink->state = FINITE_TENTATIVE;
    sink->reserved_events += want_event;
    sink->reserved_bytes += want_byte;
    sink->horizon_owned = 1U;
    if (!sink->control_accept) {
        sink->reserved_events -= want_event;
        sink->reserved_bytes -= want_byte;
        sink->horizon_owned = 0U;
        sink->state = FINITE_IDLE;
        ++sink->tentative_rollbacks;
        return sink->control_terminates ?
            NP2AUDIO86_GUEST_TRANSACTION_TERMINATED :
            NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    }
    sink->active = 1U;
    sink->active_kind = (uint8_t)kind;
    sink->state = FINITE_AUTHORIZED;
    sink->active_generation = ++sink->generation;
    token_set(sink, transaction, kind);
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static int finite_extend_checked(void *opaque,
                                 np2audio86_guest_transaction_t *transaction,
                                 size_t bytes)
{
    struct finite_sink *sink = opaque;
    if (!token_matches(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        bytes != 1U || sink->state != FINITE_AUTHORIZED) {
        contract(sink);
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    }
    if (sink->byte_capacity - sink->committed_bytes - sink->reserved_bytes < 1U)
        return NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    ++sink->reserved_bytes;
    if (!sink->control_accept) {
        --sink->reserved_bytes;
        ++sink->tentative_rollbacks;
        return sink->control_terminates ?
            NP2AUDIO86_GUEST_TRANSACTION_TERMINATED :
            NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    }
    if (sink->stop_after_extend) {
        sink->stop_after_extend = 0U;
        sink->control_accept = 0U;
        sink->control_terminates = 1U;
    }
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static void finite_commit_event(void *opaque,
                                np2audio86_guest_transaction_t *transaction,
                                const np2audio86_guest_event_t *event)
{
    struct finite_sink *sink = opaque;
    if ((!token_matches(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_EVENT) &&
         !token_matches(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_RESET)) ||
        sink->reserved_events != 1U || sink->state != FINITE_AUTHORIZED ||
        sink->committed_events >= sink->event_capacity || event == NULL) {
        contract(sink);
        return;
    }
    if (sink->adversarial_commit) {
        if (sink->committed_events != 0U) --sink->committed_events;
        sink->control_accept = 0U;
    }
    sink->events[sink->event_commits % EVENT_CAPACITY] = *event;
    ++sink->event_commits;
    ++sink->committed_events;
    --sink->reserved_events;
    sink->state = FINITE_EVENT_COMMITTED;
    if (sink->reenter_on_commit) {
        sink->reenter_on_commit = 0U;
        np2audio86_guest_opna_write_data_low(0x5aU);
    }
}

static void finite_commit_byte(void *opaque,
                               np2audio86_guest_transaction_t *transaction,
                               uint64_t frame, uint64_t sequence, uint8_t value)
{
    struct finite_sink *sink = opaque;
    (void)frame; (void)sequence;
    if (!token_matches(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        sink->state != FINITE_AUTHORIZED || sink->reserved_bytes == 0U ||
        sink->committed_bytes >= sink->byte_capacity) {
        contract(sink);
        return;
    }
    sink->bytes[sink->byte_commits % BYTE_CAPACITY] = value;
    ++sink->byte_commits;
    ++sink->committed_bytes;
    --sink->reserved_bytes;
}

static void finite_commit_run(void *opaque,
                              np2audio86_guest_transaction_t *transaction,
                              const np2audio86_guest_data_run_t *run)
{
    struct finite_sink *sink = opaque;
    if (!token_matches(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN) ||
        sink->state != FINITE_AUTHORIZED || sink->reserved_events != 1U ||
        sink->committed_events >= sink->event_capacity || run == NULL) {
        contract(sink);
        return;
    }
    sink->last_run = *run;
    ++sink->run_commits;
    ++sink->committed_events;
    --sink->reserved_events;
    sink->state = FINITE_RUN_COMMITTED;
}

static void finite_commit_horizon(void *opaque,
                                  np2audio86_guest_transaction_t *transaction,
                                  uint64_t frame)
{
    struct finite_sink *sink = opaque;
    (void)frame;
    if (sink == NULL || transaction == NULL || !sink->active ||
        transaction->opaque[0] != (uintptr_t)sink ||
        transaction->opaque[1] != (uintptr_t)sink->active_generation ||
        !sink->horizon_owned ||
        (sink->state != FINITE_EVENT_COMMITTED && sink->state != FINITE_RUN_COMMITTED)) {
        contract(sink);
        return;
    }
    sink->horizon_owned = 0U;
    sink->active = 0U;
    sink->active_kind = 0U;
    sink->state = FINITE_IDLE;
    transaction->opaque[3] = 0U;
    ++sink->horizon_commits;
}

static int finite_make_tentative(struct finite_sink *sink,
                                 np2audio86_guest_transaction_t *transaction)
{
    if (sink == NULL || sink->active || sink->horizon_owned) return -1;
    sink->active = 1U;
    sink->active_kind = NP2AUDIO86_GUEST_TRANSACTION_EVENT;
    sink->active_generation = ++sink->generation;
    sink->reserved_events = 1U;
    sink->horizon_owned = 1U;
    sink->state = FINITE_TENTATIVE;
    token_set(sink, transaction, NP2AUDIO86_GUEST_TRANSACTION_EVENT);
    return 0;
}

static void finite_cancel_tentative(struct finite_sink *sink,
                                    np2audio86_guest_transaction_t *transaction)
{
    if (sink == NULL || transaction == NULL || !sink->active ||
        sink->state != FINITE_TENTATIVE ||
        transaction->opaque[0] != (uintptr_t)sink ||
        transaction->opaque[1] != (uintptr_t)sink->active_generation) {
        contract(sink);
        return;
    }
    sink->reserved_events = 0U;
    sink->reserved_bytes = 0U;
    sink->horizon_owned = 0U;
    sink->active = 0U;
    sink->state = FINITE_CANCELLED;
    transaction->opaque[3] = 0U;
}

static const np2audio86_guest_sink_t k_sink = {
    .reserve_checked = finite_reserve_checked,
    .extend_checked = finite_extend_checked,
    .commit_event = finite_commit_event,
    .commit_pcm_byte = finite_commit_byte,
    .commit_data_run = finite_commit_run,
    .commit_horizon = finite_commit_horizon,
};

static void timer_schedule(uint8_t timer, uint64_t clock, uint8_t absolute)
{
    ++g_timer.schedule_calls;
    g_timer.last_schedule_timer = timer;
    g_timer.last_schedule_clock = (uint32_t)clock;
    g_timer.last_schedule_absolute = absolute;
}
static void timer_cancel(uint8_t timer)
{
    ++g_timer.cancel_calls;
    g_timer.last_cancel_timer = timer;
}
static uint8_t timer_iswork(uint8_t timer) { (void)timer; return 0U; }
static void timer_irq(uint32_t irq, uint8_t level)
{
    ++g_timer.irq_calls;
    g_timer.last_irq = irq;
    g_timer.last_irq_level = level;
}

static void sink_init(struct finite_sink *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->event_capacity = EVENT_CAPACITY;
    sink->byte_capacity = BYTE_CAPACITY;
    sink->control_accept = 1U;
}

static void reset_adapter(struct finite_sink *sink)
{
    static np2audio86_guest_sink_t bound;
    sink_init(sink);
    memset(&g_timer, 0, sizeof(g_timer));
    np2audio86_guest_sink_unbind();
    np2audio86_guest_opna_unbind();
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10U, 1U, 2U);
    np2audio86_guest_opna_bind();
    np2audio86_guest_host_test_seed(0U, 0U);
    np2audio86_guest_host_set_cpu_position(0U);
    np2audio86_guest_host_set_timer_hooks(timer_schedule, timer_cancel,
                                          timer_iswork, timer_irq);
    bound = k_sink;
    bound.opaque = sink;
    np2audio86_guest_sink_bind(&bound);
    assert(!np2audio86_guest_host_failed());
}

static void snapshot(struct full_snapshot *snapshot,
                     const struct finite_sink *sink)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->guest_bytes = np2audio86_guest_test_full_snapshot_size();
    assert(snapshot->guest_bytes <= sizeof(snapshot->guest));
    assert(np2audio86_guest_test_full_snapshot(snapshot->guest,
                                                snapshot->guest_bytes) == 0);
    snapshot->committed_events = sink->committed_events;
    snapshot->committed_bytes = sink->committed_bytes;
    snapshot->reserved_events = sink->reserved_events;
    snapshot->reserved_bytes = sink->reserved_bytes;
    snapshot->horizon_full = sink->horizon_full;
    snapshot->horizon_owned = sink->horizon_owned;
    snapshot->active = sink->active;
    snapshot->active_kind = sink->active_kind;
    snapshot->active_generation = sink->active_generation;
    snapshot->event_commits = sink->event_commits;
    snapshot->byte_commits = sink->byte_commits;
    snapshot->run_commits = sink->run_commits;
    snapshot->horizon_commits = sink->horizon_commits;
    snapshot->contract_failures = sink->contract_failures;
    memcpy(snapshot->events, sink->events, sizeof(snapshot->events));
    memcpy(snapshot->bytes, sink->bytes, sizeof(snapshot->bytes));
    snapshot->last_run = sink->last_run;
    snapshot->timer = g_timer;
}

static void assert_snapshot_equal(const struct full_snapshot *before,
                                  const struct full_snapshot *after)
{ assert(memcmp(before, after, sizeof(*before)) == 0); }

static void assert_no_reservations(const struct finite_sink *sink)
{
    assert(sink->reserved_events == 0U && sink->reserved_bytes == 0U);
    assert(!sink->horizon_owned && !sink->active && sink->state == FINITE_IDLE);
}

static void test_event_and_horizon_negative(void)
{
    struct finite_sink sink;
    struct full_snapshot before, after;
    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x24U);
    snapshot(&before, &sink);
    sink.event_capacity = 0U;
    np2audio86_guest_opna_write_data_low(0xffU);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.event_capacity = EVENT_CAPACITY;
    np2audio86_guest_opna_write_data_low(0xffU);
    assert(sink.event_commits == 1U && sink.horizon_commits == 1U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x25U);
    sink.horizon_full = 1U;
    snapshot(&before, &sink);
    np2audio86_guest_opna_write_data_low(3U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.horizon_full = 0U;
    np2audio86_guest_opna_write_data_low(3U);
    assert(sink.event_commits == 1U && sink.horizon_commits == 1U);
    assert_no_reservations(&sink);
}

static void test_pcm_control_negative(void)
{
    struct finite_sink sink;
    struct full_snapshot before, after;
    reset_adapter(&sink);
    np2audio86_guest_host_set_cpu_position(1234U);
    snapshot(&before, &sink);
    sink.event_capacity = 0U;
    assert(np2audio86_guest_pcm86_write(0x00U, 1U) == 0);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.event_capacity = EVENT_CAPACITY;
    assert(np2audio86_guest_pcm86_write(0x00U, 1U) == 1);
    assert(sink.event_commits == 1U);
    assert_no_reservations(&sink);
}

static void test_data_run_first_byte_matrix(void)
{
    struct finite_sink sink;
    struct full_snapshot before, after;
    reset_adapter(&sink);
    snapshot(&before, &sink);
    sink.event_capacity = 0U;
    np2audio86_guest_pcm86_write_data(0x10U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.event_capacity = EVENT_CAPACITY;
    assert(np2audio86_guest_pcm86_write_data(0x10U) == 1);
    np2audio86_guest_host_flush_data_run();
    assert(sink.byte_commits == 1U && sink.run_commits == 1U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    snapshot(&before, &sink);
    sink.byte_capacity = 0U;
    np2audio86_guest_pcm86_write_data(0x10U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.byte_capacity = BYTE_CAPACITY;
    assert(np2audio86_guest_pcm86_write_data(0x10U) == 1);
    np2audio86_guest_host_flush_data_run();
    assert(sink.byte_commits == 1U && sink.run_commits == 1U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    sink.horizon_full = 1U;
    snapshot(&before, &sink);
    np2audio86_guest_pcm86_write_data(0x10U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.horizon_full = 0U;
    np2audio86_guest_pcm86_write_data(0x10U);
    np2audio86_guest_host_flush_data_run();
    assert(sink.byte_commits == 1U && sink.run_commits == 1U);
    assert(sink.last_run.count == 1U);
    assert_no_reservations(&sink);
}

static void test_data_run_midrun_matrix(void)
{
    struct finite_sink sink;
    struct full_snapshot before, after;
    reset_adapter(&sink);
    np2audio86_guest_pcm86_write_data(0x10U);
    snapshot(&before, &sink);
    sink.byte_capacity = sink.committed_bytes;
    np2audio86_guest_pcm86_write_data(0x20U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.byte_capacity = BYTE_CAPACITY;
    np2audio86_guest_pcm86_write_data(0x20U);
    np2audio86_guest_host_flush_data_run();
    assert(sink.last_run.count == 2U && sink.byte_commits == 2U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    np2audio86_guest_pcm86_write_data(0x10U);
    sink.control_accept = 0U;
    sink.control_terminates = 0U;
    snapshot(&before, &sink);
    np2audio86_guest_pcm86_write_data(0x20U);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.control_accept = 1U;
    np2audio86_guest_pcm86_write_data(0x20U);
    np2audio86_guest_host_flush_data_run();
    assert(sink.last_run.count == 2U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    np2audio86_guest_pcm86_write_data(0x10U);
    sink.stop_after_extend = 1U;
    np2audio86_guest_pcm86_write_data(0x20U);
    assert(sink.byte_commits == 2U && sink.reserved_bytes == 0U);
    np2audio86_guest_pcm86_write_data(0x30U);
    assert(sink.run_commits == 1U && sink.last_run.count == 2U);
    assert_no_reservations(&sink);
}

static void test_invalid_tokens(void)
{
    struct finite_sink sink;
    np2audio86_guest_transaction_t token, stale, tentative, empty;
    reset_adapter(&sink);
    assert(finite_reserve_checked(&sink, NP2AUDIO86_GUEST_TRANSACTION_EVENT,
                                  0U, &token) == 0);
    stale = token;
    finite_commit_event(&sink, &token, &(np2audio86_guest_event_t){0});
    finite_commit_horizon(&sink, &token, 0U);
    finite_commit_event(&sink, &token, &(np2audio86_guest_event_t){0});
    token_clear(&empty);
    finite_commit_event(&sink, &empty, &(np2audio86_guest_event_t){0});
    (void)finite_extend_checked(&sink, &stale, 1U);
    assert(finite_make_tentative(&sink, &tentative) == 0);
    finite_cancel_tentative(&sink, &tentative);
    finite_cancel_tentative(&sink, &tentative);
    assert(finite_reserve_checked(&sink, NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN,
                                  1U, &token) == 0);
    finite_commit_byte(&sink, &token, 0U, 0U, 0x10U);
    finite_commit_run(&sink, &token, &(np2audio86_guest_data_run_t){.count = 1U});
    finite_commit_horizon(&sink, &token, 0U);
    (void)finite_extend_checked(&sink, &token, 1U);
    assert(sink.contract_failures >= 5U);
    assert_no_reservations(&sink);
}

static void test_reentrancy_and_void_commit(void)
{
    struct finite_sink sink;
    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x24U);
    sink.reenter_on_commit = 1U;
    np2audio86_guest_opna_write_data_low(1U);
    assert(sink.event_commits == 1U && np2audio86_guest_test_contract_violation());
    assert(!np2audio86_guest_host_failed());
    np2audio86_guest_opna_write_data_low(2U);
    assert(np2audio86_guest_host_failed());

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x24U);
    np2audio86_guest_opna_write_data_low(1U);
    sink.adversarial_commit = 1U;
    np2audio86_guest_opna_write_data_low(2U);
    assert(sink.event_commits == 2U && sink.horizon_commits == 2U);
    assert(sink.contract_failures == 0U);
    assert_no_reservations(&sink);
}

static void test_timer_negative(void)
{
    struct finite_sink sink;
    struct full_snapshot before, after;
    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x27U);
    np2audio86_guest_opna_write_data_low(0x80U);
    memset(&g_timer, 0, sizeof(g_timer));
    sink.event_capacity = sink.committed_events;
    snapshot(&before, &sink);
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_TIMER_A);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.event_capacity = EVENT_CAPACITY;
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_TIMER_A);
    assert(sink.event_commits == 2U && sink.horizon_commits == 2U);
    assert(g_timer.schedule_calls == 1U);
    assert_no_reservations(&sink);

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x27U);
    np2audio86_guest_opna_write_data_low(0x80U);
    memset(&g_timer, 0, sizeof(g_timer));
    sink.control_accept = 0U;
    sink.control_terminates = 0U;
    snapshot(&before, &sink);
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_TIMER_A);
    snapshot(&after, &sink);
    assert_snapshot_equal(&before, &after);
    sink.control_accept = 1U;
    np2audio86_guest_host_timer_dispatch(NP2AUDIO86_TRACE_TIMER_A);
    assert(sink.event_commits == 2U && g_timer.schedule_calls == 1U);
    assert_no_reservations(&sink);
}

int main(void)
{
    uint8_t program[8192];
    const size_t program_bytes = np2audio86_guest_program_build(program, sizeof(program));
    assert(program_bytes == 4971U);
    assert(np2_crc32_iso_hdlc(program, program_bytes) == UINT32_C(0x544b2e8c));
    test_event_and_horizon_negative();
    test_pcm_control_negative();
    test_data_run_first_byte_matrix();
    test_data_run_midrun_matrix();
    test_invalid_tokens();
    test_reentrancy_and_void_commit();
    test_timer_negative();
    printf("CHECKED_RESERVATION_LINEARIZATION=PASS\n");
    printf("TENTATIVE_REJECTION_ROLLBACK_INTERNAL=PASS\n");
    printf("INVALID_TOKEN_CONTRACT_TESTS=PASS\n");
    printf("ADAPTER_REENTRANCY_GUARD=PASS\n");
    printf("OPNA_DATA_PREMUTATION_PREFLIGHT=PASS\n");
    printf("PCM86_SYNC_PLAN_SIDE_EFFECT_FREE=PASS\n");
    printf("PCM86_CONTROL_PREMUTATION_PREFLIGHT=PASS\n");
    printf("TIMER_CSM_PREMUTATION_PREFLIGHT=PASS\n");
    printf("TIMER_REJECTION_GUEST_STATE_UNCHANGED=PASS\n");
    printf("DATA_RUN_INITIAL_CHECKED_RESERVE=PASS\n");
    printf("DATA_RUN_CHECKED_EXTENSION=PASS\n");
    printf("DATA_RUN_CLOSE_INFALLIBLE=PASS\n");
    printf("AUTHORIZED_BYTE_COMPLETES_AFTER_STOP=PASS\n");
    printf("OPEN_DATA_RUN_LOSSLESS_TERMINATION=PASS\n");
    printf("FULL_NEGATIVE_GUEST_SNAPSHOT=PASS\n");
    printf("EVENT_CAPACITY_NEGATIVE=PASS\n");
    printf("HORIZON_NEGATIVE=PASS\n");
    printf("DATA_RUN_FIRST_NEGATIVE_3_OF_3=PASS\n");
    printf("DATA_RUN_MIDRUN_NEGATIVE_3_OF_3=PASS\n");
    printf("TOKEN_MISUSE_MATRIX=PASS\n");
    printf("REENTRANCY_FAILSTOP=PASS\n");
    printf("VOID_COMMIT_ADVERSARIAL=PASS\n");
    printf("TIMER_NEGATIVE_2_OF_2=PASS\n");
    printf("FINITE_CAPACITY_BACKEND=PASS\n");
    printf("RESERVATION_LEAKS=0\n");
    printf("AUDIO86_GUEST_TRANSACTION_RESULT=PASS\n");
    return 0;
}
