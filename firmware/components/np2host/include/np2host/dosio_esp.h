#ifndef NP2HOST_DOSIO_ESP_H
#define NP2HOST_DOSIO_ESP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attach exactly one read-only raw fixture to the firmware DOSIO layer. */
int np2_dosio_attach_fixture(const char *path,
                             const uint8_t *data,
                             size_t size);
void np2_dosio_detach_fixture(void);

/* Attach exactly one logical DOSIO path to one already-mounted VFS file. */
int np2_dosio_attach_vfs_file(const char *logical_path,
                              const char *physical_path);
void np2_dosio_detach_vfs_file(void);

#ifdef __cplusplus
}
#endif

#endif /* NP2HOST_DOSIO_ESP_H */
