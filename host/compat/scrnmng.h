#ifndef NP2_HOST_SCRNMNG_H
#define NP2_HOST_SCRNMNG_H

typedef struct {
    UINT8 *ptr;
    int xalign;
    int yalign;
    int width;
    int height;
    UINT bpp;
    int extend;
} SCRNSURF;

const SCRNSURF *scrnmng_surflock(void);
void scrnmng_surfunlock(const SCRNSURF *surf);
void scrnmng_setwidth(int posx, int width);
void scrnmng_setheight(int posy, int height);
void scrnmng_update(void);
RGB16 scrnmng_makepal16(RGB32 pal32);

#define scrnmng_haveextend() (0)
#define scrnmng_getbpp() (16)
#define scrnmng_setextend(value) ((void)(value))
#define scrnmng_allflash() ((void)0)
#define scrnmng_palchanged() ((void)0)

#endif /* NP2_HOST_SCRNMNG_H */
