#include "np2audio86_guest_async.h"

int np2audio86_guest_action_kind_for_opcode(uint32_t opcode, uint8_t *kind)
{
    return np2audio86_core_guest_action_kind_for_opcode(opcode, kind);
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
    struct np2audio86_core_guest_action core_action;
    uint8_t expected_kind;
    if (state == NULL || action == NULL) {
        return -1;
    }
    if (action->kind != NP2_AUDIO86_GUEST_ACTION_DATA_RUN &&
        (np2audio86_guest_action_kind_for_opcode(action->opcode,
                                                  &expected_kind) != 0 ||
         expected_kind != action->kind))
        return -1;
    if (action->kind == NP2_AUDIO86_GUEST_ACTION_RESET) {
        return action->payload == 0U
                   ? prepare_worker(state, source, source_bytes, 0)
                   : -1;
    }
    core_action.frame_timestamp = action->frame_timestamp;
    core_action.sequence = action->sequence;
    core_action.opcode = action->opcode;
    core_action.payload = action->payload;
    core_action.byte_offset = action->byte_offset;
    core_action.byte_count = action->byte_count;
    core_action.kind = action->kind;
    return np2audio86_core_guest_action_apply(
        state, &core_action, data, data_count);
}
