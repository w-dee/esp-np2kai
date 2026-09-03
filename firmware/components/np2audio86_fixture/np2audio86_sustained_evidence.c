#include "np2audio86_sustained_evidence.h"

#include <limits.h>
#include <string.h>

#include "np2_crc32.h"

static uint32_t slot_crc32(const uint8_t *pcm, size_t bytes)
{
    return np2_crc32_iso_hdlc_finish(np2_crc32_iso_hdlc_update(
        np2_crc32_iso_hdlc_init(), pcm, bytes));
}

void np2audio86_sustained_digest_init(np2audio86_sustained_digest *digest)
{
    if (digest == NULL) return;
    memset(digest, 0, sizeof(*digest));
    digest->crc32_running = np2_crc32_iso_hdlc_init();
    np2_sha256_init(&digest->sha256);
}

void np2audio86_sustained_digest_update(np2audio86_sustained_digest *digest,
                                        const uint8_t *bytes, size_t length,
                                        uint64_t records)
{
    if (digest == NULL || (bytes == NULL && length != 0U) ||
        UINT64_MAX - digest->bytes < length ||
        UINT64_MAX - digest->records < records)
        return;
    digest->crc32_running = np2_crc32_iso_hdlc_update(
        digest->crc32_running, bytes, length);
    np2_sha256_update(&digest->sha256, bytes, length);
    digest->bytes += length;
    digest->records += records;
}

void np2audio86_sustained_digest_snapshot(
    const np2audio86_sustained_digest *digest, uint32_t *crc32,
    uint8_t sha256[NP2_SHA256_DIGEST_SIZE])
{
    np2_sha256_context copy;
    if (digest == NULL) return;
    if (crc32 != NULL)
        *crc32 = np2_crc32_iso_hdlc_finish(digest->crc32_running);
    if (sha256 != NULL) {
        copy = digest->sha256;
        np2_sha256_final(&copy, sha256);
    }
}

void np2audio86_sustained_evidence_init(
    np2audio86_sustained_evidence *evidence)
{
    size_t i;
    if (evidence == NULL) return;
    memset(evidence, 0, sizeof(*evidence));
    np2audio86_sustained_digest_init(&evidence->generated);
    np2audio86_sustained_digest_init(&evidence->accepted);
    np2audio86_sustained_digest_init(&evidence->pre_reset);
    for (i = 0U; i < NP2_AUDIO86_SUSTAINED_TRACE_STREAM_COUNT; ++i)
        np2audio86_sustained_digest_init(&evidence->trace[i]);
}

int np2audio86_sustained_generated(
    np2audio86_sustained_evidence *evidence, uint32_t sequence,
    uint64_t frame_offset, const uint8_t *pcm, uint16_t valid_frames)
{
    size_t bytes;
    if (evidence == NULL || pcm == NULL || valid_frames == 0U ||
        valid_frames > NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES ||
        sequence != evidence->next_generated_sequence ||
        frame_offset != evidence->next_generated_frame_offset)
        return -1;
    bytes = (size_t)valid_frames * NP2_AUDIO86_SUSTAINED_BYTES_PER_FRAME;
    np2audio86_sustained_digest_update(&evidence->generated, pcm, bytes, 0U);
    if (!evidence->reset.frozen)
        np2audio86_sustained_digest_update(&evidence->pre_reset, pcm, bytes, 0U);
    evidence->next_generated_frame_offset += valid_frames;
    evidence->generated_slot_fill_frames = (uint16_t)(
        evidence->generated_slot_fill_frames + valid_frames);
    if (evidence->generated_slot_fill_frames >=
        NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES) {
        evidence->generated_slot_fill_frames = (uint16_t)(
            evidence->generated_slot_fill_frames -
            NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES);
        ++evidence->generated.records;
        ++evidence->next_generated_sequence;
    }
    return 0;
}

static int retry_identity_matches(const np2audio86_sustained_evidence *evidence,
                                  uint32_t sequence, uint64_t frame_offset,
                                  const uint8_t *pcm, uint16_t valid_frames,
                                  uint32_t crc32)
{
    const size_t bytes =
        (size_t)valid_frames * NP2_AUDIO86_SUSTAINED_BYTES_PER_FRAME;
    return evidence->retry_sequence == sequence &&
           evidence->retry_frame_offset == frame_offset &&
           evidence->retry_valid_frames == valid_frames &&
           evidence->retry_crc32 == crc32 &&
           memcmp(evidence->retry_pcm, pcm, bytes) == 0;
}

int np2audio86_sustained_submit(
    np2audio86_sustained_evidence *evidence,
    enum np2audio86_sustained_submit_result result, uint32_t sequence,
    uint64_t frame_offset, const uint8_t *pcm, uint16_t valid_frames,
    uint8_t sink_running, uint64_t now_ms)
{
    size_t bytes;
    uint32_t crc32;
    np2audio86_sustained_slot_fingerprint slot;
    if (evidence == NULL || pcm == NULL || valid_frames == 0U ||
        valid_frames > NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES ||
        sequence != evidence->next_accepted_sequence ||
        frame_offset != evidence->next_accepted_frame_offset)
        return -1;
    bytes = (size_t)valid_frames * NP2_AUDIO86_SUSTAINED_BYTES_PER_FRAME;
    crc32 = slot_crc32(pcm, bytes);
    if (evidence->retry_pending &&
        !retry_identity_matches(evidence, sequence, frame_offset, pcm,
                                valid_frames, crc32)) {
        ++evidence->retry_identity_failures;
        return -1;
    }
    if (result == NP2_AUDIO86_SUSTAINED_RETRY) {
        if (!evidence->retry_pending) {
            evidence->retry_pending = 1U;
            evidence->retry_sequence = sequence;
            evidence->retry_frame_offset = frame_offset;
            evidence->retry_valid_frames = valid_frames;
            evidence->retry_crc32 = crc32;
            memcpy(evidence->retry_pcm, pcm, bytes);
        }
        ++evidence->retry_attempts;
        return 0;
    }
    if (result != NP2_AUDIO86_SUSTAINED_ACCEPTED) return -1;
    if (sink_running) {
        if (evidence->retry_pending)
            ++evidence->retry_episode_units;
        else
            ++evidence->direct_running_accept_units;
    }
    np2audio86_sustained_digest_update(&evidence->accepted, pcm, bytes, 1U);
    slot.present = 1U;
    slot.sequence = sequence;
    slot.frame_offset = frame_offset;
    slot.valid_frames = valid_frames;
    slot.crc32 = crc32;
    if (!evidence->first_accepted.present) evidence->first_accepted = slot;
    evidence->final_accepted = slot;
    evidence->next_accepted_frame_offset += valid_frames;
    ++evidence->next_accepted_sequence;
    evidence->retry_pending = 0U;
    if (sink_running && evidence->stream_started) {
        const uint64_t relative_ms = now_ms >= evidence->stream_started_ms
            ? now_ms - evidence->stream_started_ms : 0U;
        const uint64_t previous_relative_ms = evidence->running_accept_seen
            ? evidence->last_running_accept_relative_ms : 0U;
        const uint64_t gap = relative_ms >= previous_relative_ms
            ? relative_ms - previous_relative_ms : 0U;
        if (!evidence->max_running_gap_present ||
            gap > evidence->max_running_accept_gap_ms) {
            evidence->max_running_accept_gap_ms = gap;
            evidence->max_running_gap_present = 1U;
            evidence->max_running_gap_initial =
                evidence->running_accept_seen ? 0U : 1U;
            evidence->max_running_gap_previous_sequence_valid =
                evidence->running_accept_seen ? 1U : 0U;
            evidence->max_running_gap_previous_sequence =
                evidence->last_running_accept_sequence;
            evidence->max_running_gap_next_sequence = sequence;
            evidence->max_running_gap_previous_relative_ms =
                previous_relative_ms;
            evidence->max_running_gap_next_relative_ms = relative_ms;
        }
        evidence->last_running_accept_ms = now_ms;
        evidence->last_running_accept_relative_ms = relative_ms;
        evidence->last_running_accept_sequence = sequence;
        evidence->running_accept_sequence_valid = 1U;
        evidence->running_accept_seen = 1U;
    }
    return 0;
}

void np2audio86_sustained_freeze_reset(
    np2audio86_sustained_evidence *evidence, uint64_t reset_event_frame,
    uint32_t reset_event_sequence, uint32_t reset_ordinal,
    uint64_t ring_next_frame_offset, uint8_t applied_after_ring,
    uint8_t ack_after_apply)
{
    if (evidence == NULL || evidence->reset.frozen) return;
    evidence->reset.frozen = 1U;
    evidence->reset.frames = evidence->pre_reset.bytes /
        NP2_AUDIO86_SUSTAINED_BYTES_PER_FRAME;
    evidence->reset.bytes = evidence->pre_reset.bytes;
    np2audio86_sustained_digest_snapshot(&evidence->pre_reset,
                                         &evidence->reset.crc32,
                                         evidence->reset.sha256);
    evidence->reset.reset_event_frame = reset_event_frame;
    evidence->reset.reset_event_sequence = reset_event_sequence;
    evidence->reset.reset_ordinal = reset_ordinal;
    evidence->reset.ring_next_frame_offset = ring_next_frame_offset;
    evidence->reset.applied_after_ring = applied_after_ring;
    evidence->reset.ack_after_apply = ack_after_apply;
}

void np2audio86_sustained_trace_record(
    np2audio86_sustained_evidence *evidence,
    enum np2audio86_sustained_trace_stream stream,
    const uint8_t *canonical, size_t canonical_bytes)
{
    if (evidence == NULL || stream >= NP2_AUDIO86_SUSTAINED_TRACE_STREAM_COUNT)
        return;
    np2audio86_sustained_digest_update(&evidence->trace[stream], canonical,
                                       canonical_bytes, 1U);
}

void np2audio86_sustained_observe_ring(
    np2audio86_sustained_evidence *evidence, uint32_t occupancy)
{
    if (evidence != NULL && occupancy > evidence->pcm_ring_max_occupancy)
        evidence->pcm_ring_max_occupancy = occupancy;
}

void np2audio86_sustained_producer_full(
    np2audio86_sustained_evidence *evidence, uint8_t full)
{
    if (evidence == NULL) return;
    if (full && !evidence->producer_full_wait_active) {
        ++evidence->pcm_producer_full_wait_count;
        evidence->producer_full_wait_active = 1U;
    } else if (!full) {
        evidence->producer_full_wait_active = 0U;
    }
}

void np2audio86_sustained_consumer_empty(
    np2audio86_sustained_evidence *evidence, uint8_t released,
    uint8_t production_done)
{
    if (evidence != NULL && released && !production_done)
        ++evidence->pcm_consumer_premature_empty_count;
}

void np2audio86_sustained_stream_start(
    np2audio86_sustained_evidence *evidence, uint64_t now_ms)
{
    if (evidence == NULL) return;
    evidence->stream_started = 1U;
    evidence->stream_started_ms = now_ms;
    evidence->last_running_accept_ms = now_ms;
}

void np2audio86_sustained_drain_complete(
    np2audio86_sustained_evidence *evidence, uint64_t now_ms)
{
    if (evidence == NULL || !evidence->stream_started) return;
    evidence->drain_completed = 1U;
    evidence->drain_completed_ms = now_ms;
}

void np2audio86_sustained_observe_downstream_submit(
    np2audio86_sustained_evidence *evidence, uint32_t sequence,
    uint32_t duration_us)
{
    if (evidence == NULL) return;
    if (!evidence->max_downstream_submit_present ||
        duration_us > evidence->max_downstream_submit_us) {
        evidence->max_downstream_submit_present = 1U;
        evidence->max_downstream_submit_us = duration_us;
        evidence->max_downstream_submit_sequence = sequence;
    }
}

void np2audio86_sustained_observe_post_accept_evidence(
    np2audio86_sustained_evidence *evidence, uint32_t sequence,
    uint32_t duration_us)
{
    if (evidence == NULL) return;
    if (!evidence->max_post_accept_evidence_present ||
        duration_us > evidence->max_post_accept_evidence_us) {
        evidence->max_post_accept_evidence_present = 1U;
        evidence->max_post_accept_evidence_us = duration_us;
        evidence->max_post_accept_evidence_sequence = sequence;
    }
}

uint64_t np2audio86_sustained_stream_wall_ms(
    const np2audio86_sustained_evidence *evidence)
{
    if (evidence == NULL || !evidence->stream_started ||
        !evidence->drain_completed ||
        evidence->drain_completed_ms < evidence->stream_started_ms)
        return 0U;
    return evidence->drain_completed_ms - evidence->stream_started_ms;
}

uint32_t np2audio86_sustained_worker_wait_ms(uint64_t remaining_frames)
{
    uint64_t duration_ms;
    const uint64_t whole = remaining_frames / 48000U;
    const uint64_t remainder = remaining_frames % 48000U;
    if (whole > UINT64_MAX / 1000U) return UINT32_MAX;
    duration_ms = whole * 1000U;
    duration_ms += (remainder * 1000U + 47999U) / 48000U;
    if (duration_ms > UINT32_MAX - 5000U) return UINT32_MAX;
    return (uint32_t)(5000U + duration_ms);
}

int np2audio86_sustained_cooperative_scheduler_init(
    np2audio86_sustained_cooperative_scheduler *scheduler,
    np2audio86_sustained_monotonic_us_fn monotonic_us,
    np2audio86_sustained_delay_one_tick_fn delay_one_tick, void *opaque)
{
    if (scheduler == NULL || monotonic_us == NULL || delay_one_tick == NULL)
        return -1;
    scheduler->monotonic_us = monotonic_us;
    scheduler->delay_one_tick = delay_one_tick;
    scheduler->opaque = opaque;
    scheduler->slice_started_us = monotonic_us(opaque);
    return 0;
}

int np2audio86_sustained_cooperative_checkpoint(
    np2audio86_sustained_cooperative_scheduler *scheduler)
{
    uint64_t now_us;
    uint64_t resumed_us;
    if (scheduler == NULL || scheduler->monotonic_us == NULL ||
        scheduler->delay_one_tick == NULL)
        return -1;
    now_us = scheduler->monotonic_us(scheduler->opaque);
    if (now_us < scheduler->slice_started_us) return -1;
    if (now_us - scheduler->slice_started_us <
        NP2_AUDIO86_SUSTAINED_COOPERATIVE_SLICE_US)
        return 0;
    scheduler->delay_one_tick(scheduler->opaque);
    resumed_us = scheduler->monotonic_us(scheduler->opaque);
    if (resumed_us < now_us) return -1;
    scheduler->slice_started_us = resumed_us;
    return 1;
}
