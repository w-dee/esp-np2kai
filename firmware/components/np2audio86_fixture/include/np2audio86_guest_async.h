#ifndef NP2_AUDIO86_GUEST_ASYNC_H
#define NP2_AUDIO86_GUEST_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#include "np2audio86_fixture.h"
#include "np2audio86_guest_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical 86R actions are distinct from the numeric 86H event namespace.
 * The transport record stays 24 bytes; this explicit action layer prevents
 * accidental interpretation of guest opcode values by the 86H dispatcher. */
enum np2audio86_guest_action_kind {
    NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER = 1U,
    NP2_AUDIO86_GUEST_ACTION_OPNA_CSM = 2U,
    NP2_AUDIO86_GUEST_ACTION_PCM_CONTROL = 3U,
    NP2_AUDIO86_GUEST_ACTION_RESET = 4U,
    NP2_AUDIO86_GUEST_ACTION_DATA_RUN = 5U,
};

/* This is transport-only and is never written into the canonical 86R trace.
 * It separates DATA_RUN from canonical PCM_CONTROL, which both otherwise use
 * numeric opcode 3 in different semantic namespaces. */
#define NP2_AUDIO86_GUEST_TRANSPORT_DATA_RUN UINT32_C(0x80000001)

struct np2audio86_guest_action {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
    uint64_t byte_offset;
    uint32_t byte_count;
    uint8_t kind;
};

int np2audio86_guest_action_kind_for_opcode(uint32_t opcode,
                                             uint8_t *kind);
int np2audio86_guest_action_prime_worker(
    struct np2audio86_render_state *state, uint8_t *source,
    size_t source_bytes);
/* Apply only the historical 5D.3 seed layer to an already-neutral core.
 * This is fixture-private: it neither initializes nor resets global/core
 * state and is safe to repeat immediately after a neutral guest RESET. */
int np2audio86_guest_action_decorate_worker(
    struct np2audio86_render_state *state, uint8_t *source,
    size_t source_bytes);
int np2audio86_guest_action_apply(
    struct np2audio86_render_state *state,
    const struct np2audio86_guest_action *action,
    const uint8_t *data, size_t data_count, uint8_t *source,
    size_t source_bytes);

#ifdef __cplusplus
}
#endif

#endif /* NP2_AUDIO86_GUEST_ASYNC_H */
