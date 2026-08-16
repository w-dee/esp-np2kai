/* Cooperative task-exit state; task creation and scheduling are later phases. */
#include <stdatomic.h>

#include <compiler.h>
#include <taskmng.h>

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
