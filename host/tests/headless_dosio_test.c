#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <compiler.h>
#include <dosio.h>

static int check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "headless_dosio_test: %s\n", message);
		return 0;
	}
	return 1;
}

static int make_path(char *path, size_t capacity, const char *directory,
	const char *name)
{
	int written = snprintf(path, capacity, "%s/%s", directory, name);
	return written >= 0 && (size_t)written < capacity;
}

int main(void)
{
	char directory_template[] = "build/headless-dosio-test.XXXXXX";
	char *directory;
	char data_path[MAX_PATH];
	char truncate_path[MAX_PATH];
	char utf8_path[MAX_PATH];
	char path[MAX_PATH];
	char cut[MAX_PATH];
	char readback[32];
	FILEH handle;
	UINT read_size;
	struct stat status;
	int ok = 1;

	directory = mkdtemp(directory_template);
	if (!check(directory != NULL, "mkdtemp failed")) {
		return 1;
	}
	ok &= check(make_path(data_path, sizeof(data_path), directory, "data.bin"),
		"data path is too long");
	ok &= check(make_path(truncate_path, sizeof(truncate_path), directory,
		"truncate.bin"), "truncate path is too long");
	ok &= check(make_path(utf8_path, sizeof(utf8_path), directory,
		"日本語.bin"), "UTF-8 path is too long");
	if (!ok) {
		rmdir(directory);
		return 1;
	}

	ok &= check(file_attr(data_path) == -1,
		"missing file_attr must return -1");
	ok &= check(file_attr(directory) & FILEATTR_DIRECTORY,
		"directory attribute is missing");

	handle = file_create(data_path);
	ok &= check(handle != FILEH_INVALID, "file_create on missing file failed");
	if (handle != FILEH_INVALID) {
		ok &= check(file_write(handle, "abcdefgh", 8) == 8,
			"initial write returned a short count");
		ok &= check(file_getsize(handle) == 8,
			"file_getsize returned the wrong size");
		ok &= check(file_seek(handle, 0, FSEEK_SET) == 0,
			"file_seek to the beginning failed");
		memset(readback, 0, sizeof(readback));
		read_size = file_read(handle, readback, 8);
		ok &= check(read_size == 8 && memcmp(readback, "abcdefgh", 8) == 0,
			"readback did not match the written bytes");
		ok &= check(file_close(handle) == 0, "file_close failed");
	}

	handle = file_open_rb(data_path);
	ok &= check(handle != FILEH_INVALID, "file_open_rb failed");
	if (handle != FILEH_INVALID) {
		memset(readback, 0, sizeof(readback));
		ok &= check(file_read(handle, readback, 8) == 8 &&
			memcmp(readback, "abcdefgh", 8) == 0,
			"read-only reopen did not preserve bytes");
		file_close(handle);
	}

	handle = file_create(data_path);
	ok &= check(handle != FILEH_INVALID, "file_create did not reopen");
	if (handle != FILEH_INVALID) {
		ok &= check(file_getsize(handle) == 0,
			"file_create did not truncate the existing file");
		file_close(handle);
	}

	handle = file_create(truncate_path);
	ok &= check(handle != FILEH_INVALID, "truncate setup failed");
	if (handle != FILEH_INVALID) {
		ok &= check(file_write(handle, "0123456789", 10) == 10,
			"truncate setup write failed");
		ok &= check(file_seek(handle, 4, FSEEK_SET) == 4,
			"truncate seek failed");
		ok &= check(file_write(handle, NULL, 0) == 0,
			"zero-length write did not return zero");
		ok &= check(file_getsize(handle) == 4,
			"zero-length write did not truncate at the current position");
		file_close(handle);
	}
	handle = file_open_rb(truncate_path);
	ok &= check(handle != FILEH_INVALID, "truncated file reopen failed");
	if (handle != FILEH_INVALID) {
		memset(readback, 0, sizeof(readback));
		ok &= check(file_read(handle, readback, 4) == 4 &&
			memcmp(readback, "0123", 4) == 0,
			"zero-length truncate changed retained bytes");
		file_close(handle);
	}

	handle = file_open(data_path);
	ok &= check(handle != FILEH_INVALID, "file_open failed for existing file");
	if (handle != FILEH_INVALID) {
		file_close(handle);
	}
	ok &= check(chmod(data_path, 0444) == 0, "chmod read-only setup failed");
	ok &= check((file_attr(data_path) & FILEATTR_READONLY) != 0,
		"read-only attribute is missing");
	handle = file_open_rb(data_path);
	ok &= check(handle != FILEH_INVALID, "file_open_rb failed on read-only file");
	if (handle != FILEH_INVALID) {
		file_close(handle);
	}
	handle = file_open(data_path);
	ok &= check(handle != FILEH_INVALID,
		"file_open did not provide the reviewed read-only fallback");
	if (handle != FILEH_INVALID) {
		file_close(handle);
	}
	ok &= check(chmod(data_path, 0644) == 0, "chmod restore failed");

	handle = file_create(utf8_path);
	ok &= check(handle != FILEH_INVALID, "UTF-8 file_create failed");
	if (handle != FILEH_INVALID) {
		ok &= check(file_write(handle, "utf8", 4) == 4,
			"UTF-8 path write failed");
		file_close(handle);
	}
	handle = file_open_rb(utf8_path);
	ok &= check(handle != FILEH_INVALID, "UTF-8 path reopen failed");
	if (handle != FILEH_INVALID) {
		memset(readback, 0, sizeof(readback));
		ok &= check(file_read(handle, readback, 4) == 4 &&
			memcmp(readback, "utf8", 4) == 0,
			"UTF-8 path read failed");
		file_close(handle);
	}

	strcpy(path, "/tmp/name.one.two");
	ok &= check(strcmp(file_getname(path), "name.one.two") == 0,
		"file_getname returned the wrong basename");
	ok &= check(strcmp(file_getext(path), "two") == 0,
		"file_getext returned the wrong extension");
	strcpy(cut, path);
	file_cutname(cut);
	ok &= check(strcmp(cut, "/tmp/") == 0,
		"file_cutname removed the wrong text");
	strcpy(path, ".foo");
	ok &= check(strcmp(file_getext(path), "foo") == 0,
		"file_getext mishandled a leading dot");
	strcpy(path, "/tmp/");
	file_cutseparator(path);
	ok &= check(strcmp(path, "/tmp") == 0,
		"file_cutseparator did not remove one trailing slash");
	strcpy(path, "/");
	file_cutseparator(path);
	ok &= check(strcmp(path, "/") == 0, "file_cutseparator changed root");
	strcpy(path, "./");
	file_cutseparator(path);
	ok &= check(strcmp(path, "./") == 0, "file_cutseparator changed ./");
	strcpy(path, "/tmp");
	file_setseparator(path, MAX_PATH);
	ok &= check(strcmp(path, "/tmp/") == 0,
		"file_setseparator did not append slash");
	file_setseparator(path, MAX_PATH);
	ok &= check(strcmp(path, "/tmp/") == 0,
		"file_setseparator appended twice");
	strcpy(path, "");
	file_setseparator(path, MAX_PATH);
	ok &= check(strcmp(path, "") == 0,
		"file_setseparator changed an empty path");
	strcpy(path, "/tmp/");
	file_catname(path, "name", MAX_PATH);
	ok &= check(strcmp(path, "/tmp/name") == 0,
		"file_catname inserted or omitted text");
	ok &= check(strcmp(file_getcd("unit.txt"), "./unit.txt") == 0,
		"file_getcd did not use the ./ base");

	ok &= check(file_delete(data_path) == 0, "file_delete data failed");
	ok &= check(file_delete(truncate_path) == 0, "file_delete truncate failed");
	ok &= check(file_delete(utf8_path) == 0, "file_delete UTF-8 file failed");
	ok &= check(stat(directory, &status) == 0 && S_ISDIR(status.st_mode),
		"test directory disappeared unexpectedly");
	ok &= check(rmdir(directory) == 0, "test directory cleanup failed");

	return ok ? 0 : 1;
}
