#ifndef NP2HOST_DOSIO_ESP_H
#define NP2HOST_DOSIO_ESP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attach exactly one read-only virtual file to the firmware DOSIO layer. */
int np2_dosio_attach_fixture(const char *path,
                             const uint8_t *data,
                             size_t size);
void np2_dosio_detach_fixture(void);

#ifdef __cplusplus
}
#endif

#endif /* NP2HOST_DOSIO_ESP_H */
