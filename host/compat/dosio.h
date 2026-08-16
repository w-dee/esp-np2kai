#ifndef NP2_HOST_DOSIO_H
#define NP2_HOST_DOSIO_H

#if defined(NP2_FIRMWARE_DOSIO)
/* Firmware keeps FILEH opaque and pointer-sized; no stdio FILE is exposed. */
typedef void *FILEH;
#define FSEEK_SET 0
#define FSEEK_END 2
#else
typedef FILE *FILEH;
#define FSEEK_SET SEEK_SET
#define FSEEK_END SEEK_END
#endif
#define FILEH_INVALID NULL

enum {
    FILEATTR_READONLY = 0x01,
    FILEATTR_VOLUME = 0x08,
    FILEATTR_DIRECTORY = 0x10
};

typedef void *FLISTH;
#define FLISTH_INVALID NULL

typedef struct {
    UINT32 attr;
    OEMCHAR path[MAX_PATH];
} FLINFO;

FILEH file_open(const OEMCHAR *path);
FILEH file_open_rb(const OEMCHAR *path);
FILEH file_create(const OEMCHAR *path);
FILEPOS file_seek(FILEH handle, FILEPOS pointer, int method);
UINT file_read(FILEH handle, void *data, UINT length);
UINT file_write(FILEH handle, const void *data, UINT length);
short file_close(FILEH handle);
FILELEN file_getsize(FILEH handle);
short file_delete(const OEMCHAR *path);
short file_attr(const OEMCHAR *path);

OEMCHAR *file_getcd(const OEMCHAR *path);
FILEH file_open_c(const OEMCHAR *path);
FILEH file_create_c(const OEMCHAR *path);

FLISTH file_list1st(const OEMCHAR *dir, FLINFO *fli);
BRESULT file_listnext(FLISTH hdl, FLINFO *fli);

#define file_cpyname(dst, src, maxlen) milstr_ncpy((dst), (src), (maxlen))
void file_catname(OEMCHAR *path, const OEMCHAR *name, int maxlen);
#define file_cmpname(path, name) milstr_cmp((path), (name))
OEMCHAR *file_getname(const OEMCHAR *path);
void file_cutname(OEMCHAR *path);
OEMCHAR *file_getext(const OEMCHAR *path);
void file_cutseparator(OEMCHAR *path);
void file_setseparator(OEMCHAR *path, int maxlen);

#endif /* NP2_HOST_DOSIO_H */
