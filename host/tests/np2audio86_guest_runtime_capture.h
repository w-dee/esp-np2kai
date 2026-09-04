#ifndef NP2AUDIO86_GUEST_RUNTIME_CAPTURE_H
#define NP2AUDIO86_GUEST_RUNTIME_CAPTURE_H

#include <stdint.h>

#include "np2audio86_guest_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the existing real-i286 86R.2 fixture once and copy its canonical
 * producer trace into caller-owned storage.  This is a host-only seam: the
 * guest adapter remains the sole source of the records. */
int np2audio86_guest_runtime_capture(np2audio86_guest_trace_t *trace,
                                     np2audio86_guest_state_snapshot_t *state);

typedef struct {
    size_t program_bytes;
    size_t io_observation_count;
    uint64_t first_io_guest_cycle;
    uint64_t last_io_guest_cycle;
    uint16_t termination_ip;
    uint8_t terminated_at_hlt;
} np2audio86_guest_execution_evidence_t;

int np2audio86_guest_runtime_capture_sustained_2s(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    np2audio86_guest_execution_evidence_t *evidence);

typedef int (*np2audio86_guest_runtime_stage_fn)(void *opaque);

/* Execute the sustained real-i286 fixture with a lifecycle owner attaching
 * after bootstrap RESET and arming its private terminal protocol immediately
 * before the guest RESET.  The attached sink remains owned by the caller so
 * it can publish producer completion, join and destroy the service. */
int np2audio86_guest_runtime_live_sustained_2s(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    np2audio86_guest_execution_evidence_t *evidence,
    np2audio86_guest_runtime_stage_fn attach_after_bootstrap,
    np2audio86_guest_runtime_stage_fn arm_before_terminal_reset, void *opaque);

/* Execute the same prepared real-i286 fixture while semantic handlers publish
 * directly to sink.  The optional trace remains an observation copy only; it
 * is never drained to drive publication. */
int np2audio86_guest_runtime_live(
    np2audio86_guest_trace_t *trace, np2audio86_guest_state_snapshot_t *state,
    const np2audio86_guest_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif
