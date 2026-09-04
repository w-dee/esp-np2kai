#include "np2audio86_guest_async.h"

int np2audio86_guest_action_kind_for_opcode(uint32_t opcode, uint8_t *kind)
{
    if (kind == NULL) {
        return -1;
    }
    switch (opcode) {
    case NP2AUDIO86_TRACE_OPNA_REGISTER:
        *kind = NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER;
        return 0;
    case NP2AUDIO86_TRACE_OPNA_CSM:
        *kind = NP2_AUDIO86_GUEST_ACTION_OPNA_CSM;
        return 0;
    case NP2AUDIO86_TRACE_PCM_CONTROL:
        *kind = NP2_AUDIO86_GUEST_ACTION_PCM_CONTROL;
        return 0;
    case NP2AUDIO86_TRACE_RESET_BARRIER:
        *kind = NP2_AUDIO86_GUEST_ACTION_RESET;
        return 0;
    default:
        return -1;
    }
}

static int prepare_worker(
    struct np2audio86_render_state *state, uint8_t *source,
    size_t source_bytes, int cold_start)
{
    if (state == NULL || source == NULL ||
        source_bytes != NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES ||
        (cold_start ? np2audio86_render_init(state)
                    : np2audio86_render_reset(state)) != 0 ||
        np2audio86_fixture_generate_source(source) != 0 ||
        np2audio86_render_pcm86_push(state, source, source_bytes) != 0) {
        return -1;
    }
    return 0;
}

int np2audio86_guest_action_prime_worker(
    struct np2audio86_render_state *state, uint8_t *source,
    size_t source_bytes)
{
    return prepare_worker(state, source, source_bytes, 1);
}

int np2audio86_guest_action_apply(
    struct np2audio86_render_state *state,
    const struct np2audio86_guest_action *action,
    const uint8_t *data, size_t data_count, uint8_t *source,
    size_t source_bytes)
{
    uint8_t expected_kind;
    if (state == NULL || action == NULL) {
        return -1;
    }
    if (action->kind == NP2_AUDIO86_GUEST_ACTION_DATA_RUN) {
        if (action->opcode != NP2AUDIO86_TRACE_PCM || action->byte_count == 0U ||
            action->byte_count > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            data == NULL || data_count != action->byte_count) {
            return -1;
        }
        return np2audio86_render_pcm86_push(state, data, data_count);
    }
    if (np2audio86_guest_action_kind_for_opcode(action->opcode,
                                                 &expected_kind) != 0 ||
        expected_kind != action->kind) {
        return -1;
    }
    switch (action->kind) {
    case NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER:
        return np2audio86_render_apply_opna_register(
            state, (uint16_t)(action->payload >> 8U),
            (uint8_t)action->payload);
    case NP2_AUDIO86_GUEST_ACTION_OPNA_CSM:
        return action->payload == 0U ? np2audio86_render_apply_opna_csm(state)
                                     : -1;
    case NP2_AUDIO86_GUEST_ACTION_PCM_CONTROL:
        return np2audio86_render_apply_pcm86_control(
            state, (uint8_t)(action->payload >> 8U),
            (uint8_t)action->payload);
    case NP2_AUDIO86_GUEST_ACTION_RESET:
        return action->payload == 0U
                   ? prepare_worker(state, source, source_bytes, 0)
                   : -1;
    default:
        return -1;
    }
}
