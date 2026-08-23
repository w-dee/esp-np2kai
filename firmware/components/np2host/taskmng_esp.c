/* Cooperative task-exit state plus the provisional scheduler-cooperation hook.
 * The one-tick block is a runtime-validation liveness policy, not final
 * emulator pacing or presentation backpressure. */
#include <stdatomic.h>
#include <stdint.h>

#include <compiler.h>
#include <taskmng.h>

/* Keep this strict-C11 translation unit independent of the FreeRTOS headers:
 * ESP-IDF's RISC-V headers use GNU-only `asm` tokens.  TickType_t is a
 * uint32_t on the ESP32-P4 configuration used here, and the ABI-compatible
 * declaration is sufficient for this one-tick cooperation hook. */
extern void vTaskDelay(uint32_t xTicksToDelay);

static atomic_bool np2_taskmng_exit_requested;

void taskmng_exit(void)
{
	atomic_store_explicit(&np2_taskmng_exit_requested, TRUE, memory_order_release);
}

void np2_host_taskmng_reset(void)
{
	atomic_store_explicit(&np2_taskmng_exit_requested, FALSE, memory_order_release);
}

BOOL np2_host_taskmng_exit_requested(void)
{
	return atomic_load_explicit(&np2_taskmng_exit_requested,
								memory_order_acquire) ? TRUE : FALSE;
}

void np2_host_taskmng_cooperate(void)
{
	/* One tick is deliberately provisional for the next physical scheduler
	 * smoke test.  It does not wait for presentation or acknowledge a frame. */
	vTaskDelay(1);
}
