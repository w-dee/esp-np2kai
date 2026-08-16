/* ESP-IDF wall-clock adapter; no synthetic or firmware-local fake clock. */
#include <time.h>
#include <sys/time.h>

#include <compiler.h>
#include <timemng.h>

BRESULT timemng_gettime(_SYSTIME *systime)
{
	struct timeval now;
	struct tm local;
	 time_t seconds;

	if ((systime == NULL) || (gettimeofday(&now, NULL) != 0)) {
		return FAILURE;
	}
	seconds = (time_t)now.tv_sec;
	if (localtime_r(&seconds, &local) == NULL) {
		return FAILURE;
	}

	systime->year = (UINT16)(local.tm_year + 1900);
	systime->month = (UINT16)(local.tm_mon + 1);
	systime->week = (UINT16)local.tm_wday;
	systime->day = (UINT16)local.tm_mday;
	systime->hour = (UINT16)local.tm_hour;
	systime->minute = (UINT16)local.tm_min;
	systime->second = (UINT16)local.tm_sec;
	systime->milli = (UINT16)(now.tv_usec / 1000);
	return SUCCESS;
}
