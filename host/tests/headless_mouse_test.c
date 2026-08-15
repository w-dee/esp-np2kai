#include <assert.h>

#include <mousemng.h>

static void check_no_motion(int clear) {
	SINT16 x = 1234;
	SINT16 y = -567;
	BYTE buttons = mousemng_getstat(&x, &y, clear);

	assert(x == 0);
	assert(y == 0);
	assert(buttons == (BYTE)0xA0);
}

int main(void) {
	check_no_motion(0);
	check_no_motion(0);
	check_no_motion(1);
	check_no_motion(0);
	return 0;
}
