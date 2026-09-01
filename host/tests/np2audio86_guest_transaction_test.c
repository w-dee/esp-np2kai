#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_guest_adapter.h"
#include "np2audio86_guest_program.h"
#include "np2_crc32.h"

struct transactional_sink {
    uint8_t allow_event;
    uint8_t allow_byte;
    uint8_t allow_horizon;
    uint8_t reject_after_reserve;
    uint8_t reserved;
    uint32_t reserve_calls;
    uint32_t cancel_calls;
    uint32_t event_commits;
    uint32_t byte_commits;
    uint32_t run_commits;
    uint32_t horizon_commits;
    np2audio86_guest_event_t last_event;
    np2audio86_guest_data_run_t last_run;
};

static int reserve(void *opaque, uint32_t kind, size_t bytes,
                   np2audio86_guest_transaction_t *transaction)
{
    struct transactional_sink *sink = opaque;
    if (sink == NULL || transaction == NULL || sink->reserved ||
        !sink->allow_horizon ||
        ((kind == NP2AUDIO86_GUEST_TRANSACTION_EVENT ||
          kind == NP2AUDIO86_GUEST_TRANSACTION_RESET) && !sink->allow_event) ||
        (kind == NP2AUDIO86_GUEST_TRANSACTION_DATA_RUN &&
         (!sink->allow_event || bytes != 1U || !sink->allow_byte))) return -1;
    ++sink->reserve_calls;
    sink->reserved = 1U;
    transaction->opaque[0] = (uintptr_t)sink;
    transaction->opaque[1] = kind;
    return 0;
}

static int extend(void *opaque, np2audio86_guest_transaction_t *transaction,
                  size_t bytes)
{
    struct transactional_sink *sink = opaque;
    return sink == NULL || transaction == NULL || !sink->reserved ||
           transaction->opaque[0] != (uintptr_t)sink || bytes != 1U ||
           !sink->allow_byte ? -1 : 0;
}

static int recheck(void *opaque, const np2audio86_guest_transaction_t *transaction)
{
    const struct transactional_sink *sink = opaque;
    return sink == NULL || transaction == NULL || !sink->reserved ||
           sink->reject_after_reserve ? -1 : 0;
}

static void cancel(void *opaque, np2audio86_guest_transaction_t *transaction)
{
    struct transactional_sink *sink = opaque;
    assert(sink != NULL && transaction != NULL && sink->reserved);
    sink->reserved = 0U;
    ++sink->cancel_calls;
}

static void commit_event(void *opaque, np2audio86_guest_transaction_t *transaction,
                         const np2audio86_guest_event_t *event)
{
    struct transactional_sink *sink = opaque;
    assert(sink != NULL && transaction != NULL && event != NULL && sink->reserved);
    sink->last_event = *event;
    ++sink->event_commits;
}

static void commit_byte(void *opaque, np2audio86_guest_transaction_t *transaction,
                        uint64_t frame, uint64_t sequence, uint8_t value)
{
    struct transactional_sink *sink = opaque;
    (void)frame; (void)sequence; (void)value;
    assert(sink != NULL && transaction != NULL && sink->reserved);
    ++sink->byte_commits;
}

static void commit_run(void *opaque, np2audio86_guest_transaction_t *transaction,
                       const np2audio86_guest_data_run_t *run)
{
    struct transactional_sink *sink = opaque;
    assert(sink != NULL && transaction != NULL && run != NULL && sink->reserved);
    sink->last_run = *run;
    ++sink->run_commits;
}

static void commit_horizon(void *opaque, np2audio86_guest_transaction_t *transaction,
                           uint64_t frame)
{
    struct transactional_sink *sink = opaque;
    (void)frame;
    assert(sink != NULL && transaction != NULL && sink->reserved);
    sink->reserved = 0U;
    ++sink->horizon_commits;
}

static const np2audio86_guest_sink_t k_sink = {
    .reserve = reserve,
    .extend = extend,
    .recheck = recheck,
    .cancel = cancel,
    .commit_event = commit_event,
    .commit_pcm_byte = commit_byte,
    .commit_data_run = commit_run,
    .commit_horizon = commit_horizon,
};

static void reset_adapter(struct transactional_sink *sink)
{
    static np2audio86_guest_sink_t bound;
    memset(sink, 0, sizeof(*sink));
    sink->allow_event = 1U;
    sink->allow_byte = 1U;
    sink->allow_horizon = 1U;
    np2audio86_guest_sink_unbind();
    np2audio86_guest_opna_unbind();
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    np2audio86_guest_opna_bind();
    bound = k_sink;
    bound.opaque = sink;
    np2audio86_guest_sink_bind(&bound);
}

static void snapshot(np2audio86_guest_state_snapshot_t *state)
{
    memset(state, 0, sizeof(*state));
    np2audio86_guest_host_snapshot(state);
}

int main(void)
{
    struct transactional_sink sink;
    np2audio86_guest_state_snapshot_t before, after;
    uint8_t program[8192];
    size_t program_bytes;

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x24u);
    snapshot(&before);
    sink.allow_event = 0U;
    np2audio86_guest_opna_write_data_low(0xffu);
    snapshot(&after);
    assert(before.sequence == after.sequence && before.timer_a_value == after.timer_a_value);
    assert(sink.event_commits == 0U && sink.horizon_commits == 0U);
    sink.allow_event = 1U;
    np2audio86_guest_opna_write_data_low(0xffu);
    assert(sink.event_commits == 1U && sink.horizon_commits == 1U);

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x25u);
    snapshot(&before);
    sink.allow_horizon = 0U;
    np2audio86_guest_opna_write_data_low(3u);
    snapshot(&after);
    assert(before.sequence == after.sequence && before.timer_a_value == after.timer_a_value);
    assert(sink.event_commits == 0U);
    sink.allow_horizon = 1U;
    np2audio86_guest_opna_write_data_low(3u);
    assert(sink.event_commits == 1U && sink.horizon_commits == 1U);

    reset_adapter(&sink);
    np2audio86_guest_opna_write_address_low(0x26u);
    snapshot(&before);
    sink.reject_after_reserve = 1U;
    np2audio86_guest_opna_write_data_low(0x7fu);
    snapshot(&after);
    assert(before.sequence == after.sequence && before.timer_b_value == after.timer_b_value);
    assert(sink.cancel_calls == 1U && sink.event_commits == 0U);

    reset_adapter(&sink);
    snapshot(&before);
    sink.allow_byte = 0U;
    np2audio86_guest_pcm86_write_data(0x10u);
    snapshot(&after);
    assert(before.sequence == after.sequence &&
           before.pcm_virtual_buffer == after.pcm_virtual_buffer &&
           before.pcm_write_position == after.pcm_write_position);
    assert(sink.byte_commits == 0U && sink.run_commits == 0U);
    sink.allow_byte = 1U;
    np2audio86_guest_pcm86_write_data(0x10u);
    snapshot(&before);
    sink.allow_byte = 0U;
    np2audio86_guest_pcm86_write_data(0x20u);
    snapshot(&after);
    assert(before.sequence == after.sequence &&
           before.pcm_virtual_buffer == after.pcm_virtual_buffer &&
           before.pcm_write_position == after.pcm_write_position);
    sink.allow_byte = 1U;
    np2audio86_guest_pcm86_write_data(0x20u);
    np2audio86_guest_host_flush_data_run();
    assert(sink.byte_commits == 2U && sink.run_commits == 1U &&
           sink.last_run.count == 2U && sink.last_run.sequence == before.sequence - 1U &&
           sink.horizon_commits == 1U);

    reset_adapter(&sink);
    np2audio86_guest_pcm86_write(0x0au, 0x0fu);
    assert(sink.reserve_calls == 0U && sink.event_commits == 0U);
    assert(!np2audio86_guest_host_failed());

    reset_adapter(&sink);
    assert(sink.event_commits == 0U && sink.horizon_commits == 0U);
    np2audio86_guest_opna_reset(NP2AUDIO86_OPNA_CAPS_2608 |
                                NP2AUDIO86_OPNA_CAPS_TIMER,
                                0x10u, 1u, 2u);
    assert(sink.event_commits == 1U && sink.last_event.payload == 0U &&
           sink.horizon_commits == 1U);

    program_bytes = np2audio86_guest_program_build(program, sizeof(program));
    assert(program_bytes == 4971U);
    assert(np2_crc32_iso_hdlc(program, program_bytes) == UINT32_C(0x544b2e8c));
    printf("AUDIO86_TRANSACTION_PROGRAM_BYTES=%zu\n", program_bytes);
    printf("AUDIO86_TRANSACTION_PROGRAM_CRC32=%08" PRIx32 "\n",
           np2_crc32_iso_hdlc(program, program_bytes));
    printf("EVENT_PREFLIGHT_BEFORE_GUEST_MUTATION=PASS\n");
    printf("HORIZON_PREFLIGHT_BEFORE_GUEST_MUTATION=PASS\n");
    printf("DATA_RUN_MIDRUN_PREFLIGHT=PASS\n");
    printf("PREMUTATION_TRANSACTION_CANCEL=PASS\n");
    printf("POST_MUTATION_COMMIT_FAILURE_PATH=NONE\n");
    printf("AUDIO86_GUEST_TRANSACTION_RESULT=PASS\n");
    return 0;
}
