#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include <scrnmng_storage.h>

void *scrnmng_storage_alloc(size_t bytes)
{
	void *ptr;

	ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if ((ptr != NULL) && !esp_ptr_external_ram(ptr)) {
		heap_caps_free(ptr);
		ptr = NULL;
	}
	return ptr;
}

void scrnmng_storage_free(void *ptr)
{
	heap_caps_free(ptr);
}

int scrnmng_storage_is_external(const void *ptr)
{
	return (ptr != NULL) && esp_ptr_external_ram(ptr);
}
