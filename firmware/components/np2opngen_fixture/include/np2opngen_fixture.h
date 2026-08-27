#ifndef NP2_OPNGEN_FIXTURE_H
#define NP2_OPNGEN_FIXTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*np2opngen_fixture_clock_fn)(void *context);

/* Run the portable E1 direct OPNGEN fixture and print its structured result. */
int np2opngen_fixture_run(np2opngen_fixture_clock_fn clock_fn, void *context);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_FIXTURE_H */
