#ifndef NP2_HOST_SCRNMNG_STORAGE_H
#define NP2_HOST_SCRNMNG_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *scrnmng_storage_alloc(size_t bytes);
void scrnmng_storage_free(void *ptr);
int scrnmng_storage_is_external(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* NP2_HOST_SCRNMNG_STORAGE_H */
