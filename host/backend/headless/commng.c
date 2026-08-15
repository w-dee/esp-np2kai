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

static INTPTR headless_commng_msg(COMMNG self, UINT message, INTPTR parameter)
{
	(void)self;
	(void)message;
	(void)parameter;
	return 0;
}

static _COMMNG headless_commng = {
	COMCONNECT_OFF,
	headless_commng_read,
	headless_commng_write,
	headless_commng_msg
};

COMMNG commng_create(UINT device, BOOL on_reset)
{
	if (device != COMCREATE_MPU98II) {
		return NULL;
	}
	(void)on_reset;
	return &headless_commng;
}

void commng_destroy(COMMNG handle)
{
	(void)handle;
}
