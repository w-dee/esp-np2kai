/* ESP32-P4 monotonic timing adapter for the portable NP2 core. */
#include <stdint.h>

#include <compiler.h>
#include <esp_timer.h>

UINT32 np2_host_gettick(void)
{
	int64_t microseconds = esp_timer_get_time();

	if (microseconds < 0) {
		return 0;
	}
	return (UINT32)((uint64_t)microseconds / 1000U);
}
