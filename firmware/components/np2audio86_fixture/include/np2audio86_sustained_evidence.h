/* Fixed-memory evidence for the sustained real-guest Audio 86 profile. */
#ifndef NP2AUDIO86_SUSTAINED_EVIDENCE_H
#define NP2AUDIO86_SUSTAINED_EVIDENCE_H

#include <stddef.h>
#include <stdint.h>

#include "np2_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP2_AUDIO86_SUSTAINED_BYTES_PER_FRAME 4U
#define NP2_AUDIO86_SUSTAINED_QUANTUM_FRAMES 240U
#define NP2_AUDIO86_SUSTAINED_SLOT_BYTES 960U
#define NP2_AUDIO86_SUSTAINED_RING_SLOTS 8U
#define NP2_AUDIO86_SUSTAINED_QUANTUM_MS 5U
#define NP2_AUDIO86_SUSTAINED_PROGRESS_BOUND_MS \
    (NP2_AUDIO86_SUSTAINED_RING_SLOTS * NP2_AUDIO86_SUSTAINED_QUANTUM_MS)

enum np2audio86_sustained_submit_result {
    NP2_AUDIO86_SUSTAINED_ACCEPTED = 0,
    NP2_AUDIO86_SUSTAINED_RETRY = 1
};

enum np2audio86_sustained_trace_stream {
    NP2_AUDIO86_SUSTAINED_TRACE_IO = 0,
    NP2_AUDIO86_SUSTAINED_TRACE_EVENT = 1,
    NP2_AUDIO86_SUSTAINED_TRACE_RUN = 2,
    NP2_AUDIO86_SUSTAINED_TRACE_TIMER = 3,
    NP2_AUDIO86_SUSTAINED_TRACE_APPLY = 4,
    NP2_AUDIO86_SUSTAINED_TRACE_FINAL_STATE = 5,
    NP2_AUDIO86_SUSTAINED_TRACE_PCM_BYTES = 6,
    NP2_AUDIO86_SUSTAINED_TRACE_STREAM_COUNT = 7
};

typedef struct {
    uint32_t crc32_running;
    np2_sha256_context sha256;
    uint64_t records;
    uint64_t bytes;
} np2audio86_sustained_digest;

typedef struct {
    uint8_t present;
    uint32_t sequence;
    uint64_t frame_offset;
    uint16_t valid_frames;
    uint32_t crc32;
} np2audio86_sustained_slot_fingerprint;

typedef struct {
    uint8_t frozen;
    uint64_t frames;
    uint64_t bytes;
    uint32_t crc32;
    uint8_t sha256[NP2_SHA256_DIGEST_SIZE];
    uint64_t reset_event_frame;
    uint32_t reset_event_sequence;
    uint32_t reset_ordinal;
    uint64_t ring_next_frame_offset;
    uint8_t applied_after_ring;
    uint8_t ack_after_apply;
} np2audio86_sustained_reset_snapshot;

typedef struct {
    np2audio86_sustained_digest generated;
    np2audio86_sustained_digest accepted;
    np2audio86_sustained_digest pre_reset;
    np2audio86_sustained_digest trace[NP2_AUDIO86_SUSTAINED_TRACE_STREAM_COUNT];
    uint64_t next_generated_frame_offset;
    uint64_t next_accepted_frame_offset;
    uint32_t next_generated_sequence;
    uint32_t next_accepted_sequence;
    uint16_t generated_slot_fill_frames;
    np2audio86_sustained_slot_fingerprint first_accepted;
    np2audio86_sustained_slot_fingerprint final_accepted;
    uint8_t retry_pending;
    uint32_t retry_sequence;
    uint64_t retry_frame_offset;
    uint16_t retry_valid_frames;
    uint32_t retry_crc32;
    uint8_t retry_pcm[NP2_AUDIO86_SUSTAINED_SLOT_BYTES];
    uint32_t retry_attempts;
    uint32_t retry_identity_failures;
    np2audio86_sustained_reset_snapshot reset;
    uint32_t pcm_ring_max_occupancy;
    uint32_t pcm_producer_full_wait_count;
    uint32_t pcm_consumer_premature_empty_count;
    uint8_t producer_full_wait_active;
    uint8_t stream_started;
    uint8_t running_accept_seen;
    uint8_t drain_completed;
    uint64_t stream_started_ms;
    uint64_t last_running_accept_ms;
    uint64_t max_running_accept_gap_ms;
    uint64_t drain_completed_ms;
} np2audio86_sustained_evidence;

void np2audio86_sustained_digest_init(np2audio86_sustained_digest *digest);
void np2audio86_sustained_digest_update(np2audio86_sustained_digest *digest,
                                        const uint8_t *bytes, size_t length,
                                        uint64_t records);
void np2audio86_sustained_digest_snapshot(
    const np2audio86_sustained_digest *digest, uint32_t *crc32,
    uint8_t sha256[NP2_SHA256_DIGEST_SIZE]);

void np2audio86_sustained_evidence_init(
    np2audio86_sustained_evidence *evidence);
int np2audio86_sustained_generated(
    np2audio86_sustained_evidence *evidence, uint32_t sequence,
    uint64_t frame_offset, const uint8_t *pcm, uint16_t valid_frames);
int np2audio86_sustained_submit(
    np2audio86_sustained_evidence *evidence,
    enum np2audio86_sustained_submit_result result, uint32_t sequence,
    uint64_t frame_offset, const uint8_t *pcm, uint16_t valid_frames,
    uint8_t sink_running, uint64_t now_ms);
void np2audio86_sustained_freeze_reset(
    np2audio86_sustained_evidence *evidence, uint64_t reset_event_frame,
    uint32_t reset_event_sequence, uint32_t reset_ordinal,
    uint64_t ring_next_frame_offset, uint8_t applied_after_ring,
    uint8_t ack_after_apply);
void np2audio86_sustained_trace_record(
    np2audio86_sustained_evidence *evidence,
    enum np2audio86_sustained_trace_stream stream,
    const uint8_t *canonical, size_t canonical_bytes);
void np2audio86_sustained_observe_ring(
    np2audio86_sustained_evidence *evidence, uint32_t occupancy);
void np2audio86_sustained_producer_full(
    np2audio86_sustained_evidence *evidence, uint8_t full);
void np2audio86_sustained_consumer_empty(
    np2audio86_sustained_evidence *evidence, uint8_t released,
    uint8_t production_done);
void np2audio86_sustained_stream_start(
    np2audio86_sustained_evidence *evidence, uint64_t now_ms);
void np2audio86_sustained_drain_complete(
    np2audio86_sustained_evidence *evidence, uint64_t now_ms);
uint64_t np2audio86_sustained_stream_wall_ms(
    const np2audio86_sustained_evidence *evidence);
uint32_t np2audio86_sustained_worker_wait_ms(uint64_t remaining_frames);

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_SUSTAINED_EVIDENCE_H */
