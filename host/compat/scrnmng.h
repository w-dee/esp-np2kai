#ifndef NP2_HOST_SCRNMNG_H
#define NP2_HOST_SCRNMNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    UINT8 *ptr;
    int xalign;
    int yalign;
    int width;
    int height;
    UINT bpp;
    int extend;
} SCRNSURF;

typedef struct {
    int width;
    int height;
    int requested_width;
    int requested_height;
    size_t bytes;
    BOOL initialized;
    BOOL failed;
    BOOL external;
} SCRNMNG_STATUS;

typedef enum {
	SCRNMNG_PIXEL_FORMAT_NONE = 0,
	SCRNMNG_PIXEL_FORMAT_RGB565LE = 1
} SCRNMNG_PIXEL_FORMAT;

typedef enum {
	SCRNMNG_SNAPSHOT_OK = 0,
	SCRNMNG_SNAPSHOT_INVALID_ARGUMENT = 1,
	SCRNMNG_SNAPSHOT_NOT_INITIALIZED = 2,
	SCRNMNG_SNAPSHOT_FAILED = 3,
	SCRNMNG_SNAPSHOT_BUSY = 4,
	SCRNMNG_SNAPSHOT_UNSUPPORTED = 5
} SCRNMNG_SNAPSHOT_STATUS;

typedef struct {
	int width;
	int height;
	UINT bpp;
	SCRNMNG_PIXEL_FORMAT pixel_format;
	size_t pitch;
	size_t visible_bytes;
	uint32_t surface_generation;
	uint32_t surface_update_sequence;
	uint32_t crc_algorithm;
	uint32_t crc_version;
	uint32_t crc32;
} SCRNMNG_SNAPSHOT;

/* Snapshot CRC is over visible row bytes encoded as RGB565 little-endian.
 * surface_update_sequence counts completed surface lock/unlock updates; it is
 * not a guest frame, VSYNC, or content-change count. surface_generation starts
 * at one after successful initialization and advances after successful resize. */

/* Valid only until the next scrnmng operation that can resize or shut down. */
typedef struct {
	const UINT8 *ptr;
	int width;
	int height;
	UINT bpp;
	SCRNMNG_PIXEL_FORMAT pixel_format;
	size_t pitch;
} SCRNMNG_SURFACE_VIEW;

const SCRNSURF *scrnmng_surflock(void);
void scrnmng_surfunlock(const SCRNSURF *surf);
void scrnmng_setwidth(int posx, int width);
void scrnmng_setheight(int posy, int height);
BOOL scrnmng_initialize(void);
void scrnmng_shutdown(void);
BOOL scrnmng_haserror(void);
void scrnmng_getstatus(SCRNMNG_STATUS *status);
SCRNMNG_SNAPSHOT_STATUS scrnmng_snapshot(SCRNMNG_SNAPSHOT *snapshot);
const char *scrnmng_snapshot_status_name(SCRNMNG_SNAPSHOT_STATUS status);
SCRNMNG_SNAPSHOT_STATUS scrnmng_get_surface_view(SCRNMNG_SURFACE_VIEW *view);
void scrnmng_update(void);
RGB16 scrnmng_makepal16(RGB32 pal32);

#define scrnmng_haveextend() (0)
#define scrnmng_getbpp() (16)
#define scrnmng_setextend(value) ((void)0)
#define scrnmng_allflash() ((void)0)
#define scrnmng_palchanged() ((void)0)

#endif /* NP2_HOST_SCRNMNG_H */
