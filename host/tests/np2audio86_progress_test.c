#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_guest_adapter.h"

struct progress_probe {
    uint64_t last_frame;
    uint32_t progress_count;
    uint32_t event_count;
    uint32_t data_run_count;
    uint32_t pcm_byte_count;
    int forced_status;
    uint8_t valid;
};

static int reserve_checked(void *opaque, uint32_t kind, size_t bytes,
                           np2audio86_guest_transaction_t *transaction)
{
    (void)opaque;
    (void)kind;
    (void)bytes;
    memset(transaction, 0, sizeof(*transaction));
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static int extend_checked(void *opaque,
                          np2audio86_guest_transaction_t *transaction,
                          size_t bytes)
{
    (void)opaque;
    (void)transaction;
    (void)bytes;
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static void commit_event(void *opaque,
                         np2audio86_guest_transaction_t *transaction,
                         const np2audio86_guest_event_t *event)
{
    struct progress_probe *probe = opaque;
    (void)transaction;
    (void)event;
    ++probe->event_count;
}

static void commit_pcm_byte(void *opaque,
                            np2audio86_guest_transaction_t *transaction,
                            uint64_t frame, uint64_t sequence, uint8_t value)
{
    struct progress_probe *probe = opaque;
    (void)transaction;
    (void)frame;
    (void)sequence;
    (void)value;
    ++probe->pcm_byte_count;
}

static void commit_data_run(void *opaque,
                            np2audio86_guest_transaction_t *transaction,
                            const np2audio86_guest_data_run_t *run)
{
    struct progress_probe *probe = opaque;
    (void)transaction;
    (void)run;
    ++probe->data_run_count;
}

static void commit_horizon(void *opaque,
                           np2audio86_guest_transaction_t *transaction,
                           uint64_t frame)
{
    (void)opaque;
    (void)transaction;
    (void)frame;
}

static int publish_progress_checked(void *opaque, uint64_t frame)
{
    struct progress_probe *probe = opaque;
    if (probe->forced_status != 0)
        return probe->forced_status;
    if (probe->valid && frame < probe->last_frame)
        return NP2AUDIO86_GUEST_TRANSACTION_CONTRACT;
    probe->last_frame = frame;
    probe->valid = 1U;
    ++probe->progress_count;
    return NP2AUDIO86_GUEST_TRANSACTION_OK;
}

static np2audio86_guest_sink_t make_sink(struct progress_probe *probe)
{
    const np2audio86_guest_sink_t sink = {
        probe, reserve_checked, extend_checked, commit_event, commit_pcm_byte,
        commit_data_run, commit_horizon, publish_progress_checked};
    return sink;
}

int main(void)
{
    struct progress_probe probe = {0};
    np2audio86_guest_sink_t sink = make_sink(&probe);
    np2audio86_guest_state_snapshot_t snapshot;
    np2audio86_guest_state_snapshot_t before_observe;
    uint64_t observed_cycles;
    uint64_t observed_frame;

    np2audio86_guest_host_test_seed(0U, 0U);
    np2audio86_guest_host_set_cpu_position(0U);
    np2audio86_guest_sink_bind(&sink);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_OK);
    assert(probe.last_frame == 0U);

    /* Five guest seconds in one O(1) owner checkpoint. */
    np2audio86_guest_host_set_cpu_position(245760000U);
    np2audio86_guest_host_snapshot(&before_observe);
    assert(np2audio86_guest_progress_observe(&observed_cycles,
                                             &observed_frame) == 0);
    assert(observed_cycles == 245760000U);
    assert(observed_frame == 240000U);
    np2audio86_guest_host_snapshot(&snapshot);
    assert(memcmp(&before_observe, &snapshot, sizeof(snapshot)) == 0);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_OK);
    assert(probe.last_frame == 240000U);
    assert(probe.progress_count == 2U);
    assert(probe.event_count == 0U);
    assert(probe.data_run_count == 0U);
    assert(probe.pcm_byte_count == 0U);
    np2audio86_guest_host_snapshot(&snapshot);
    assert(snapshot.frame_timestamp == 240000U);

    /* Equal checkpoints are legal and bounded. */
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_OK);
    assert(probe.last_frame == 240000U);

    /* Retry preserves both producer state and the last published horizon. */
    probe.forced_status = NP2AUDIO86_GUEST_TRANSACTION_RETRY;
    np2audio86_guest_host_set_cpu_position(245761024U);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_RETRY);
    np2audio86_guest_host_snapshot(&snapshot);
    assert(snapshot.frame_timestamp == 240000U);
    assert(probe.last_frame == 240000U);
    probe.forced_status = 0;
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_OK);
    assert(probe.last_frame == 240001U);

    /* A sink-reported backward/contract violation is fatal, not ignored. */
    np2audio86_guest_host_test_seed(10U, 0U);
    np2audio86_guest_host_set_cpu_position(0U);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_CONTRACT);
    assert(np2audio86_guest_host_failed());

    /* Overflow is rejected before publication. */
    memset(&probe, 0, sizeof(probe));
    sink = make_sink(&probe);
    np2audio86_guest_host_test_seed(UINT64_MAX, 0U);
    np2audio86_guest_host_set_cpu_position(0U);
    np2audio86_guest_sink_bind(&sink);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_OK);
    np2audio86_guest_host_set_cpu_position(1024U);
    assert(np2audio86_guest_progress_checkpoint() ==
           NP2AUDIO86_GUEST_TRANSACTION_CONTRACT);
    assert(np2audio86_guest_host_failed());

    np2audio86_guest_sink_unbind();
    printf("PROGRESS_ONLY_HORIZON_LONG_INTERVAL_FRAME=%" PRIu64 "\n",
           UINT64_C(240000));
    printf("PROGRESS_ONLY_HORIZON_EVENT_COUNT=0\n");
    printf("PROGRESS_ONLY_HORIZON_TEST=PASS\n");
    return 0;
}
