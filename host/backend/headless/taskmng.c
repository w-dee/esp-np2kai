#include <signal.h>

#include <taskmng.h>

#include "taskmng_control.h"

static volatile sig_atomic_t headless_taskmng_exit_requested;

void taskmng_exit(void)
{
	headless_taskmng_exit_requested = 1;
}

void np2_host_taskmng_reset(void)
{
	headless_taskmng_exit_requested = 0;
}

BOOL np2_host_taskmng_exit_requested(void)
{
	return headless_taskmng_exit_requested != 0 ? TRUE : FALSE;
}
