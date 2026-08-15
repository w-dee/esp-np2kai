#include <commng.h>

#include <stddef.h>

typedef struct {
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
} reference_commng;

_Static_assert(COMCREATE_SERIAL == 0, "COMCREATE_SERIAL drift");
_Static_assert(COMCREATE_PRINTER == 3, "COMCREATE_PRINTER drift");
_Static_assert(COMCREATE_MPU98II == 4, "COMCREATE_MPU98II drift");
_Static_assert(COMCREATE_NULL == 0xffff, "COMCREATE_NULL drift");

_Static_assert(COMCONNECT_OFF == 0, "COMCONNECT_OFF drift");
_Static_assert(COMCONNECT_SERIAL == 1, "COMCONNECT_SERIAL drift");
_Static_assert(COMCONNECT_MIDI == 2, "COMCONNECT_MIDI drift");
_Static_assert(COMCONNECT_PARALLEL == 3, "COMCONNECT_PARALLEL drift");

_Static_assert(COMMSG_MIDIRESET == 0, "COMMSG_MIDIRESET drift");
_Static_assert(COMMSG_SETFLAG == 1, "COMMSG_SETFLAG drift");
_Static_assert(COMMSG_GETFLAG == 2, "COMMSG_GETFLAG drift");
#if defined(VAEG_FIX)
_Static_assert(COMMSG_SETRSFLAG == 3, "COMMSG_SETRSFLAG drift");
_Static_assert(COMMSG_CHANGESPEED == 4, "COMMSG_CHANGESPEED drift");
_Static_assert(COMMSG_CHANGEMODE == 5, "COMMSG_CHANGEMODE drift");
_Static_assert(COMMSG_SETCOMMAND == 6, "COMMSG_SETCOMMAND drift");
_Static_assert(COMMSG_PURGE == 7, "COMMSG_PURGE drift");
_Static_assert(COMMSG_GETERROR == 8, "COMMSG_GETERROR drift");
_Static_assert(COMMSG_CLRERROR == 9, "COMMSG_CLRERROR drift");
_Static_assert(COMMSG_REOPEN == 10, "COMMSG_REOPEN drift");
#else
_Static_assert(COMMSG_CHANGESPEED == 3, "COMMSG_CHANGESPEED drift");
_Static_assert(COMMSG_CHANGEMODE == 4, "COMMSG_CHANGEMODE drift");
_Static_assert(COMMSG_SETCOMMAND == 5, "COMMSG_SETCOMMAND drift");
_Static_assert(COMMSG_PURGE == 6, "COMMSG_PURGE drift");
_Static_assert(COMMSG_GETERROR == 7, "COMMSG_GETERROR drift");
_Static_assert(COMMSG_CLRERROR == 8, "COMMSG_CLRERROR drift");
_Static_assert(COMMSG_REOPEN == 9, "COMMSG_REOPEN drift");
#endif
_Static_assert(COMMSG_USER == 0x80, "COMMSG_USER drift");

#define ASSERT_COMMNG_LAYOUT(member) \
	_Static_assert(offsetof(_COMMNG, member) == offsetof(reference_commng, member), \
		"COMMNG layout drift: " #member)

ASSERT_COMMNG_LAYOUT(connect);
ASSERT_COMMNG_LAYOUT(read);
ASSERT_COMMNG_LAYOUT(write);
ASSERT_COMMNG_LAYOUT(writeretry);
ASSERT_COMMNG_LAYOUT(beginblocktranster);
ASSERT_COMMNG_LAYOUT(endblocktranster);
ASSERT_COMMNG_LAYOUT(lastwritesuccess);
ASSERT_COMMNG_LAYOUT(getstat);
ASSERT_COMMNG_LAYOUT(msg);
ASSERT_COMMNG_LAYOUT(release);
ASSERT_COMMNG_LAYOUT(lastdata);
ASSERT_COMMNG_LAYOUT(lastdatafail);
ASSERT_COMMNG_LAYOUT(lastdatatime);
_Static_assert(sizeof(_COMMNG) == sizeof(reference_commng), "COMMNG size drift");
_Static_assert(sizeof(INT_PTR) == sizeof(void *), "INT_PTR is not pointer-sized");
_Static_assert((INT_PTR)-1 < (INT_PTR)0, "INT_PTR is not signed");

static int require(int condition)
{
	return condition ? 0 : 1;
}

int main(void)
{
	static const UINT devices[] = {
		COMCREATE_SERIAL,
		COMCREATE_PRINTER,
		COMCREATE_MPU98II
	};
	COMMNG handle;
	COMMNG second;
	UINT8 data;
	size_t index;

	handle = NULL;
	for (index = 0; index < sizeof(devices) / sizeof(devices[0]); ++index) {
		COMMNG current = commng_create(devices[index], FALSE);
		if (require(current != NULL) ||
			require(current->connect == COMCONNECT_OFF) ||
			require(current->read != NULL) ||
			require(current->write != NULL) ||
			require(current->writeretry != NULL) ||
			require(current->beginblocktranster != NULL) ||
			require(current->endblocktranster != NULL) ||
			require(current->lastwritesuccess != NULL) ||
			require(current->getstat != NULL) ||
			require(current->msg != NULL) ||
			require(current->release != NULL) ||
			require(current->lastdata == 0) ||
			require(current->lastdatafail == 0) ||
			require(current->lastdatatime == 0)) {
			return 1;
		}
		if (handle == NULL) {
			handle = current;
		} else if (require(handle == current)) {
			return 1;
		}
		if (require(commng_create(devices[index], TRUE) == handle)) {
			return 1;
		}
	}

	second = commng_create(COMCREATE_MPU98II, TRUE);
	if (require(second == handle)) {
		return 1;
	}

	data = 0x5a;
	if (require(handle->read(handle, &data) == 0) || require(data == 0x5a)) {
		return 1;
	}
	if (require(handle->write(handle, 0x90) == 0) ||
		require(handle->write(handle, 0xfc) == 0)) {
		return 1;
	}
	if (require(handle->writeretry(handle) == 1) ||
		require(handle->lastwritesuccess(handle) == 1) ||
		require(handle->getstat(handle) == 0xf0)) {
		return 1;
	}
	handle->beginblocktranster(handle);
	handle->endblocktranster(handle);
	if (require(handle->msg(handle, COMMSG_MIDIRESET, 0) == 0) ||
		require(handle->msg(handle, COMMSG_SETFLAG, 0) == 0) ||
		require(handle->msg(handle, COMMSG_GETFLAG, 0) == 0) ||
		require(handle->msg(handle, COMMSG_CHANGESPEED, 0) == 0) ||
		require(handle->msg(handle, COMMSG_CHANGEMODE, 0) == 0) ||
		require(handle->msg(handle, COMMSG_SETCOMMAND, 0) == 0) ||
		require(handle->msg(handle, COMMSG_PURGE, 0) == 0) ||
		require(handle->msg(handle, COMMSG_GETERROR, 0) == 0) ||
		require(handle->msg(handle, COMMSG_CLRERROR, 0) == 0) ||
		require(handle->msg(handle, COMMSG_REOPEN, 0) == 0)) {
		return 1;
	}
	handle->release(handle);

	commng_destroy(NULL);
	commng_destroy(handle);
	commng_destroy(second);

	handle = commng_create(COMCREATE_MPU98II, FALSE);
	if (require(handle == second) || require(handle->connect == COMCONNECT_OFF)) {
		return 1;
	}
	if (require(commng_create(COMCREATE_NULL, FALSE) == NULL) ||
		require(commng_create(0xdeadbeefU, FALSE) == NULL)) {
		return 1;
	}
	commng_destroy(handle);
	return 0;
}
