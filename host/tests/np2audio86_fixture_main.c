#include <stdio.h>
#include <string.h>

#include "np2audio86_fixture.h"

static int result_equal(const struct np2audio86_fixture_result *left,
                        const struct np2audio86_fixture_result *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

int main(int argc, char **argv)
{
    struct np2audio86_fixture_result first;
    struct np2audio86_fixture_result second;
    int probe = argc == 2 && strcmp(argv[1], "--probe") == 0;
    if (argc > 2 || (argc == 2 && !probe)) {
        fprintf(stderr, "Usage: %s [--probe]\n", argv[0]);
        return 2;
    }
    if (np2audio86_fixture_render(&first) != 0) {
        return 1;
    }
    if (np2audio86_fixture_render(&second) != 0) {
        return 1;
    }
    np2audio86_fixture_print_result(&first);
    printf("AUDIO86_SYNC_SAME_PROCESS_RESET=%s\n",
           result_equal(&first, &second) ? "PASS" : "FAIL");
    if (!probe) {
        printf("AUDIO86_SYNC_GOLDEN=%s\n",
               np2audio86_fixture_matches_golden(&first) ? "PASS" : "FAIL");
        if (!np2audio86_fixture_matches_golden(&first) ||
            !result_equal(&first, &second) || first.frames != 2880000U ||
            first.bytes != 11520000U || first.quanta != 12000U ||
            first.fm_contribution == 0U || first.psg_contribution == 0U ||
            first.rhythm_contribution == 0U || first.pcm86_contribution == 0U ||
            first.pcm86_fifo_underrun != 0U || first.sequence_error != 0U ||
            first.arithmetic_error != 0U || first.clamped_samples != 0U ||
            first.mix_peak_abs >= 29490U) {
            return 1;
        }
    }
    return 0;
}
