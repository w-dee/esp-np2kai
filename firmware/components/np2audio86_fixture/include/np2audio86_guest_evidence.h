/* Portable deterministic evidence serializers for the real PC-9801-86 guest
 * boundary.  Callers own storage and hashing; no host filesystem is involved. */
#ifndef NP2AUDIO86_GUEST_EVIDENCE_H
#define NP2AUDIO86_GUEST_EVIDENCE_H

#include <stddef.h>
#include <stdint.h>

#include "np2audio86_guest_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t np2audio86_guest_evidence_serialize_events(
    const np2audio86_guest_trace_t *trace, uint8_t *out);
size_t np2audio86_guest_evidence_serialize_runs(
    const np2audio86_guest_trace_t *trace, uint8_t *out);
size_t np2audio86_guest_evidence_serialize_timers(
    const np2audio86_guest_trace_t *trace, uint8_t *out);
size_t np2audio86_guest_evidence_serialize_io(
    const np2audio86_guest_trace_t *trace, uint8_t *out);
size_t np2audio86_guest_evidence_serialize_state(
    const np2audio86_guest_state_snapshot_t *state, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_GUEST_EVIDENCE_H */
