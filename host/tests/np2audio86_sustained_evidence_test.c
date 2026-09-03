#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2_crc32.h"
#include "np2audio86_guest_adapter.h"
#include "np2audio86_sustained_evidence.h"

#define TEST_QUANTA 400U
#define TEST_FRAMES (TEST_QUANTA * NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES)

static void make_slot(uint8_t *pcm, uint32_t sequence)
{
    size_t i;
    for (i = 0U; i < NP2_AUDIO86_SUSTAINED_SLOT_BYTES; ++i)
        pcm[i] = (uint8_t)((sequence * 29U + i * 17U + (i >> 3U)) & 0xffU);
}

static int digest_equal(const np2audio86_sustained_digest *left,
                        const np2audio86_sustained_digest *right)
{
    uint32_t left_crc, right_crc;
    uint8_t left_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t right_sha[NP2_SHA256_DIGEST_SIZE];
    np2audio86_sustained_digest_snapshot(left, &left_crc, left_sha);
    np2audio86_sustained_digest_snapshot(right, &right_crc, right_sha);
    return left->bytes == right->bytes && left->records == right->records &&
           left_crc == right_crc && memcmp(left_sha, right_sha,
                                           sizeof(left_sha)) == 0;
}

static int test_400_q240_and_retry(void)
{
    np2audio86_sustained_evidence evidence;
    np2audio86_sustained_digest accepted_before;
    uint8_t pcm[NP2_AUDIO86_SUSTAINED_SLOT_BYTES];
    uint8_t changed[NP2_AUDIO86_SUSTAINED_SLOT_BYTES];
    uint32_t sequence;
    np2audio86_sustained_evidence_init(&evidence);
    np2audio86_sustained_stream_start(&evidence, 1000U);
    for (sequence = 0U; sequence < TEST_QUANTA; ++sequence) {
        uint64_t offset = (uint64_t)sequence *
            NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES;
        make_slot(pcm, sequence);
        if (sequence == 399U) {
            if (np2audio86_sustained_generated(&evidence, sequence, offset,
                                               pcm, 1U) != 0)
                return -1;
            np2audio86_sustained_freeze_reset(&evidence, 95761U, 18U, 1U,
                                              95761U, 1U, 1U);
            if (np2audio86_sustained_generated(&evidence, sequence, offset + 1U,
                                               pcm + 4U, 239U) != 0)
                return -1;
        } else if (np2audio86_sustained_generated(
                       &evidence, sequence, offset, pcm,
                       NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES) != 0) {
            return -1;
        }
        if (sequence == 0U || sequence == 200U || sequence == 399U) {
            accepted_before = evidence.accepted;
            if (np2audio86_sustained_submit(
                    &evidence, NP2_AUDIO86_SUSTAINED_RETRY, sequence, offset,
                    pcm, NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES, 1U,
                    1005U + sequence * 5U) != 0 ||
                !digest_equal(&accepted_before, &evidence.accepted) ||
                evidence.next_accepted_sequence != sequence ||
                evidence.next_accepted_frame_offset != offset)
                return -1;
            if (sequence == 200U) {
                if (np2audio86_sustained_submit(
                        &evidence, NP2_AUDIO86_SUSTAINED_RETRY, sequence,
                        offset, pcm, NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES,
                        1U, 2010U) != 0)
                    return -1;
                memcpy(changed, pcm, sizeof(changed));
                changed[511] ^= 1U;
                if (np2audio86_sustained_submit(
                        &evidence, NP2_AUDIO86_SUSTAINED_ACCEPTED, sequence,
                        offset, changed, NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES,
                        1U, 2010U) == 0 ||
                    evidence.retry_identity_failures != 1U ||
                    evidence.next_accepted_sequence != sequence)
                    return -1;
            }
        }
        if (np2audio86_sustained_submit(
                &evidence, NP2_AUDIO86_SUSTAINED_ACCEPTED, sequence, offset,
                pcm, NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES, 1U,
                1005U + sequence * 5U) != 0)
            return -1;
    }
    np2audio86_sustained_drain_complete(&evidence, 3040U);
    if (evidence.next_generated_sequence != TEST_QUANTA ||
        evidence.generated_slot_fill_frames != 0U ||
        evidence.next_generated_frame_offset != TEST_FRAMES ||
        evidence.next_accepted_sequence != TEST_QUANTA ||
        evidence.next_accepted_frame_offset != TEST_FRAMES ||
        evidence.accepted.bytes != (uint64_t)TEST_FRAMES * 4U ||
        evidence.accepted.records != TEST_QUANTA ||
        evidence.retry_attempts != 4U || evidence.retry_pending != 0U ||
        !digest_equal(&evidence.generated, &evidence.accepted) ||
        !evidence.first_accepted.present ||
        evidence.first_accepted.sequence != 0U ||
        evidence.first_accepted.frame_offset != 0U ||
        !evidence.final_accepted.present ||
        evidence.final_accepted.sequence != 399U ||
        evidence.final_accepted.frame_offset != 95760U ||
        !evidence.reset.frozen || evidence.reset.frames != 95761U ||
        evidence.reset.bytes != 383044U ||
        evidence.reset.reset_event_sequence != 18U ||
        !evidence.reset.applied_after_ring || !evidence.reset.ack_after_apply ||
        np2audio86_sustained_stream_wall_ms(&evidence) != 2040U)
        return -1;
    return 0;
}

static int test_trace_rollover(void)
{
    static const size_t capacities[] = {128U, 64U, 8U, 64U, 32U};
    uint32_t window[128U];
    size_t c;
    for (c = 0U; c < sizeof(capacities) / sizeof(capacities[0]); ++c) {
        const size_t capacity = capacities[c];
        const uint32_t records = (uint32_t)capacity + 19U;
        const size_t first = capacity / 2U;
        const size_t last = capacity - first;
        np2audio86_sustained_evidence evidence;
        uint32_t i;
        size_t start;
        memset(window, 0, sizeof(window));
        np2audio86_sustained_evidence_init(&evidence);
        for (i = 0U; i < records; ++i) {
            uint8_t canonical[4] = {
                (uint8_t)i, (uint8_t)(i >> 8U), 0U, 0U};
            window[np2audio86_guest_trace_window_index(i, capacity)] = i;
            np2audio86_sustained_trace_record(
                &evidence, NP2_AUDIO86_SUSTAINED_TRACE_IO, canonical,
                sizeof(canonical));
        }
        for (i = 0U; i < first; ++i)
            if (window[i] != i) return -1;
        if (evidence.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].records != records ||
            evidence.trace[NP2_AUDIO86_SUSTAINED_TRACE_IO].bytes !=
                (uint64_t)records * 4U)
            return -1;
        start = np2audio86_guest_trace_window_last_start(records, capacity);
        for (i = 0U; i < last; ++i) {
            const size_t index = first + ((start - first + i) % last);
            if (window[index] != records - last + i) return -1;
        }
    }
    return 0;
}

static int test_pressure_and_progress(void)
{
    np2audio86_sustained_evidence evidence;
    uint8_t pcm[NP2_AUDIO86_SUSTAINED_SLOT_BYTES] = {0U};
    np2audio86_sustained_evidence_init(&evidence);
    np2audio86_sustained_observe_ring(&evidence, 3U);
    np2audio86_sustained_observe_ring(&evidence, 8U);
    np2audio86_sustained_observe_ring(&evidence, 2U);
    np2audio86_sustained_producer_full(&evidence, 1U);
    np2audio86_sustained_producer_full(&evidence, 1U);
    np2audio86_sustained_producer_full(&evidence, 0U);
    np2audio86_sustained_producer_full(&evidence, 1U);
    np2audio86_sustained_consumer_empty(&evidence, 0U, 0U);
    np2audio86_sustained_consumer_empty(&evidence, 1U, 0U);
    np2audio86_sustained_consumer_empty(&evidence, 1U, 1U);
    np2audio86_sustained_stream_start(&evidence, 100U);
    if (np2audio86_sustained_submit(
            &evidence, NP2_AUDIO86_SUSTAINED_ACCEPTED, 0U, 0U, pcm, 240U,
            1U, 105U) != 0 ||
        np2audio86_sustained_submit(
            &evidence, NP2_AUDIO86_SUSTAINED_RETRY, 1U, 240U, pcm, 240U,
            1U, 130U) != 0 ||
        np2audio86_sustained_submit(
            &evidence, NP2_AUDIO86_SUSTAINED_ACCEPTED, 1U, 240U, pcm, 240U,
            1U, 145U) != 0 ||
        evidence.max_running_accept_gap_ms != 40U ||
        !evidence.max_running_gap_present || evidence.max_running_gap_initial ||
        !evidence.max_running_gap_previous_sequence_valid ||
        evidence.max_running_gap_previous_sequence != 0U ||
        evidence.max_running_gap_next_sequence != 1U ||
        evidence.max_running_gap_previous_relative_ms != 5U ||
        evidence.max_running_gap_next_relative_ms != 45U ||
        evidence.retry_episode_units != 1U ||
        evidence.direct_running_accept_units != 1U)
        return -1;
    np2audio86_sustained_drain_complete(&evidence, 300U);
    if (evidence.max_running_accept_gap_ms != 40U ||
        evidence.pcm_ring_max_occupancy != 8U ||
        evidence.pcm_producer_full_wait_count != 2U ||
        evidence.pcm_consumer_premature_empty_count != 1U ||
        NP2_AUDIO86_SUSTAINED_PROGRESS_BOUND_MS != 40U ||
        np2audio86_sustained_worker_wait_ms(0U) != 5000U ||
        np2audio86_sustained_worker_wait_ms(1U) != 5001U ||
        np2audio86_sustained_worker_wait_ms(96000U) != 7000U)
        return -1;
    return 0;
}

static int test_diagnostic_maxima(void)
{
    np2audio86_sustained_evidence evidence;
    np2audio86_sustained_evidence_init(&evidence);
    np2audio86_sustained_observe_downstream_submit(&evidence, 7U, 12U);
    np2audio86_sustained_observe_downstream_submit(&evidence, 8U, 12U);
    if (!evidence.max_downstream_submit_present ||
        evidence.max_downstream_submit_us != 12U ||
        evidence.max_downstream_submit_sequence != 7U)
        return -1;
    np2audio86_sustained_observe_downstream_submit(&evidence, 9U, 19U);
    if (evidence.max_downstream_submit_us != 19U ||
        evidence.max_downstream_submit_sequence != 9U)
        return -1;
    np2audio86_sustained_observe_post_accept_evidence(&evidence, 3U, 21U);
    np2audio86_sustained_observe_post_accept_evidence(&evidence, 4U, 20U);
    if (!evidence.max_post_accept_evidence_present ||
        evidence.max_post_accept_evidence_us != 21U ||
        evidence.max_post_accept_evidence_sequence != 3U)
        return -1;
    np2audio86_sustained_observe_post_accept_evidence(&evidence, 5U, 22U);
    return evidence.max_post_accept_evidence_us == 22U &&
           evidence.max_post_accept_evidence_sequence == 5U ? 0 : -1;
}

struct fake_cooperative_clock {
    uint64_t now_us;
    uint32_t delays;
    uint64_t guest_cycles;
};

static uint64_t fake_monotonic_us(void *opaque)
{
    return ((struct fake_cooperative_clock *)opaque)->now_us;
}

static void fake_delay_one_tick(void *opaque)
{
    struct fake_cooperative_clock *clock = opaque;
    ++clock->delays;
    clock->now_us += 1000U;
}

static int test_cooperative_scheduler(void)
{
    struct fake_cooperative_clock clock = {1000U, 0U, 123456U};
    np2audio86_sustained_cooperative_scheduler scheduler;
    if (np2audio86_sustained_cooperative_scheduler_init(
            &scheduler, fake_monotonic_us, fake_delay_one_tick, &clock) != 0)
        return -1;
    clock.now_us = 250999U;
    if (np2audio86_sustained_cooperative_checkpoint(&scheduler) != 0 ||
        clock.delays != 0U || clock.guest_cycles != 123456U)
        return -1;
    clock.now_us = 251000U;
    if (np2audio86_sustained_cooperative_checkpoint(&scheduler) != 1 ||
        clock.delays != 1U || scheduler.slice_started_us != 252000U ||
        clock.guest_cycles != 123456U)
        return -1;
    clock.now_us = 502000U;
    if (np2audio86_sustained_cooperative_checkpoint(&scheduler) != 1 ||
        clock.delays != 2U || scheduler.slice_started_us != 503000U ||
        clock.guest_cycles != 123456U)
        return -1;
    clock.now_us = 502999U;
    return np2audio86_sustained_cooperative_checkpoint(&scheduler) == -1
        ? 0 : -1;
}

static int test_generated_render_boundaries(void)
{
    np2audio86_sustained_evidence evidence;
    uint8_t pcm[NP2_AUDIO86_SUSTAINED_SLOT_BYTES] = {0U};
    np2audio86_sustained_evidence_init(&evidence);
    if (np2audio86_sustained_generated(&evidence, 0U, 0U, pcm, 13U) != 0 ||
        np2audio86_sustained_generated(&evidence, 0U, 13U, pcm, 240U) != 0 ||
        evidence.next_generated_sequence != 1U ||
        evidence.next_generated_frame_offset != 253U ||
        evidence.generated_slot_fill_frames != 13U ||
        np2audio86_sustained_generated(&evidence, 1U, 253U, pcm, 227U) != 0 ||
        evidence.next_generated_sequence != 2U ||
        evidence.next_generated_frame_offset != 480U ||
        evidence.generated_slot_fill_frames != 0U)
        return -1;
    return 0;
}

int main(void)
{
    if (test_400_q240_and_retry() != 0) {
        fprintf(stderr, "400-q240/retry test failed\n");
        return 1;
    }
    if (test_trace_rollover() != 0) {
        fprintf(stderr, "trace rollover test failed\n");
        return 1;
    }
    if (test_pressure_and_progress() != 0) {
        fprintf(stderr, "pressure/progress test failed\n");
        return 1;
    }
    if (test_generated_render_boundaries() != 0) {
        fprintf(stderr, "generated render-boundary test failed\n");
        return 1;
    }
    if (test_diagnostic_maxima() != 0) {
        fprintf(stderr, "diagnostic maxima test failed\n");
        return 1;
    }
    if (test_cooperative_scheduler() != 0) {
        fprintf(stderr, "cooperative scheduler test failed\n");
        return 1;
    }
    printf("SUSTAINED_HOST_400_Q240_EVIDENCE=PASS\n");
    printf("SUSTAINED_HOST_RETRY_DIGEST_NONREGRESSION=PASS\n");
    printf("SUSTAINED_TRACE_WINDOW_ROLLOVER=PASS\n");
    printf("SUSTAINED_RING_PRESSURE_TELEMETRY_TEST=PASS\n");
    printf("SUSTAINED_PROGRESS_METRIC_TEST=PASS\n");
    printf("DIAGNOSTIC_MAXIMA_HOST_TEST=PASS\n");
    printf("SUSTAINED_GUEST_COOPERATIVE_SCHEDULER_TEST=PASS\n");
    printf("SUSTAINED_EVIDENCE_FIXED_BYTES=%zu\n",
           sizeof(np2audio86_sustained_evidence));
    return 0;
}
