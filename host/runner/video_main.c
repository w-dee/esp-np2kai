#define _XOPEN_SOURCE 700

#include <compiler.h>
#include <cpumem.h>
#include <diskimage/fddfile.h>
#include <dosio.h>
#include <pccore.h>
#include <scrnmng.h>
#include <scrnmng_bmp.h>

#include <np2_sha256.h>
#include <stage1_machine_config.h>
#include <taskmng_control.h>
#include <video_control_v1.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VIDEO_FIXTURE_SIZE 1261568
#define VIDEO_READY_SLICE_LIMIT UINT32_C(4096)
#define VIDEO_POST_READY_SLICE_LIMIT UINT32_C(4)
#define VIDEO_DEFAULT_FIXTURE_ID "np2video-7a3a-text"
#define VIDEO_MAX_FIXTURE_ID_LENGTH 63U
#define VIDEO_MULTIFRAME_MAX_UPDATES 64U

static void print_usage(void)
{
	fprintf(stderr, "usage: video_runner <video-image> [--dump-framebuffer <path.bmp>] "
			"[--fixture-id <id>] [--scene-id <id>] [--multi-frame <count>]\n");
}

static int valid_fixture_id(const char *fixture_id)
{
	size_t index;
	size_t length;

	if (fixture_id == NULL) {
		return 0;
	}
	length = strlen(fixture_id);
	if (length == 0 || length > VIDEO_MAX_FIXTURE_ID_LENGTH) {
		return 0;
	}
	for (index = 0; index < length; ++index) {
		char character = fixture_id[index];

		if (!((character >= 'a' && character <= 'z') ||
				(character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9') ||
				character == '.' || character == '_' || character == '-')) {
			return 0;
		}
	}
	return 1;
}

static int parse_scene_id(const char *argument, uint16_t *scene_id)
{
	char *end;
	unsigned long value;

	if (argument == NULL || argument[0] == '\0') {
		return 0;
	}
	errno = 0;
	value = strtoul(argument, &end, 10);
	if (errno != 0 || *end != '\0' || value > UINT16_MAX) {
		return 0;
	}
	*scene_id = (uint16_t)value;
	return 1;
}

static void print_digest(const uint8_t digest[NP2_SHA256_DIGEST_SIZE],
		char output[NP2_SHA256_DIGEST_SIZE * 2 + 1])
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	for (index = 0; index < NP2_SHA256_DIGEST_SIZE; ++index) {
		output[index * 2] = digits[digest[index] >> 4];
		output[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	output[NP2_SHA256_DIGEST_SIZE * 2] = '\0';
}

static int hash_file(const char *path,
		uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
	np2_sha256_context context;
	uint8_t buffer[4096];
	FILE *file;

	file = fopen(path, "rb");
	if (file == NULL) {
		return 0;
	}
	np2_sha256_init(&context);
	for (;;) {
		size_t length = fread(buffer, 1, sizeof(buffer), file);

		if (length != 0) {
			np2_sha256_update(&context, buffer, length);
		}
		if (length != sizeof(buffer)) {
			if (ferror(file)) {
				fclose(file);
				return 0;
			}
			break;
		}
	}
	if (fclose(file) != 0) {
		return 0;
	}
	np2_sha256_final(&context, digest);
	return 1;
}

static int validate_input(const char *argument, char **canonical_out,
		uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
	struct stat image_stat;
	char *canonical_path;

	canonical_path = realpath(argument, NULL);
	if (canonical_path == NULL) {
		fprintf(stderr, "video_runner: image path cannot be resolved\n");
		return 0;
	}
	if (strlen(canonical_path) >= MAX_PATH) {
		fprintf(stderr, "video_runner: image path is too long\n");
		free(canonical_path);
		return 0;
	}
	if (stat(canonical_path, &image_stat) != 0) {
		fprintf(stderr, "video_runner: image cannot be stat'ed\n");
		free(canonical_path);
		return 0;
	}
	if (!S_ISREG(image_stat.st_mode)) {
		fprintf(stderr, "video_runner: image is not a regular file\n");
		free(canonical_path);
		return 0;
	}
	if (access(canonical_path, R_OK) != 0) {
		fprintf(stderr, "video_runner: image is not readable\n");
		free(canonical_path);
		return 0;
	}
	if (image_stat.st_size != (off_t)VIDEO_FIXTURE_SIZE) {
		fprintf(stderr, "video_runner: image has the wrong size\n");
		free(canonical_path);
		return 0;
	}
	if (!hash_file(canonical_path, digest)) {
		fprintf(stderr, "video_runner: image SHA-256 failed\n");
		free(canonical_path);
		return 0;
	}
	*canonical_out = canonical_path;
	return 1;
}

static int make_absolute_path(const char *argument, char **absolute_out)
{
	char *cwd;
	char *absolute;
	size_t cwd_length;
	size_t argument_length;

	if (argument == NULL || argument[0] == '\0') {
		return 0;
	}
	if (argument[0] == '/') {
		absolute = strdup(argument);
		if (absolute == NULL) {
			return 0;
		}
		*absolute_out = absolute;
		return 1;
	}
	cwd = getcwd(NULL, 0);
	if (cwd == NULL) {
		return 0;
	}
	cwd_length = strlen(cwd);
	argument_length = strlen(argument);
	absolute = (char *)malloc(cwd_length + 1 + argument_length + 1);
	if (absolute == NULL) {
		free(cwd);
		return 0;
	}
	memcpy(absolute, cwd, cwd_length);
	absolute[cwd_length] = '/';
	memcpy(absolute + cwd_length + 1, argument, argument_length + 1);
	free(cwd);
	*absolute_out = absolute;
	return 1;
}

static int verify_fdd_attachment(const char *canonical_path)
{
	UINT stored_ftype = 0;
	int stored_readonly = 0;

	if (fdd_set(0, canonical_path, FTYPE_NONE, 1) != SUCCESS) {
		fprintf(stderr, "video_runner: FDD attach failed\n");
		return 0;
	}
	if (!fdd_diskready(0)) {
		fprintf(stderr, "video_runner: FDD is not ready after attach\n");
		return 0;
	}
	if (strcmp(fdd_getfileex(0, &stored_ftype, &stored_readonly), canonical_path) != 0 ||
			stored_readonly != 1 || stored_ftype != FTYPE_NONE) {
		fprintf(stderr, "video_runner: attached file metadata mismatch\n");
		return 0;
	}
	if (strcmp(np2cfg.fddfile[0], canonical_path) != 0) {
		fprintf(stderr, "video_runner: configuration path mismatch\n");
		return 0;
	}
	if (fddfile[0].type != DISKTYPE_BETA ||
			fddfile[0].inf.xdf.tracks != 154 ||
			fddfile[0].inf.xdf.sectors != 8 ||
			fddfile[0].inf.xdf.n != 3 ||
			fddfile[0].inf.xdf.disktype != DISKTYPE_2HD) {
		fprintf(stderr, "video_runner: attached image geometry mismatch\n");
		return 0;
	}
	return 1;
}

static int has_np2v_magic(const uint8_t *bytes)
{
	return bytes != NULL && bytes[0] == (uint8_t)'N' &&
			bytes[1] == (uint8_t)'P' && bytes[2] == (uint8_t)'2' &&
			bytes[3] == (uint8_t)'V';
}

static int validate_framebuffer(const SCRNMNG_SNAPSHOT *snapshot,
		uint32_t generation, uint32_t sequence)
{
	if (snapshot == NULL || snapshot->width != 640 || snapshot->height != 400 ||
			snapshot->bpp != 16 || snapshot->pixel_format != SCRNMNG_PIXEL_FORMAT_RGB565LE ||
		snapshot->pitch != 1280 || snapshot->visible_bytes != 512000 ||
		snapshot->surface_generation != generation ||
		snapshot->surface_update_sequence <= sequence) {
		return 0;
	}
	return 1;
}

static void print_framebuffer(const char *fixture_id, uint16_t scene_id,
		const SCRNMNG_SNAPSHOT *snapshot)
{
	SCRNMNG_STATUS status;

	scrnmng_getstatus(&status);
	fprintf(stdout,
			"NP2VIDEO_FRAMEBUFFER fixture_id=%s scene_id=%u width=%d height=%d bytes=%zu "
			"format=rgb565le bpp=%u pitch=%zu generation=%u "
			"surface_update_sequence=%u crc_algorithm=crc32_iso_hdlc "
			"crc32=0x%08x storage_external=%d\n",
			fixture_id, (unsigned)scene_id, snapshot->width, snapshot->height,
			snapshot->visible_bytes,
			(unsigned)snapshot->bpp, snapshot->pitch,
			(unsigned)snapshot->surface_generation,
			(unsigned)snapshot->surface_update_sequence,
			(unsigned)snapshot->crc32, status.external ? 1 : 0);
}

int main(int argc, char **argv)
{
	char *canonical_path = NULL;
	char *bmp_path = NULL;
	const char *fixture_id = VIDEO_DEFAULT_FIXTURE_ID;
	uint16_t scene_id = NP2V_CONTROL_SCENE_ID;
	uint32_t multi_frame_count = 0;
	char temp_template[] = "/tmp/esp-np2kai-video-XXXXXX";
	char *temp_dir = NULL;
	const char *failure = "unknown";
	uint8_t digest[NP2_SHA256_DIGEST_SIZE];
	char digest_text[NP2_SHA256_DIGEST_SIZE * 2 + 1];
	SCRNMNG_SNAPSHOT ready_snapshot;
	SCRNMNG_SNAPSHOT final_snapshot;
	np2v_control control;
	np2v_control_status control_status;
	uint32_t ready_generation = 0;
	uint32_t ready_sequence = 0;
	uint32_t slice;
	int core_initialized = 0;
	int framebuffer_initialized = 0;
	int ready = 0;
	int success = 0;

	int argument_index;

	if (argc < 2) {
		print_usage();
		return 64;
	}
	for (argument_index = 2; argument_index < argc; ++argument_index) {
		if (strcmp(argv[argument_index], "--dump-framebuffer") == 0) {
			if (++argument_index >= argc || bmp_path != NULL ||
					!make_absolute_path(argv[argument_index], &bmp_path)) {
				print_usage();
				return 64;
			}
		} else if (strcmp(argv[argument_index], "--fixture-id") == 0) {
			if (++argument_index >= argc || !valid_fixture_id(argv[argument_index])) {
				print_usage();
				return 64;
			}
			fixture_id = argv[argument_index];
		} else if (strcmp(argv[argument_index], "--scene-id") == 0) {
			if (++argument_index >= argc ||
					!parse_scene_id(argv[argument_index], &scene_id)) {
				print_usage();
				return 64;
			}
		} else if (strcmp(argv[argument_index], "--multi-frame") == 0) {
			uint16_t multi_frame_value;
			if (++argument_index >= argc ||
					!parse_scene_id(argv[argument_index], &multi_frame_value)) {
				print_usage();
				return 64;
			}
			multi_frame_count = (uint32_t)multi_frame_value;
			if (multi_frame_count == 0U ||
					multi_frame_count > VIDEO_MULTIFRAME_MAX_UPDATES) {
				print_usage();
				return 64;
			}
		} else {
			print_usage();
			return 64;
		}
	}
	if (!validate_input(argv[1], &canonical_path, digest)) {
		free(bmp_path);
		return 66;
	}
	print_digest(digest, digest_text);

	temp_dir = mkdtemp(temp_template);
	if (temp_dir == NULL) {
		failure = "cannot create isolated CWD";
		goto cleanup;
	}
	if (chdir(temp_dir) != 0) {
		failure = "cannot enter isolated CWD";
		goto cleanup;
	}
	np2_stage1_configure_machine();
	np2_host_taskmng_reset();
	pccore_init();
	core_initialized = 1;
	pccore_reset();
	if (!scrnmng_initialize()) {
		failure = "framebuffer initialization failed";
		goto cleanup;
	}
	framebuffer_initialized = 1;
	if (!verify_fdd_attachment(canonical_path)) {
		failure = "FDD attachment failed";
		goto cleanup;
	}

	/* PRE-READY: advance guest/events while explicitly suppressing rendering. */
	for (slice = 0; slice < VIDEO_READY_SLICE_LIMIT; ++slice) {
		pccore_exec(FALSE);
		if (scrnmng_haserror()) {
			failure = "framebuffer failure during PRE-READY";
			goto cleanup;
		}
		control_status = np2v_control_parse_for_scene(
				mem + NP2V_CONTROL_PHYSICAL_ADDRESS, NP2V_CONTROL_SIZE,
				scene_id, &control);
		/* Before the guest publishes its magic, the control address is ordinary
		 * uninitialized guest RAM and must not be treated as a protocol fault. */
		if (control_status == NP2V_CONTROL_INVALID &&
				!has_np2v_magic(mem + NP2V_CONTROL_PHYSICAL_ADDRESS)) {
			continue;
		}
		if (control_status == NP2V_CONTROL_INVALID) {
			failure = "malformed NP2V control block";
			goto cleanup;
		}
		if (control_status == NP2V_CONTROL_VALID && control.state == NP2V_STATE_ERROR) {
			fprintf(stderr, "video_runner: guest NP2V ERROR diagnostic=0x%04x\n",
					(unsigned)control.diagnostic);
			failure = "guest reported NP2V ERROR";
			goto cleanup;
		}
		if (control_status == NP2V_CONTROL_VALID && control.state == NP2V_STATE_SCENE_READY) {
			ready = 1;
			break;
		}
		if (np2_host_taskmng_exit_requested() != FALSE) {
			failure = "host exit requested during PRE-READY";
			goto cleanup;
		}
	}
	if (!ready) {
		failure = "SCENE_READY was not observed within PRE-READY limit";
		goto cleanup;
	}
	if (scrnmng_snapshot(&ready_snapshot) != SCRNMNG_SNAPSHOT_OK ||
			ready_snapshot.width != 640 || ready_snapshot.height != 400 ||
			ready_snapshot.visible_bytes != 512000) {
		failure = "invalid framebuffer at SCENE_READY";
		goto cleanup;
	}
	ready_generation = ready_snapshot.surface_generation;
	ready_sequence = ready_snapshot.surface_update_sequence;
	fprintf(stdout,
			"NP2VIDEO_FIXTURE fixture_id=%s scene_id=%u fixture_sha256=%s image_bytes=%d\n",
			fixture_id, (unsigned)scene_id, digest_text, VIDEO_FIXTURE_SIZE);
	fprintf(stdout,
			"NP2VIDEO_READY fixture_id=%s scene_id=%u state=SCENE_READY generation=%u "
			"surface_update_sequence=%u\n",
			fixture_id, (unsigned)scene_id,
			(unsigned)ready_generation, (unsigned)ready_sequence);

	/* POST-READY: enable rendering and require a new update in the same surface. */
	for (slice = 0; slice < VIDEO_POST_READY_SLICE_LIMIT; ++slice) {
		pccore_exec(TRUE);
		if (scrnmng_haserror()) {
			failure = "framebuffer failure during POST-READY";
			goto cleanup;
		}
		control_status = np2v_control_parse_for_scene(
				mem + NP2V_CONTROL_PHYSICAL_ADDRESS, NP2V_CONTROL_SIZE,
				scene_id, &control);
		if (control_status != NP2V_CONTROL_VALID ||
				control.state != NP2V_STATE_SCENE_READY) {
			failure = "NP2V state changed after SCENE_READY";
			goto cleanup;
		}
		if (scrnmng_snapshot(&final_snapshot) != SCRNMNG_SNAPSHOT_OK) {
			failure = "framebuffer snapshot failed after SCENE_READY";
			goto cleanup;
		}
		if (final_snapshot.surface_generation != ready_generation) {
			failure = "surface generation changed after SCENE_READY";
			goto cleanup;
		}
		if (final_snapshot.surface_update_sequence > ready_sequence) {
			break;
		}
	}
	if (final_snapshot.surface_update_sequence <= ready_sequence ||
			!validate_framebuffer(&final_snapshot, ready_generation, ready_sequence)) {
		failure = "no valid post-ready framebuffer update was observed";
		goto cleanup;
	}
	if (multi_frame_count != 0U) {
		uint32_t observed_updates = 0;
		uint32_t distinct_frames = 0;
		uint32_t last_sequence = final_snapshot.surface_update_sequence;
		uint32_t frame_crc[VIDEO_MULTIFRAME_MAX_UPDATES];
		uint32_t slice_limit = 0;

		memset(frame_crc, 0, sizeof(frame_crc));
		while (observed_updates < multi_frame_count &&
				slice_limit++ < VIDEO_READY_SLICE_LIMIT) {
			uint32_t index;

			pccore_exec(TRUE);
			if (scrnmng_haserror() ||
					scrnmng_snapshot(&final_snapshot) != SCRNMNG_SNAPSHOT_OK) {
				failure = "multi-frame framebuffer failure";
				goto cleanup;
			}
			if (final_snapshot.surface_generation != ready_generation) {
				failure = "multi-frame surface generation changed";
				goto cleanup;
			}
			if (final_snapshot.surface_update_sequence <= last_sequence) {
				continue;
			}
			last_sequence = final_snapshot.surface_update_sequence;
			++observed_updates;
			for (index = 0; index < distinct_frames; ++index) {
				if (frame_crc[index] == final_snapshot.crc32) {
					break;
				}
			}
			if (index == distinct_frames) {
				frame_crc[distinct_frames++] = final_snapshot.crc32;
			}
		}
		fprintf(stdout,
				"NP2VIDEO_MULTIFRAME updates=%u distinct=%u generation=%u "
				"last_sequence=%u framebuffer_errors=%d\n",
				(unsigned)observed_updates, (unsigned)distinct_frames,
				(unsigned)final_snapshot.surface_generation,
				(unsigned)final_snapshot.surface_update_sequence,
				scrnmng_haserror() ? 1 : 0);
		if (observed_updates < multi_frame_count || distinct_frames < 16U) {
			failure = "multi-frame distinct-update proof failed";
			goto cleanup;
		}
	}
	print_framebuffer(fixture_id, scene_id, &final_snapshot);
	if (bmp_path != NULL) {
		SCRNMNG_BMP_STATUS bmp_status = scrnmng_write_bmp(bmp_path);

		if (bmp_status != SCRNMNG_BMP_OK) {
			fprintf(stderr, "video_runner: BMP write failed: %s\n",
					scrnmng_bmp_status_name(bmp_status));
			failure = "BMP write failed";
			goto cleanup;
		}
		fprintf(stdout, "NP2VIDEO_BMP path=%s\n", bmp_path);
	}
	success = 1;

cleanup:
	if (core_initialized) {
		pccore_term();
	}
	if (framebuffer_initialized) {
		scrnmng_shutdown();
	}
	if (temp_dir != NULL && chdir("/") != 0) {
		failure = "cannot leave isolated CWD";
		success = 0;
	}
	if (temp_dir != NULL && rmdir(temp_dir) != 0) {
		failure = "isolated CWD cleanup failed";
		success = 0;
	}
	if (success) {
		fprintf(stdout, "NP2VIDEO_RESULT=REFERENCE_READY\n");
	} else {
		fprintf(stdout, "NP2VIDEO_RESULT=HARNESS_ERROR reason=%s\n", failure);
	}
	fflush(stdout);
	free(bmp_path);
	free(canonical_path);
	return success ? 0 : 5;
}
