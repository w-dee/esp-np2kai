/* ESP32-P4 DOSIO boundary: one read-only raw or POSIX/VFS file is attached. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#include <compiler.h>
#include <dosio.h>
#include <np2host/dosio_esp.h>

static OEMCHAR np2_dosio_current_path[MAX_PATH] = "./";

typedef enum {
	NP2_DOSIO_HANDLE_NONE = 0,
	NP2_DOSIO_HANDLE_RAW_MEMORY,
	NP2_DOSIO_HANDLE_VFS_FILE,
} NP2_DOSIO_HANDLE_KIND;

typedef struct {
	NP2_DOSIO_HANDLE_KIND kind;
	int active;
	union {
		struct {
			const uint8_t *data;
			size_t size;
			size_t position;
		} raw;
		struct {
			int fd;
			FILELEN size;
			FILEPOS position;
			int last_error;
		} vfs;
	} backend;
} NP2_DOSIO_HANDLE;

static NP2_DOSIO_HANDLE_KIND np2_dosio_backend_kind;
static OEMCHAR np2_dosio_fixture_path[MAX_PATH];
static const uint8_t *np2_dosio_fixture_data;
static size_t np2_dosio_fixture_size;
static OEMCHAR np2_dosio_vfs_logical_path[MAX_PATH];
static char np2_dosio_vfs_physical_path[MAX_PATH];
static NP2_DOSIO_HANDLE np2_dosio_fixture_handle;

static int np2_dosio_copy_path(char *destination, const char *source)
{
	size_t length;

	if ((destination == NULL) || (source == NULL) || (source[0] == '\0')) {
		return 0;
	}
	length = strlen(source);
	if (length >= MAX_PATH) {
		return 0;
	}
	memcpy(destination, source, length + 1);
	return 1;
}

static void np2_dosio_reset_handle(void)
{
	memset(&np2_dosio_fixture_handle, 0, sizeof(np2_dosio_fixture_handle));
	np2_dosio_fixture_handle.kind = NP2_DOSIO_HANDLE_NONE;
}

static int np2_dosio_handle_active(void)
{
	return np2_dosio_fixture_handle.active != 0;
}

int np2_dosio_attach_fixture(const char *path,
							 const uint8_t *data,
							 size_t size)
{
	if (np2_dosio_backend_kind != NP2_DOSIO_HANDLE_NONE ||
		np2_dosio_handle_active()) {
		return 0;
	}
	if ((data == NULL) || (size == 0) ||
		!np2_dosio_copy_path(np2_dosio_fixture_path, path)) {
		return 0;
	}
	np2_dosio_fixture_data = data;
	np2_dosio_fixture_size = size;
	np2_dosio_backend_kind = NP2_DOSIO_HANDLE_RAW_MEMORY;
	return 1;
}

void np2_dosio_detach_fixture(void)
{
	if (np2_dosio_backend_kind != NP2_DOSIO_HANDLE_RAW_MEMORY) {
		return;
	}
	np2_dosio_reset_handle();
	np2_dosio_backend_kind = NP2_DOSIO_HANDLE_NONE;
	np2_dosio_fixture_path[0] = '\0';
	np2_dosio_fixture_data = NULL;
	np2_dosio_fixture_size = 0;
}

int np2_dosio_attach_vfs_file(const char *logical_path,
							  const char *physical_path)
{
	if (np2_dosio_backend_kind != NP2_DOSIO_HANDLE_NONE ||
		np2_dosio_handle_active()) {
		return 0;
	}
	if (!np2_dosio_copy_path(np2_dosio_vfs_logical_path, logical_path) ||
		!np2_dosio_copy_path(np2_dosio_vfs_physical_path, physical_path)) {
		return 0;
	}
	np2_dosio_backend_kind = NP2_DOSIO_HANDLE_VFS_FILE;
	return 1;
}

void np2_dosio_detach_vfs_file(void)
{
	int close_result = 0;

	if (np2_dosio_backend_kind != NP2_DOSIO_HANDLE_VFS_FILE) {
		return;
	}
	if (np2_dosio_fixture_handle.active &&
		np2_dosio_fixture_handle.kind == NP2_DOSIO_HANDLE_VFS_FILE) {
		close_result = close(np2_dosio_fixture_handle.backend.vfs.fd);
		(void)close_result;
	}
	np2_dosio_reset_handle();
	np2_dosio_backend_kind = NP2_DOSIO_HANDLE_NONE;
	np2_dosio_vfs_logical_path[0] = '\0';
	np2_dosio_vfs_physical_path[0] = '\0';
}

static int np2_dosio_is_fixture_path(const OEMCHAR *path)
{
	return (path != NULL) &&
		(np2_dosio_backend_kind == NP2_DOSIO_HANDLE_RAW_MEMORY) &&
		(np2_dosio_fixture_path[0] != '\0') &&
		(strcmp(path, np2_dosio_fixture_path) == 0);
}

static int np2_dosio_is_vfs_path(const OEMCHAR *path)
{
	return (path != NULL) &&
		(np2_dosio_backend_kind == NP2_DOSIO_HANDLE_VFS_FILE) &&
		(np2_dosio_vfs_logical_path[0] != '\0') &&
		(strcmp(path, np2_dosio_vfs_logical_path) == 0);
}

static NP2_DOSIO_HANDLE *np2_dosio_handle(FILEH handle)
{
	if ((handle == FILEH_INVALID) ||
		(handle != (FILEH)&np2_dosio_fixture_handle) ||
		!np2_dosio_fixture_handle.active ||
		(np2_dosio_fixture_handle.kind == NP2_DOSIO_HANDLE_NONE)) {
		return NULL;
	}
	return &np2_dosio_fixture_handle;
}

static int np2_dosio_open_regular(const char *path, struct stat *status)
{
	int fd;
	int open_errno;
	int fstat_result;

	printf("NP2DOSIO_DIAG dosio_open_regular_enter path=%s\n",
		   path != NULL ? path : "(null)");
	fflush(stdout);

	if ((path == NULL) || (status == NULL)) {
		printf("NP2DOSIO_DIAG dosio_open_regular_exit fd=-1 errno=%d\n", EINVAL);
		fflush(stdout);
		return -1;
	}
	printf("NP2DOSIO_DIAG dosio_before_open\n");
	fflush(stdout);
	fd = open(path, O_RDONLY);
	open_errno = errno;
	printf("NP2DOSIO_DIAG dosio_after_open fd=%d errno=%d\n", fd, open_errno);
	fflush(stdout);
	if (fd < 0) {
		printf("NP2DOSIO_DIAG dosio_open_regular_exit fd=-1 errno=%d\n", open_errno);
		fflush(stdout);
		return -1;
	}
	printf("NP2DOSIO_DIAG dosio_before_fstat\n");
	fflush(stdout);
	fstat_result = fstat(fd, status);
	printf("NP2DOSIO_DIAG dosio_after_fstat rc=%d errno=%d size=%lld mode=%o\n",
		   fstat_result, errno, (long long)status->st_size,
		   (unsigned int)status->st_mode);
	fflush(stdout);
	if ((fstat_result != 0) || !S_ISREG(status->st_mode)) {
		printf("NP2DOSIO_DIAG dosio_before_close_failure\n");
		fflush(stdout);
		close(fd);
		printf("NP2DOSIO_DIAG dosio_open_regular_exit fd=-1 errno=%d\n",
		   fstat_result != 0 ? errno : EINVAL);
		fflush(stdout);
		return -1;
	}
	printf("NP2DOSIO_DIAG dosio_open_regular_exit fd=%d errno=0\n", fd);
	fflush(stdout);
	return fd;
}

static int np2_dosio_type_max(size_t size, uintmax_t *maximum)
{
	if (maximum == NULL) {
		return 0;
	}
	if (size == sizeof(int32_t)) {
		*maximum = (uintmax_t)INT32_MAX;
		return 1;
	}
	if (size == sizeof(int64_t)) {
		*maximum = (uintmax_t)INT64_MAX;
		return 1;
	}
	return 0;
}

static int np2_dosio_file_size(const struct stat *status, FILELEN *size)
{
	uintmax_t filelen_max;
	uintmax_t filepos_max;
	uintmax_t value;

	if ((status == NULL) || (size == NULL) || (status->st_size < 0) ||
		!np2_dosio_type_max(sizeof(FILELEN), &filelen_max) ||
		!np2_dosio_type_max(sizeof(FILEPOS), &filepos_max)) {
		return 0;
	}
	value = (uintmax_t)status->st_size;
	if ((value > filelen_max) || (value > filepos_max)) {
		return 0;
	}
	*size = (FILELEN)value;
	return 1;
}

static int np2_dosio_seek_target(FILELEN size, FILEPOS pointer,
							 int method, FILEPOS *target)
{
	uintmax_t magnitude;
	uintmax_t file_size;

	if (target == NULL) {
		return 0;
	}
	file_size = (uintmax_t)size;
	if (method == FSEEK_SET) {
		if (pointer < 0 || (uintmax_t)pointer > file_size) {
			return 0;
		}
		*target = pointer;
		return 1;
	}
	if (method != FSEEK_END) {
		return 0;
	}
	if (pointer >= 0) {
		/* Positive offsets would be beyond EOF; exact EOF is pointer == 0. */
		if (pointer != 0) {
			return 0;
		}
		*target = size;
		return 1;
	}
	/* This form avoids overflowing when pointer is the minimum FILEPOS. */
	magnitude = (uintmax_t)(-(pointer + 1));
	magnitude += 1;
	if (magnitude > file_size) {
		return 0;
	}
	*target = (FILEPOS)(file_size - magnitude);
	return 1;
}

static int np2_dosio_lseek(FILEH handle, FILEPOS target)
{
	NP2_DOSIO_HANDLE *dosio = np2_dosio_handle(handle);
	off_t result;

	if ((dosio == NULL) || (dosio->kind != NP2_DOSIO_HANDLE_VFS_FILE)) {
		return 0;
	}
	result = lseek(dosio->backend.vfs.fd, (off_t)target, SEEK_SET);
	if (result == (off_t)-1) {
		return 0;
	}
	dosio->backend.vfs.position = target;
	return 1;
}

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
	/* All writes remain unsupported; callers must request read-only access. */
	return FILEH_INVALID;
}

FILEH file_open_rb(const OEMCHAR *path)
{
	struct stat status;
	FILELEN size;
	int fd;

	if (np2_dosio_handle_active()) {
		return FILEH_INVALID;
	}
	if (np2_dosio_is_fixture_path(path)) {
		if (np2_dosio_fixture_data == NULL) {
			return FILEH_INVALID;
		}
		np2_dosio_reset_handle();
		np2_dosio_fixture_handle.kind = NP2_DOSIO_HANDLE_RAW_MEMORY;
		np2_dosio_fixture_handle.backend.raw.data = np2_dosio_fixture_data;
		np2_dosio_fixture_handle.backend.raw.size = np2_dosio_fixture_size;
		np2_dosio_fixture_handle.backend.raw.position = 0;
		np2_dosio_fixture_handle.active = 1;
		return (FILEH)&np2_dosio_fixture_handle;
	}
	if (!np2_dosio_is_vfs_path(path)) {
		return FILEH_INVALID;
	}
	fd = np2_dosio_open_regular(np2_dosio_vfs_physical_path, &status);
	if ((fd < 0) || !np2_dosio_file_size(&status, &size)) {
		if (fd >= 0) close(fd);
		return FILEH_INVALID;
	}
	np2_dosio_reset_handle();
	np2_dosio_fixture_handle.kind = NP2_DOSIO_HANDLE_VFS_FILE;
	np2_dosio_fixture_handle.backend.vfs.fd = fd;
	np2_dosio_fixture_handle.backend.vfs.size = size;
	np2_dosio_fixture_handle.backend.vfs.position = 0;
	np2_dosio_fixture_handle.backend.vfs.last_error = 0;
	np2_dosio_fixture_handle.active = 1;
	return (FILEH)&np2_dosio_fixture_handle;
}

FILEH file_create(const OEMCHAR *path)
{
	(void)path;
	return FILEH_INVALID;
}

FILEPOS file_seek(FILEH handle, FILEPOS pointer, int method)
{
	NP2_DOSIO_HANDLE *dosio = np2_dosio_handle(handle);
	FILEPOS target;

	if (dosio == NULL) {
		return (FILEPOS)-1;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_RAW_MEMORY) {
		if (!np2_dosio_seek_target((FILELEN)dosio->backend.raw.size,
								pointer, method, &target)) {
			return (FILEPOS)-1;
		}
		dosio->backend.raw.position = (size_t)target;
		return target;
	}
	if (dosio->kind != NP2_DOSIO_HANDLE_VFS_FILE ||
		!np2_dosio_seek_target(dosio->backend.vfs.size, pointer, method,
							   &target) || !np2_dosio_lseek(handle, target)) {
		return (FILEPOS)-1;
	}
	return target;
}

UINT file_read(FILEH handle, void *data, UINT length)
{
	NP2_DOSIO_HANDLE *dosio = np2_dosio_handle(handle);
	size_t remaining;

	if ((dosio == NULL) || ((data == NULL) && (length != 0))) {
		return 0;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_RAW_MEMORY) {
		remaining = dosio->backend.raw.size - dosio->backend.raw.position;
		if ((size_t)length > remaining) {
			length = (UINT)remaining;
		}
		if (length != 0) {
			memcpy(data, dosio->backend.raw.data +
						dosio->backend.raw.position, length);
			dosio->backend.raw.position += length;
		}
		return length;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_VFS_FILE) {
		UINT total = 0;
		const FILEPOS remaining_vfs = dosio->backend.vfs.size -
			dosio->backend.vfs.position;

		if ((uintmax_t)length > (uintmax_t)remaining_vfs) {
			length = (UINT)remaining_vfs;
		}
		while (total < length) {
			const ssize_t amount = read(dosio->backend.vfs.fd,
									(unsigned char *)data + total,
									length - total);
			if (amount > 0) {
				total += (UINT)amount;
				dosio->backend.vfs.position += (FILEPOS)amount;
				continue;
			}
			if (amount == 0) {
				break;
			}
			if (errno == EINTR) {
				continue;
			}
			dosio->backend.vfs.last_error = errno;
			break;
		}
		return total;
	}
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
	NP2_DOSIO_HANDLE *dosio = np2_dosio_handle(handle);
	int result = 0;

	if (dosio == NULL) {
		return (handle == FILEH_INVALID) ? 0 : -1;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_VFS_FILE) {
		result = close(dosio->backend.vfs.fd);
	}
	np2_dosio_reset_handle();
	return (result == 0) ? 0 : -1;
}

FILELEN file_getsize(FILEH handle)
{
	NP2_DOSIO_HANDLE *dosio = np2_dosio_handle(handle);

	if (dosio == NULL) {
		return 0;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_RAW_MEMORY) {
		return (FILELEN)dosio->backend.raw.size;
	}
	if (dosio->kind == NP2_DOSIO_HANDLE_VFS_FILE) {
		return dosio->backend.vfs.size;
	}
	return 0;
}

short file_delete(const OEMCHAR *path)
{
	(void)path;
	return -1;
}

short file_attr(const OEMCHAR *path)
{
	struct stat status;
	int fd;

	if (np2_dosio_is_fixture_path(path)) {
		return FILEATTR_READONLY;
	}
	if (!np2_dosio_is_vfs_path(path)) {
		return -1;
	}
	fd = np2_dosio_open_regular(np2_dosio_vfs_physical_path, &status);
	if (fd >= 0) {
		close(fd);
		return FILEATTR_READONLY;
	}
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
