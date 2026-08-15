#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include <compiler.h>

UINT32 np2_host_gettick(void) {
	struct timespec now = {0, 0};
	UINT64 milliseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}

	milliseconds = ((UINT64)now.tv_sec * 1000U) +
					((UINT64)now.tv_nsec / 1000000U);
	return (UINT32)milliseconds;
}
