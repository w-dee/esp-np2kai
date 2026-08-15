#ifndef NP2_HOST_SYSMNG_H
#define NP2_HOST_SYSMNG_H

enum {
    SYS_UPDATECFG = 0x0001,
    SYS_UPDATEFDD = 0x0080,
    SYS_UPDATEHDD = 0x0100
};

void sysmng_update(UINT update);
void sysmng_cpureset(void);

#define sysmng_fddaccess(value) ((void)(value))
#define sysmng_hddaccess(value) ((void)(value))

#endif /* NP2_HOST_SYSMNG_H */
