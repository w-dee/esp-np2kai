#include <scrnmng_storage.h>

#include <stdlib.h>

void *scrnmng_storage_alloc(size_t bytes)
{
	return calloc(1, bytes);
}

void scrnmng_storage_free(void *ptr)
{
	free(ptr);
}

int scrnmng_storage_is_external(const void *ptr)
{
	(void)ptr;
	return 0;
}
