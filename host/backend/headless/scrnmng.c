#include <compiler.h>
#include <scrnmng.h>

/*
 * The current Step 4 feature set selects no SUPPORT_*BPP renderer, so the
 * core explicitly supports a NULL surface. Revisit this backend if a pixel
 * renderer is enabled; framebuffer validation would then require a RAM surface.
 */
const SCRNSURF *scrnmng_surflock(void)
{
	return NULL;
}

void scrnmng_surfunlock(const SCRNSURF *surf)
{
	(void)surf;
}

void scrnmng_setwidth(int posx, int width)
{
	(void)posx;
	(void)width;
}

void scrnmng_setheight(int posy, int height)
{
	(void)posy;
	(void)height;
}
