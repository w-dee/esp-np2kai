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

#ifdef __cplusplus
}
#endif

#endif
