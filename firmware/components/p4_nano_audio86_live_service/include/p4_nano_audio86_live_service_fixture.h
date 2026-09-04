#ifndef P4_NANO_AUDIO86_LIVE_SERVICE_FIXTURE_H
#define P4_NANO_AUDIO86_LIVE_SERVICE_FIXTURE_H

/*
 * Private bounded-fixture extension for the frozen 86R.5D.3 workload.
 *
 * This is intentionally not part of the generic live-service API.  In
 * particular, it does not accept a caller-selected render horizon: the only
 * terminal extension it can express is the frozen 5D.3 RESET at frame 95761
 * followed by the fixed 96000-frame terminal horizon.
 */

#include <stddef.h>
#include <stdint.h>

#include "p4_nano_audio86_live_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4_NANO_AUDIO86_5D3_RESET_FRAME UINT64_C(95761)
#define P4_NANO_AUDIO86_5D3_TERMINAL_HORIZON UINT64_C(96000)

enum p4_nano_audio86_5d3_terminal_point {
    P4_NANO_AUDIO86_5D3_T0_TERMINAL_PAIR_OBSERVED = 0,
    P4_NANO_AUDIO86_5D3_T1_PRE_RESET_RENDER_COMPLETE,
    P4_NANO_AUDIO86_5D3_T2_RESET_ACTION_BEGIN,
    P4_NANO_AUDIO86_5D3_T3_RESET_ACTION_COMPLETE,
    P4_NANO_AUDIO86_5D3_T4_RESET_EVIDENCE_COMPLETE,
    P4_NANO_AUDIO86_5D3_T5_RESET_ACK_PUBLISHED,
    P4_NANO_AUDIO86_5D3_T6_TERMINAL_PREDICATE_READY,
    P4_NANO_AUDIO86_5D3_T7_POST_RESET_RENDER_BEGIN,
    P4_NANO_AUDIO86_5D3_T8_POST_RESET_SYNTHESIS_COMPLETE,
    P4_NANO_AUDIO86_5D3_T9_Q399_PUBLISHED,
    P4_NANO_AUDIO86_5D3_T10_PCM_FINISH_COMPLETE,
};

struct p4_nano_audio86_5d3_hooks {
    void *opaque;
    int (*decorate_render)(void *opaque,
                           struct np2audio86_render_state *render,
                           uint8_t after_guest_reset);
    int (*observe_rendered_pcm)(void *opaque, const uint8_t *pcm,
                                uint16_t frames, uint64_t frame_offset);
    int (*observe_applied_action)(
        void *opaque, const struct np2audio86_core_guest_action *action,
        uint32_t reset_ordinal, uint64_t ring_next_frame_offset);
    void (*observe_ring)(void *opaque, uint32_t occupancy,
                         uint32_t next_sequence,
                         uint64_t next_frame_offset);
    void (*observe_terminal_point)(void *opaque, uint32_t point);
    /* 0 is production-equivalent fixture execution.  Values 1 and 2 are
     * retained test selectors for success-order and partial-publication
     * failure coverage respectively. */
    uint8_t terminal_publication_test_mode;
};

struct p4_nano_audio86_5d3_snapshot {
    uint64_t rendered_frames;
    uint64_t accepted_frames;
    uint64_t final_horizon;
    uint64_t ring_next_frame_offset;
    uint32_t ring_next_sequence;
    uint32_t ring_occupancy;
    uint32_t ring_partial_frames;
    uint32_t reset_ordinal;
    uint32_t reset_applied_ordinal;
    uint32_t terminal_horizon_published;
    uint32_t terminal_horizon_observed;
    uint32_t terminal_pcm_ready;
    uint32_t terminal_pcm_before_producer_done;
    uint32_t reset_event_before_terminal_horizon;
    uint32_t worker_observed_matching_pair;
    uint32_t reset_before_post_reset_render;
    uint32_t q399_published;
    uint32_t output_finished;
    uint32_t producer_done;
    uint32_t guest_attached;
    uint32_t first_error;
    uint32_t transport_residual;
    uint32_t worker_hold_ack;
    uint32_t partial_failure_event_visible;
    uint32_t partial_failure_wake_issued;
};

/* READY-only setup: neutral core init has already completed, and this call
 * applies the fixture decorator before the worker starts. */
enum p4_nano_audio86_live_result p4_nano_audio86_5d3_fixture_configure(
    struct p4_nano_audio86_live_service *service,
    const struct p4_nano_audio86_5d3_hooks *hooks);

/* Owner-only, RUNNING-only arm for the one frozen terminal RESET pair. */
enum p4_nano_audio86_live_result p4_nano_audio86_5d3_fixture_arm_terminal(
    struct p4_nano_audio86_live_service *service);

/* Owner-only completion after guest cleanup.  This is deliberately separate
 * from generic request_stop(), whose final horizon is normal guest time and
 * which never emits or depends on RESET. */
enum p4_nano_audio86_live_result p4_nano_audio86_5d3_fixture_complete_producer(
    struct p4_nano_audio86_live_service *service);

void p4_nano_audio86_5d3_fixture_snapshot(
    const struct p4_nano_audio86_live_service *service,
    struct p4_nano_audio86_5d3_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* P4_NANO_AUDIO86_LIVE_SERVICE_FIXTURE_H */
