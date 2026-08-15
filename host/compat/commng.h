#ifndef NP2_HOST_COMMNG_H
#define NP2_HOST_COMMNG_H

enum {
    COMCREATE_MPU98II = 4
};

enum {
    COMCONNECT_OFF = 0
};

enum {
    COMMSG_MIDIRESET = 0,
    COMMSG_CHANGESPEED = 3,
    COMMSG_CHANGEMODE = 4
};

struct _commng;
typedef struct _commng _COMMNG;
typedef struct _commng *COMMNG;

struct _commng {
    UINT connect;
    UINT (*read)(COMMNG self, UINT8 *data);
    UINT (*write)(COMMNG self, UINT8 data);
    INTPTR (*msg)(COMMNG self, UINT message, INTPTR parameter);
};

COMMNG commng_create(UINT device, BOOL on_reset);
void commng_destroy(COMMNG handle);

#endif /* NP2_HOST_COMMNG_H */
