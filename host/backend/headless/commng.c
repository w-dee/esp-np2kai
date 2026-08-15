#include <compiler.h>
#include <commng.h>

/*
 * This mirrors the pinned disconnected COMMNG semantics: COMCONNECT_OFF is
 * intentional, and real MIDI transport requires a separately reviewed backend.
 */
static UINT headless_commng_read(COMMNG self, UINT8 *data)
{
	(void)self;
	(void)data;
	return 0;
}

static UINT headless_commng_write(COMMNG self, UINT8 data)
{
	(void)self;
	(void)data;
	return 0;
}

static UINT headless_commng_writeretry(COMMNG self)
{
	(void)self;
	return 1;
}

static void headless_commng_beginblocktranster(COMMNG self)
{
	(void)self;
}

static void headless_commng_endblocktranster(COMMNG self)
{
	(void)self;
}

static UINT headless_commng_lastwritesuccess(COMMNG self)
{
	(void)self;
	return 1;
}

static UINT8 headless_commng_getstat(COMMNG self)
{
	(void)self;
	return 0xf0;
}

static INTPTR headless_commng_msg(COMMNG self, UINT message, INTPTR parameter)
{
	(void)self;
	(void)message;
	(void)parameter;
	return 0;
}

static void headless_commng_release(COMMNG self)
{
	(void)self;
}

static _COMMNG headless_commng = {
	.connect = COMCONNECT_OFF,
	.read = headless_commng_read,
	.write = headless_commng_write,
	.writeretry = headless_commng_writeretry,
	.beginblocktranster = headless_commng_beginblocktranster,
	.endblocktranster = headless_commng_endblocktranster,
	.lastwritesuccess = headless_commng_lastwritesuccess,
	.getstat = headless_commng_getstat,
	.msg = headless_commng_msg,
	.release = headless_commng_release,
	.lastdata = 0,
	.lastdatafail = 0,
	.lastdatatime = 0
};

COMMNG commng_create(UINT device, BOOL on_reset)
{
	switch (device) {
	case COMCREATE_SERIAL:
	case COMCREATE_PRINTER:
	case COMCREATE_MPU98II:
		(void)on_reset;
		return &headless_commng;
	default:
		return NULL;
	}
}

void commng_destroy(COMMNG handle)
{
	if (handle != NULL && handle->release != NULL) {
		handle->release(handle);
	}
}
