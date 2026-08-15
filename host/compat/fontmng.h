#ifndef NP2_HOST_FONTMNG_H
#define NP2_HOST_FONTMNG_H

typedef struct {
    int width;
    int height;
    int pitch;
} _FNTDAT, *FNTDAT;

void *fontmng_create(int size, UINT type, const OEMCHAR *fontface);
void fontmng_destroy(void *handle);
FNTDAT fontmng_get(void *handle, const OEMCHAR *string);

#endif /* NP2_HOST_FONTMNG_H */
