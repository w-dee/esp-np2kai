/* Ubuntu/headless DOSIO backend for the retained portable core. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <compiler.h>
#include <dosio.h>

static OEMCHAR dosio_curpath[MAX_PATH] = "./";

typedef struct {
	DIR *directory;
	OEMCHAR path[MAX_PATH];
} HEADLESS_FLIST;

static int dosio_is_separator(OEMCHAR value)
{
	return value == '/';
}

static void dosio_copy(OEMCHAR *destination, const OEMCHAR *source, int maxlen)
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

static void dosio_append(OEMCHAR *destination, const OEMCHAR *source, int maxlen)
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

static short dosio_attr_from_stat(const struct stat *status)
{
	short attribute = 0;

	if (S_ISDIR(status->st_mode)) {
		attribute |= FILEATTR_DIRECTORY;
	}
	if ((status->st_mode & S_IWUSR) == 0) {
		attribute |= FILEATTR_READONLY;
	}
	return attribute;
}

static void dosio_flist_release(HEADLESS_FLIST *list)
{
	if (list == NULL) {
		return;
	}
	if (list->directory != NULL) {
		closedir(list->directory);
		list->directory = NULL;
	}
	free(list);
}

static int dosio_flist_prefix(HEADLESS_FLIST *list, const OEMCHAR *directory)
{
	size_t length;

	if ((list == NULL) || (directory == NULL)) {
		return 0;
	}
	length = strlen(directory);
	if (length >= MAX_PATH) {
		return 0;
	}
	dosio_copy(list->path, directory, MAX_PATH);
	if ((length > 0) && (list->path[length - 1] == '/')) {
		return 1;
	}
	if (length + 1 >= MAX_PATH) {
		return 0;
	}
	list->path[length] = '/';
	list->path[length + 1] = '\0';
	return 1;
}

static int dosio_flist_next(HEADLESS_FLIST *list, FLINFO *fli)
{
	struct dirent *entry;
	struct stat status;
	char fullpath[MAX_PATH];
	size_t prefix_length;
	size_t name_length;

	if ((list == NULL) || (list->directory == NULL) || (fli == NULL)) {
		return 0;
	}
	entry = readdir(list->directory);
	if (entry == NULL) {
		return 0;
	}
	prefix_length = strlen(list->path);
	name_length = strlen(entry->d_name);
	if (prefix_length + name_length >= MAX_PATH) {
		return 0;
	}
	memcpy(fullpath, list->path, prefix_length);
	memcpy(fullpath + prefix_length, entry->d_name, name_length + 1);
	if (stat(fullpath, &status) != 0) {
		return 0;
	}
	fli->attr = (UINT32)dosio_attr_from_stat(&status);
	dosio_copy(fli->path, entry->d_name, MAX_PATH);
	return 1;
}

FILEH file_open(const OEMCHAR *path)
{
	FILE *handle;

	if (path == NULL) {
		return FILEH_INVALID;
	}
	handle = fopen(path, "rb+");
	if (handle == NULL) {
		handle = fopen(path, "rb");
	}
	return handle;
}

FILEH file_open_rb(const OEMCHAR *path)
{
	if (path == NULL) {
		return FILEH_INVALID;
	}
	return fopen(path, "rb");
}

FILEH file_create(const OEMCHAR *path)
{
	if (path == NULL) {
		return FILEH_INVALID;
	}
	return fopen(path, "wb+");
}

FILEPOS file_seek(FILEH handle, FILEPOS pointer, int method)
{
	long position;

	if ((handle == FILEH_INVALID) || (fseek(handle, (long)pointer, method) != 0)) {
		return (FILEPOS)-1;
	}
	position = ftell(handle);
	if (position < 0) {
		return (FILEPOS)-1;
	}
	return (FILEPOS)position;
}

UINT file_read(FILEH handle, void *data, UINT length)
{
	if (handle == FILEH_INVALID) {
		return 0;
	}
	return (UINT)fread(data, 1, length, handle);
}

UINT file_write(FILEH handle, const void *data, UINT length)
{
	long position;

	if (handle == FILEH_INVALID) {
		return 0;
	}
	if (length == 0) {
		if (fflush(handle) != 0) {
			return 0;
		}
		position = ftell(handle);
		if ((position < 0) ||
			(ftruncate(fileno(handle), (off_t)position) != 0)) {
			return 0;
		}
		return 0;
	}
	return (UINT)fwrite(data, 1, length, handle);
}

short file_close(FILEH handle)
{
	if (handle == FILEH_INVALID) {
		return 0;
	}
	return (short)fclose(handle);
}

FILELEN file_getsize(FILEH handle)
{
	struct stat status;

	if ((handle == FILEH_INVALID) ||
		(fflush(handle) != 0) ||
		(fstat(fileno(handle), &status) != 0)) {
		return 0;
	}
	return (FILELEN)status.st_size;
}

short file_delete(const OEMCHAR *path)
{
	if (path == NULL) {
		return -1;
	}
	return (short)remove(path);
}

short file_attr(const OEMCHAR *path)
{
	struct stat status;

	if ((path == NULL) || (stat(path, &status) != 0)) {
		return -1;
	}
	return dosio_attr_from_stat(&status);
}

OEMCHAR *file_getcd(const OEMCHAR *path)
{
	dosio_copy(dosio_curpath, "./", MAX_PATH);
	dosio_append(dosio_curpath, path, MAX_PATH);
	return dosio_curpath;
}

FILEH file_create_c(const OEMCHAR *path)
{
	return file_create(file_getcd(path));
}

void file_catname(OEMCHAR *path, const OEMCHAR *name, int maxlen)
{
	dosio_append(path, name, maxlen);
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
		if (dosio_is_separator(*current)) {
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
		dosio_is_separator(path[length - 1])) {
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
	if ((length > 0) && !dosio_is_separator(path[length - 1]) &&
		(length + 1 < maxlen)) {
		path[length] = '/';
		path[length + 1] = '\0';
	}
}

FLISTH file_list1st(const OEMCHAR *dir, FLINFO *fli)
{
	HEADLESS_FLIST *list;

	if (dir == NULL) {
		return FLISTH_INVALID;
	}
	list = (HEADLESS_FLIST *)malloc(sizeof(*list));
	if (list == NULL) {
		return FLISTH_INVALID;
	}
	list->directory = NULL;
	if (!dosio_flist_prefix(list, dir)) {
		dosio_flist_release(list);
		return FLISTH_INVALID;
	}
	list->directory = opendir(dir);
	if (list->directory == NULL) {
		dosio_flist_release(list);
		return FLISTH_INVALID;
	}
	if (!dosio_flist_next(list, fli)) {
		dosio_flist_release(list);
		return FLISTH_INVALID;
	}
	return (FLISTH)list;
}

BRESULT file_listnext(FLISTH hdl, FLINFO *fli)
{
	HEADLESS_FLIST *list = (HEADLESS_FLIST *)hdl;

	if (list == NULL) {
		return FAILURE;
	}
	if (!dosio_flist_next(list, fli)) {
		/* A terminal failure consumes the trimmed ABI's enumeration handle. */
		dosio_flist_release(list);
		return FAILURE;
	}
	return SUCCESS;
}
