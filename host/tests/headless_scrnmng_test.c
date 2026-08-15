#include <compiler.h>
#include <scrnmng.h>

int main(void)
{
	if (scrnmng_surflock() != NULL) {
		return 1;
	}

	scrnmng_setwidth(0, 640);
	scrnmng_setwidth(17, 1024);
	scrnmng_setheight(0, 400);
	scrnmng_setheight(23, 800);
	scrnmng_surfunlock(NULL);

	if (scrnmng_haveextend() != 0) {
		return 1;
	}
	if (scrnmng_getbpp() != 16) {
		return 1;
	}

	return 0;
}
