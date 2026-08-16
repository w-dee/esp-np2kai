/* ESP32-P4 DOSIO boundary: no filesystem is attached in Phase 2. */
#include <string.h>

#include <compiler.h>
#include <dosio.h>

static OEMCHAR np2_dosio_current_path[MAX_PATH] = "./";

static int np2_dosio_is_separator(OEMCHAR value)
{
	return value == '/';
}

static void np2_dosio_copy(OEMCHAR *destination,
						   const OEMCHAR *source,
						   int maxlen)
{
	int index;

	if ((destination == NULL) || (maxlen <= 0)) {
		return;
	}
	if (source == NULL) {
		destination[0] = '\0';
		return;
	}
	for (index = 0; (index + 1 < maxlen) && (source[index] != '\0'); index++) {
		destination[index] = source[index];
	}
	destination[index] = '\0';
}

static void np2_dosio_append(OEMCHAR *destination,
							 const OEMCHAR *source,
							 int maxlen)
{
	int offset;
	int index;

	if ((destination == NULL) || (source == NULL) || (maxlen <= 0)) {
		return;
	}
	offset = (int)strlen(destination);
	for (index = 0;
		 (offset + index + 1 < maxlen) && (source[index] != '\0');
		 index++) {
		destination[offset + index] = source[index];
	}
	destination[offset + index] = '\0';
}

FILEH file_open(const OEMCHAR *path)
{
	(void)path;
	return FILEH_INVALID;
}

FILEH file_open_rb(const OEMCHAR *path)
{
	(void)path;
	return FILEH_INVALID;
}

FILEH file_create(const OEMCHAR *path)
{
	(void)path;
	return FILEH_INVALID;
}

FILEPOS file_seek(FILEH handle, FILEPOS pointer, int method)
{
	(void)handle;
	(void)pointer;
	(void)method;
	return (FILEPOS)-1;
}

UINT file_read(FILEH handle, void *data, UINT length)
{
	(void)handle;
	(void)data;
	(void)length;
	return 0;
}

UINT file_write(FILEH handle, const void *data, UINT length)
{
	(void)handle;
	(void)data;
	(void)length;
	return 0;
}

short file_close(FILEH handle)
{
	return (handle == FILEH_INVALID) ? 0 : -1;
}

FILELEN file_getsize(FILEH handle)
{
	(void)handle;
	return 0;
}

short file_delete(const OEMCHAR *path)
{
	(void)path;
	return -1;
}

short file_attr(const OEMCHAR *path)
{
	(void)path;
	return -1;
}

OEMCHAR *file_getcd(const OEMCHAR *path)
{
	np2_dosio_copy(np2_dosio_current_path, "./", MAX_PATH);
	np2_dosio_append(np2_dosio_current_path, path, MAX_PATH);
	return np2_dosio_current_path;
}

FILEH file_open_c(const OEMCHAR *path)
{
	(void)file_getcd(path);
	return FILEH_INVALID;
}

FILEH file_create_c(const OEMCHAR *path)
{
	(void)file_getcd(path);
	return FILEH_INVALID;
}

FLISTH file_list1st(const OEMCHAR *dir, FLINFO *fli)
{
	(void)dir;
	(void)fli;
	return FLISTH_INVALID;
}

BRESULT file_listnext(FLISTH hdl, FLINFO *fli)
{
	(void)hdl;
	(void)fli;
	return FAILURE;
}

void file_catname(OEMCHAR *path, const OEMCHAR *name, int maxlen)
{
	np2_dosio_append(path, name, maxlen);
}

OEMCHAR *file_getname(const OEMCHAR *path)
{
	const OEMCHAR *current;
	const OEMCHAR *basename;

	if (path == NULL) {
		return NULL;
	}
	basename = path;
	for (current = path; *current != '\0'; current++) {
		if (np2_dosio_is_separator(*current)) {
			basename = current + 1;
		}
	}
	return (OEMCHAR *)basename;
}

void file_cutname(OEMCHAR *path)
{
	OEMCHAR *basename = file_getname(path);

	if (basename != NULL) {
		*basename = '\0';
	}
}

OEMCHAR *file_getext(const OEMCHAR *path)
{
	OEMCHAR *basename;
	OEMCHAR *current;
	OEMCHAR *last_dot = NULL;

	basename = file_getname(path);
	if (basename == NULL) {
		return NULL;
	}
	for (current = basename; *current != '\0'; current++) {
		if (*current == '.') {
			last_dot = current;
		}
	}
	return (last_dot == NULL) ? current : last_dot + 1;
}

void file_cutseparator(OEMCHAR *path)
{
	size_t length;

	if (path == NULL) {
		return;
	}
	length = strlen(path);
	if ((length > 1) &&
		!(length == 2 && path[0] == '.' && path[1] == '/') &&
		np2_dosio_is_separator(path[length - 1])) {
		path[length - 1] = '\0';
	}
}

void file_setseparator(OEMCHAR *path, int maxlen)
{
	int length;

	if ((path == NULL) || (maxlen <= 1)) {
		return;
	}
	length = (int)strlen(path);
	if ((length > 0) && !np2_dosio_is_separator(path[length - 1]) &&
		(length + 1 < maxlen)) {
		path[length] = '/';
		path[length + 1] = '\0';
	}
}
