#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "np2_fixture.h"
#include <compiler.h>
#include <dosio.h>
#include <diskimage/fddfile.h>

#include "np2_fixture_probe.h"

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

    handle = file_open_rb(NP2_FIXTURE_PATH);
    if (handle == FILEH_INVALID ||
        file_getsize(handle) != (FILELEN)NP2_FIXTURE_IMAGE_SIZE ||
        file_read(handle, first, sizeof(first)) != sizeof(first) ||
        memcmp(first, mapped, sizeof(first)) != 0 ||
        file_seek(handle, (FILEPOS)(NP2_FIXTURE_IMAGE_SIZE - sizeof(last)),
                  FSEEK_SET) !=
            (FILEPOS)(NP2_FIXTURE_IMAGE_SIZE - sizeof(last)) ||
        file_read(handle, last, sizeof(last)) != sizeof(last) ||
        memcmp(last, mapped + NP2_FIXTURE_IMAGE_SIZE - sizeof(last),
               sizeof(last)) != 0 || file_close(handle) != 0) {
        if (handle != FILEH_INVALID) {
            file_close(handle);
        }
        return 0;
    }
    fputs("NP2FIXTURE dosio_path=" NP2_FIXTURE_PATH " dosio_read=1 first16=",
          stdout);
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
    np2_fixture fixture;
    esp_err_t error;

    error = np2_fixture_acquire(&fixture);
    if (error != ESP_OK) {
        return np2fixture_fail(np2_fixture_error_name(error));
    }
    printf("NP2FIXTURE partition_type=0x%02x partition_subtype=0x%02x "
           "label=%s offset=0x%08lx partition_size=%lu readonly=%d\n",
           (unsigned)fixture.partition->type,
           (unsigned)fixture.partition->subtype,
           fixture.partition->label,
           (unsigned long)fixture.partition->address,
           (unsigned long)fixture.partition->size,
           fixture.partition->readonly ? 1 : 0);
    printf("NP2FIXTURE image_size=%u\n", (unsigned)NP2_FIXTURE_IMAGE_SIZE);
    np2fixture_print_sha256(fixture.digest);
    printf("NP2FIXTURE map_ptr=%p map_size=%u map_handle=%lu "
           "flash_mapped=1\n",
           fixture.mapped, (unsigned)NP2_FIXTURE_IMAGE_SIZE,
           (unsigned long)fixture.map_handle);

    error = np2_fixture_attach_dosio(&fixture);
    if (error != ESP_OK) {
        np2_fixture_release(&fixture);
        return np2fixture_fail("dosio_attach_failed");
    }
    if (!np2fixture_verify_dosio_bytes(fixture.mapped)) {
        np2_fixture_release(&fixture);
        return np2fixture_fail("dosio_read_failed");
    }

    /* No CPU lifecycle is called by this probe. */
    fddfile_initialize();
    error = np2_fixture_attach_fdd(&fixture);
    if (error != ESP_OK) {
        np2_fixture_release(&fixture);
        return np2fixture_fail(np2_fixture_error_name(error));
    }
    printf("NP2FIXTURE geometry_tracks=%u sectors=%u n=%u disktype=%u "
           "disk_type=%u read_only=1 cpu_lifecycle=0\n",
           (unsigned)fddfile[0].inf.xdf.tracks,
           (unsigned)fddfile[0].inf.xdf.sectors,
           (unsigned)fddfile[0].inf.xdf.n,
           (unsigned)fddfile[0].inf.xdf.disktype,
           (unsigned)fddfile[0].type);
    np2_fixture_release(&fixture);
    printf("NP2FIXTURE_RESULT=PASS\n");
    return ESP_OK;
}
