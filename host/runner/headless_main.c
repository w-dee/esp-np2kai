#define _XOPEN_SOURCE 700

#include <compiler.h>
#include <dosio.h>
#include <pccore.h>

#include <diskimage/fddfile.h>

#include <taskmng_control.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STAGE1_IMAGE_SIZE 1261568

static int validate_input(const char *argument, char **canonical_out)
{
	struct stat image_stat;
	char *canonical_path;

	canonical_path = realpath(argument, NULL);
	if (!canonical_path) {
		fprintf(stderr, "headless_runner: image path cannot be resolved\n");
		return 0;
	}
	if (strlen(canonical_path) >= MAX_PATH) {
		fprintf(stderr, "headless_runner: image path is too long\n");
		free(canonical_path);
		return 0;
	}
	if (stat(canonical_path, &image_stat) != 0) {
		fprintf(stderr, "headless_runner: image cannot be stat'ed\n");
		free(canonical_path);
		return 0;
	}
	if (!S_ISREG(image_stat.st_mode)) {
		fprintf(stderr, "headless_runner: image is not a regular file\n");
		free(canonical_path);
		return 0;
	}
	if (access(canonical_path, R_OK) != 0) {
		fprintf(stderr, "headless_runner: image is not readable\n");
		free(canonical_path);
		return 0;
	}
	if (image_stat.st_size != (off_t)STAGE1_IMAGE_SIZE) {
		fprintf(stderr, "headless_runner: image has the wrong size\n");
		free(canonical_path);
		return 0;
	}

	*canonical_out = canonical_path;
	return 1;
}

static void configure_stage1_machine(void)
{
	static const UINT8 stage1_dipsw[3] = {0x3e, 0xe3, 0x7b};
	static const UINT8 stage1_memsw[8] = {0x48, 0x05, 0x04, 0x08, 0x01, 0x00, 0x00, 0x6e};
	static const UINT8 stage1_wait[6] = {1, 1, 6, 1, 8, 1};
	unsigned i;

	file_cpyname(np2cfg.model, OEMTEXT("VX"), NELEMENTS(np2cfg.model));
	np2cfg.baseclock = PCBASECLOCK25;
	np2cfg.multiple = 20;
	for (i = 0; i < 3; ++i) {
		np2cfg.dipsw[i] = stage1_dipsw[i];
	}
	for (i = 0; i < 8; ++i) {
		np2cfg.memsw[i] = stage1_memsw[i];
	}
	np2cfg.EXTMEM = 13;
	np2cfg.fddequip = 3;
	np2cfg.memcheckspeed = 8;
	np2cfg.ITF_WORK = 1;
	np2cfg.emuspeed = 100;
	np2cfg.DISPSYNC = 1;
	for (i = 0; i < 6; ++i) {
		np2cfg.wait[i] = stage1_wait[i];
	}

	np2cfg.usebios = 0;
	np2cfg.biospath[0] = '\0';
	np2cfg.fontfile[0] = '\0';
	np2cfg.fontface[0] = '\0';
	for (i = 0; i < 4; ++i) {
		np2cfg.fddfile[i][0] = '\0';
	}
	for (i = 0; i < 2; ++i) {
		np2cfg.sasihdd[i][0] = '\0';
	}
}

int main(int argc, char **argv)
{
	char *canonical_path = NULL;
	char temp_template[] = "/tmp/esp-np2kai-headless-XXXXXX";
	char *temp_dir = NULL;
	UINT stored_ftype = 0;
	int stored_readonly = 0;
	int status = 3;
	int core_initialized = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: headless_runner <stage1-image>\n");
		return 2;
	}
	if (!validate_input(argv[1], &canonical_path)) {
		return 2;
	}

	temp_dir = mkdtemp(temp_template);
	if (!temp_dir) {
		fprintf(stderr, "headless_runner: cannot create isolated CWD\n");
		free(canonical_path);
		return 3;
	}
	if (chdir(temp_dir) != 0) {
		fprintf(stderr, "headless_runner: cannot enter isolated CWD\n");
		(void)chdir("/");
		(void)rmdir(temp_dir);
		free(canonical_path);
		return 3;
	}

	configure_stage1_machine();
	np2_host_taskmng_reset();
	pccore_init();
	core_initialized = 1;
	pccore_reset();

	if (fdd_set(0, canonical_path, FTYPE_NONE, 1) != SUCCESS) {
		fprintf(stderr, "headless_runner: FDD attach failed\n");
		goto core_cleanup;
	}
	if (!fdd_diskready(0)) {
		fprintf(stderr, "headless_runner: FDD is not ready after attach\n");
		goto core_cleanup;
	}
	if (strcmp(fdd_getfileex(0, &stored_ftype, &stored_readonly), canonical_path) != 0 ||
			stored_readonly != 1 || stored_ftype != FTYPE_NONE) {
		fprintf(stderr, "headless_runner: attached file metadata mismatch\n");
		goto core_cleanup;
	}
	if (strcmp(np2cfg.fddfile[0], canonical_path) != 0) {
		fprintf(stderr, "headless_runner: configuration path mismatch\n");
		goto core_cleanup;
	}
	if (fddfile[0].type != DISKTYPE_BETA ||
			fddfile[0].inf.xdf.tracks != 154 ||
			fddfile[0].inf.xdf.sectors != 8 ||
			fddfile[0].inf.xdf.n != 3 ||
			fddfile[0].inf.xdf.disktype != DISKTYPE_2HD) {
		fprintf(stderr, "headless_runner: attached image geometry mismatch\n");
		goto core_cleanup;
	}

	status = 0;

core_cleanup:
	if (core_initialized) {
		pccore_term();
	}
	if (chdir("/") != 0) {
		fprintf(stderr, "headless_runner: cannot leave isolated CWD\n");
		status = 3;
	}
	if (rmdir(temp_dir) != 0) {
		fprintf(stderr, "headless_runner: isolated CWD cleanup failed\n");
		status = 3;
	}
	free(canonical_path);
	return status;
}
