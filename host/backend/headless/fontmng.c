#include <compiler.h>
#include <fontmng.h>

void *fontmng_create(int size, UINT type, const OEMCHAR *fontface)
{
	(void)size;
	(void)type;
	(void)fontface;
	return NULL;
}

void fontmng_destroy(void *handle)
{
	(void)handle;
}

FNTDAT fontmng_get(void *handle, const OEMCHAR *string)
{
	(void)handle;
	(void)string;
	return NULL;
}
