#include <compiler.h>
#include <commng.h>

static int require(int condition)
{
	return condition ? 0 : 1;
}

int main(void)
{
	COMMNG handle;
	COMMNG second;
	UINT8 data;

	handle = commng_create(COMCREATE_MPU98II, FALSE);
	if (require(handle != NULL) ||
		require(handle->connect == COMCONNECT_OFF) ||
		require(handle->read != NULL) ||
		require(handle->write != NULL) ||
		require(handle->msg != NULL)) {
		return 1;
	}

	second = commng_create(COMCREATE_MPU98II, TRUE);
	if (require(second != NULL) || require(second == handle)) {
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
	if (require(handle->msg(handle, COMMSG_MIDIRESET, 0) == 0) ||
		require(handle->msg(handle, COMMSG_CHANGESPEED, 0) == 0) ||
		require(handle->msg(handle, COMMSG_CHANGEMODE, 0) == 0)) {
		return 1;
	}

	commng_destroy(NULL);
	commng_destroy(handle);
	commng_destroy(second);

	handle = commng_create(COMCREATE_MPU98II, FALSE);
	if (require(handle == second) || require(handle->connect == COMCONNECT_OFF)) {
		return 1;
	}
	if (require(commng_create(0xdeadbeefU, FALSE) == NULL)) {
		return 1;
	}
	commng_destroy(handle);
	return 0;
}
