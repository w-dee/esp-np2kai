#ifndef NP2_HOST_TASKMNG_H
#define NP2_HOST_TASKMNG_H

#ifdef __cplusplus
extern "C" {
#endif

void taskmng_exit(void);

/* Project-owned scheduler cooperation.  This is not presentation
 * backpressure, does not wait for the display consumer, and is not final
 * emulator frame pacing.  The ESP implementation currently blocks for one
 * FreeRTOS tick only to support the Step 7B.2d runtime-validation phase;
 * native/headless hosts intentionally implement this as a no-op. */
void np2_host_taskmng_cooperate(void);

#ifdef __cplusplus
}
#endif

#endif /* NP2_HOST_TASKMNG_H */
