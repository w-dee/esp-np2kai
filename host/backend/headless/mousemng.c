#include <mousemng.h>

#define HEADLESS_MOUSE_BUTTONS_RELEASED ((BYTE)0xA0)

BYTE mousemng_getstat(SINT16 *x, SINT16 *y, int clear) {
	*x = 0;
	*y = 0;
	(void)clear;
	return HEADLESS_MOUSE_BUTTONS_RELEASED;
}
