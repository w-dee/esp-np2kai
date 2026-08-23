#include <taskmng.h>

#include <taskmng_control.h>

int main(void)
{
	np2_host_taskmng_reset();
	if (np2_host_taskmng_exit_requested()) {
		return 1;
	}
	np2_host_taskmng_cooperate();

	taskmng_exit();
	if (!np2_host_taskmng_exit_requested()) {
		return 2;
	}
	if (!np2_host_taskmng_exit_requested()) {
		return 3;
	}

	taskmng_exit();
	if (!np2_host_taskmng_exit_requested()) {
		return 4;
	}

	np2_host_taskmng_reset();
	if (np2_host_taskmng_exit_requested()) {
		return 5;
	}

	taskmng_exit();
	if (!np2_host_taskmng_exit_requested()) {
		return 6;
	}
	np2_host_taskmng_cooperate();
	return 0;
}
