#include <assert.h>

#include <compiler.h>
#include <timemng.h>

int main(void)
{
	_SYSTIME value;

	assert(timemng_gettime(&value) == SUCCESS);
	assert(value.month >= 1 && value.month <= 12);
	assert(value.week <= 6);
	assert(value.day >= 1 && value.day <= 31);
	assert(value.hour <= 23);
	assert(value.minute <= 59);
	assert(value.second <= 60);
	assert(value.milli <= 999);
	return 0;
}
