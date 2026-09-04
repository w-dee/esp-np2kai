#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2audio86_core.h"

static void assert_neutral(const struct np2audio86_render_state *state)
{
    size_t i;
    assert(state->fm.playchannels == 3U);
    assert(state->fm.playing == 0U);
    assert(state->psg.reg.mixer == 0xbfU);
    assert(state->psg.reg.vol[0] == 0U);
    assert(state->psg.reg.vol[1] == 0U);
    assert(state->psg.reg.vol[2] == 0U);
    assert(state->rhythm.hdr.enable == 0U);
    assert(state->rhythm.hdr.playing == 0U);
    assert(state->pcm86.pcm.realbuf == 0);
    assert(state->pcm86.pcm.fifosize == 0x80);
    assert(state->pcm86.pcm.dactrl == 0x32U);
    assert(state->pcm86.supplied == 0U);
    assert(state->pcm86.refills == 0U);
    for (i = 0U; i < sizeof(state->pcm86.source); ++i)
        assert(state->pcm86.source[i] == 0U);
}

int main(void)
{
    struct np2audio86_render_state *first = malloc(sizeof(*first));
    struct np2audio86_render_state *second = malloc(sizeof(*second));
    struct np2audio86_core_mix_result result = {0};
    SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * NP2_AUDIO86_CHANNELS];
    size_t i;

    assert(first != NULL && second != NULL);
    np2audio86_test_opngen_initialize_reset();
    np2audio86_test_opngen_initialize_fail_next();
    assert(np2audio86_core_render_init(first) != 0);
    assert(np2audio86_test_opngen_initialize_call_count() == 0U);

    assert(np2audio86_core_render_init(first) == 0);
    assert_neutral(first);
    assert(np2audio86_test_opngen_initialize_call_count() == 1U);

    assert(np2audio86_core_render_init(second) == 0);
    assert_neutral(second);
    assert(np2audio86_test_opngen_initialize_call_count() == 1U);

    assert(np2audio86_core_render_apply_opna_register(first, 0x28U, 0xf0U) == 0);
    assert(np2audio86_core_render_apply_opna_register(first, 0x08U, 0x0fU) == 0);
    assert(np2audio86_core_render_apply_opna_register(first, 0x10U, 0x3fU) == 0);
    assert(np2audio86_core_render_reset(first) == 0);
    assert_neutral(first);
    assert(np2audio86_test_opngen_initialize_call_count() == 1U);

    memset(mix, 0, sizeof(mix));
    assert(np2audio86_core_render_span(
               first, mix, NP2_AUDIO86_QUANTUM_FRAMES, &result) == 0);
    for (i = 0U; i < sizeof(mix) / sizeof(mix[0]); ++i)
        assert(mix[i] == 0);
    assert(result.fm_contribution == 0U);
    assert(result.psg_contribution == 0U);
    assert(result.rhythm_contribution == 0U);
    assert(result.pcm86_contribution == 0U);

    np2audio86_core_render_destroy(first);
    assert(np2audio86_core_render_init(first) == 0);
    assert_neutral(first);
    assert(np2audio86_test_opngen_initialize_call_count() == 1U);
    printf("OPN_GLOBAL_INIT_PROCESS_LIFETIME_CALL_COUNT=%" PRIu32 "\n",
           np2audio86_test_opngen_initialize_call_count());
    printf("AUDIO86_CORE_NEUTRAL_INIT=PASS\n");
    printf("AUDIO86_CORE_RESET_EQUIVALENCE=PASS\n");
    printf("AUDIO86_CORE_TEST=PASS\n");
    free(first);
    free(second);
    return 0;
}
