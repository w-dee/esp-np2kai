#include <stdio.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_err.h"

#include <compiler.h>
#include <dosio.h>
#include <diskimage/fddfile.h>
#include <np2host/dosio_esp.h>

#include "np2_fixture_probe.h"

#define NP2FIXTURE_PARTITION_TYPE ((esp_partition_type_t)0x40)
#define NP2FIXTURE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x01)
#define NP2FIXTURE_PARTITION_LABEL "np2test"
#define NP2FIXTURE_PATH "./np2test-fd1232.hdm"
#define NP2FIXTURE_IMAGE_SIZE 0x134000U
#define NP2FIXTURE_TRACKS 154U
#define NP2FIXTURE_SECTORS 8U
#define NP2FIXTURE_N 3U

static const uint8_t np2fixture_expected_sha256[32] = {
	0x3b, 0x73, 0x66, 0x7d, 0x23, 0x56, 0x15, 0xe8,
	0x92, 0x05, 0xfb, 0xda, 0xb0, 0x4d, 0x3e, 0x6c,
	0xf9, 0xc2, 0xf9, 0xa1, 0xf3, 0xa1, 0xde, 0x82,
	0xcd, 0xb2, 0xb3, 0x86, 0x2a, 0xa3, 0x94, 0xb3,
};

static void np2fixture_print_sha256(const uint8_t *digest)
{
	unsigned index;

	fputs("NP2FIXTURE sha256=", stdout);
	for (index = 0; index < 32; ++index) {
		printf("%02x", (unsigned)digest[index]);
	}
	fputc('\n', stdout);
}

static esp_err_t np2fixture_fail(const char *reason)
{
	printf("NP2FIXTURE_RESULT=FAIL reason=%s\n", reason);
	return ESP_FAIL;
}

static int np2fixture_verify_dosio_bytes(const uint8_t *mapped)
{
	uint8_t first[16];
	uint8_t last[16];
	FILEH handle;
	unsigned index;

	handle = file_open_rb(NP2FIXTURE_PATH);
	if (handle == FILEH_INVALID ||
		file_getsize(handle) != (FILELEN)NP2FIXTURE_IMAGE_SIZE ||
		file_read(handle, first, sizeof(first)) != sizeof(first) ||
		memcmp(first, mapped, sizeof(first)) != 0 ||
		file_seek(handle, (FILEPOS)(NP2FIXTURE_IMAGE_SIZE - sizeof(last)),
			FSEEK_SET) != (FILEPOS)(NP2FIXTURE_IMAGE_SIZE - sizeof(last)) ||
		file_read(handle, last, sizeof(last)) != sizeof(last) ||
		memcmp(last, mapped + NP2FIXTURE_IMAGE_SIZE - sizeof(last),
			sizeof(last)) != 0 || file_close(handle) != 0) {
		if (handle != FILEH_INVALID) {
			file_close(handle);
		}
		return 0;
	}
	fputs("NP2FIXTURE dosio_path=" NP2FIXTURE_PATH " dosio_read=1 "
		  "first16=", stdout);
	for (index = 0; index < sizeof(first); ++index) {
		printf("%02x", (unsigned)first[index]);
	}
	fputs(" last16=", stdout);
	for (index = 0; index < sizeof(last); ++index) {
		printf("%02x", (unsigned)last[index]);
	}
	fputc('\n', stdout);
	return 1;
}

esp_err_t np2_fixture_probe_run(void)
{
	const esp_partition_t *partition;
	const void *mapped = NULL;
	esp_partition_mmap_handle_t map_handle = 0;
	uint8_t digest[32];
	FDDFILE fdd;
	esp_err_t result = ESP_FAIL;

	partition = esp_partition_find_first(NP2FIXTURE_PARTITION_TYPE,
			NP2FIXTURE_PARTITION_SUBTYPE, NP2FIXTURE_PARTITION_LABEL);
	if (partition == NULL) {
		return np2fixture_fail("partition_missing");
	}
	printf("NP2FIXTURE partition_type=0x%02x partition_subtype=0x%02x "
		   "label=%s offset=0x%08lx partition_size=%lu readonly=%d\n",
		   (unsigned)partition->type, (unsigned)partition->subtype,
		   partition->label, (unsigned long)partition->address,
		   (unsigned long)partition->size, partition->readonly ? 1 : 0);
	if (partition->size < NP2FIXTURE_IMAGE_SIZE) {
		return np2fixture_fail("size_mismatch");
	}
	if (!partition->readonly) {
		return np2fixture_fail("partition_not_readonly");
	}

	printf("NP2FIXTURE image_size=%u\n", (unsigned)NP2FIXTURE_IMAGE_SIZE);
	if (esp_partition_get_sha256(partition, digest) != ESP_OK) {
		return np2fixture_fail("sha256_failed");
	}
	np2fixture_print_sha256(digest);
	if (memcmp(digest, np2fixture_expected_sha256, sizeof(digest)) != 0) {
		return np2fixture_fail("sha256_mismatch");
	}

	if (esp_partition_mmap(partition, 0, NP2FIXTURE_IMAGE_SIZE,
			ESP_PARTITION_MMAP_DATA, &mapped, &map_handle) != ESP_OK) {
		return np2fixture_fail("map_failed");
	}
	printf("NP2FIXTURE map_ptr=%p map_size=%u map_handle=%lu flash_mapped=1\n",
		   mapped, (unsigned)NP2FIXTURE_IMAGE_SIZE,
		   (unsigned long)map_handle);

	if (!np2_dosio_attach_fixture(NP2FIXTURE_PATH,
			(const uint8_t *)mapped, NP2FIXTURE_IMAGE_SIZE)) {
		result = np2fixture_fail("dosio_attach_failed");
		goto cleanup;
	}
	if (!np2fixture_verify_dosio_bytes((const uint8_t *)mapped)) {
		result = np2fixture_fail("dosio_read_failed");
		goto cleanup_dosio;
	}

	/* fdd_set() -> fdd_set_xdf() is the retained vendor recognition path.
	 * fddfile_initialize() initializes only the FDD image tables; no pccore
	 * lifecycle function is called here. */
	fddfile_initialize();
	if (fdd_set(0, NP2FIXTURE_PATH, FTYPE_NONE, 1) != SUCCESS) {
		result = np2fixture_fail("fdd_recognition_failed");
		goto cleanup_dosio;
	}
	fdd = fddfile;
	if ((fdd->type != DISKTYPE_BETA) ||
		(fdd->inf.xdf.tracks != NP2FIXTURE_TRACKS) ||
		(fdd->inf.xdf.sectors != NP2FIXTURE_SECTORS) ||
		(fdd->inf.xdf.n != NP2FIXTURE_N) ||
		(fdd->inf.xdf.disktype != DISKTYPE_2HD)) {
		result = np2fixture_fail("geometry_mismatch");
		goto cleanup_fdd;
	}
	printf("NP2FIXTURE geometry_tracks=%u sectors=%u n=%u disktype=%u "
		   "disk_type=%u read_only=1 cpu_lifecycle=0\n",
		   (unsigned)fdd->inf.xdf.tracks,
		   (unsigned)fdd->inf.xdf.sectors,
		   (unsigned)fdd->inf.xdf.n,
		   (unsigned)fdd->inf.xdf.disktype,
		   (unsigned)fdd->type);
	result = ESP_OK;

cleanup_fdd:
	fdd_eject(0);
cleanup_dosio:
	np2_dosio_detach_fixture();
cleanup:
	esp_partition_munmap(map_handle);
	if (result == ESP_OK) {
		printf("NP2FIXTURE_RESULT=PASS\n");
	}
	return result;
}
