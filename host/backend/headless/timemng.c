#define _POSIX_C_SOURCE 200809L

#include <sys/time.h>
#include <time.h>

#include <compiler.h>
#include <timemng.h>

BRESULT timemng_gettime(_SYSTIME *systime)
{
	struct timeval now;
	struct tm local;
	_SYSTIME value;

	if (gettimeofday(&now, NULL) != 0) {
		return FAILURE;
	}
	if (localtime_r(&now.tv_sec, &local) == NULL) {
		return FAILURE;
	}

	value.year = (UINT16)(local.tm_year + 1900);
	value.month = (UINT16)(local.tm_mon + 1);
	value.week = (UINT16)local.tm_wday;
	value.day = (UINT16)local.tm_mday;
	value.hour = (UINT16)local.tm_hour;
	value.minute = (UINT16)local.tm_min;
	value.second = (UINT16)local.tm_sec;
	value.milli = (UINT16)(now.tv_usec / 1000L);

	*systime = value;
	return SUCCESS;
}
