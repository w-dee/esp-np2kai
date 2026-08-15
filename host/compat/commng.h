#ifndef NP2_HOST_COMMNG_H
#define NP2_HOST_COMMNG_H

#include <compiler.h>

enum {
    COMCREATE_SERIAL = 0,
    COMCREATE_PC9861K1,
    COMCREATE_PC9861K2,
    COMCREATE_PRINTER,
    COMCREATE_MPU98II,
#if defined(SUPPORT_SMPU98)
    COMCREATE_SMPU98_A,
    COMCREATE_SMPU98_B,
#endif
    COMCREATE_NULL = 0xffff
};

enum {
    COMCONNECT_OFF = 0,
    COMCONNECT_SERIAL,
    COMCONNECT_MIDI,
    COMCONNECT_PARALLEL
};

enum {
    COMMSG_MIDIRESET = 0,
    COMMSG_SETFLAG,
    COMMSG_GETFLAG,
#if defined(VAEG_FIX)
    COMMSG_SETRSFLAG,
#endif
    COMMSG_CHANGESPEED,
    COMMSG_CHANGEMODE,
    COMMSG_SETCOMMAND,
    COMMSG_PURGE,
    COMMSG_GETERROR,
    COMMSG_CLRERROR,
    COMMSG_REOPEN,
    COMMSG_USER = 0x80
};

struct _commng;
typedef struct _commng _COMMNG;
typedef struct _commng *COMMNG;

struct _commng {
    UINT connect;
    UINT (*read)(COMMNG self, UINT8 *data);
    UINT (*write)(COMMNG self, UINT8 data);
    UINT (*writeretry)(COMMNG self);
    void (*beginblocktranster)(COMMNG self);
    void (*endblocktranster)(COMMNG self);
    UINT (*lastwritesuccess)(COMMNG self);
    UINT8 (*getstat)(COMMNG self);
    INTPTR (*msg)(COMMNG self, UINT message, INTPTR parameter);
    void (*release)(COMMNG self);
    UINT8 lastdata;
    UINT8 lastdatafail;
    UINT lastdatatime;
};

COMMNG commng_create(UINT device, BOOL on_reset);
void commng_destroy(COMMNG handle);

#endif /* NP2_HOST_COMMNG_H */
