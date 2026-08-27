#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <time.h>

#include "np2opngen_fixture.h"

static uint64_t host_clock(void *context)
{
    struct timespec now;
    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

int main(void)
{
    return np2opngen_fixture_run(host_clock, 0) == 0 ? 0 : 1;
}
