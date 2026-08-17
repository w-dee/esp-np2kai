#define _XOPEN_SOURCE 700

#include <compiler.h>
#include <dosio.h>
#include <pccore.h>

#include <cpumem.h>
#include <diskimage/fddfile.h>
#include <scrnmng.h>

#include <execution_controller.h>
#include <taskmng_control.h>
#include <result_v1_parser.h>
#include <stage1_machine_config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STAGE1_IMAGE_SIZE 1261568
#define RESULT_PHYSICAL_ADDRESS 0x29000

static const char *execution_outcome_name(np2_execution_outcome outcome)
{
	switch (outcome) {
		case NP2_EXECUTION_PASS:
			return "PASS";
		case NP2_EXECUTION_FAIL:
			return "FAIL";
		case NP2_EXECUTION_NOT_REACHED:
			return "NOT_REACHED";
		case NP2_EXECUTION_RUNNING_TIMEOUT:
			return "RUNNING_TIMEOUT";
		case NP2_EXECUTION_INVALID:
			return "INVALID";
		case NP2_EXECUTION_HARNESS_ERROR:
			return "HARNESS_ERROR";
		default:
			return "HARNESS_ERROR";
	}
}

static int execution_outcome_status(np2_execution_outcome outcome)
{
	switch (outcome) {
		case NP2_EXECUTION_PASS:
			return 0;
		case NP2_EXECUTION_FAIL:
			return 1;
		case NP2_EXECUTION_NOT_REACHED:
			return 2;
		case NP2_EXECUTION_RUNNING_TIMEOUT:
			return 3;
		case NP2_EXECUTION_INVALID:
			return 4;
		case NP2_EXECUTION_HARNESS_ERROR:
		default:
			return 5;
	}
}

static uint32_t read_u32le(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
			((uint32_t)bytes[1] << 8) |
			((uint32_t)bytes[2] << 16) |
			((uint32_t)bytes[3] << 24);
}

static void print_snapshot_hex(const uint8_t *snapshot)
{
	size_t index;

	fputs("NP2TEST_SNAPSHOT_HEX=", stderr);
	for (index = 0; index < NP2_RESULT_V1_SIZE; ++index) {
		fprintf(stderr, "%02x", (unsigned)snapshot[index]);
	}
	fputc('\n', stderr);
}

static void print_counter_diagnostics(
		const np2_execution_controller *controller)
{
	fprintf(stderr, "NP2TEST_PRE_RUNNING_SLICES=%u\n",
			(unsigned)controller->pre_running_slices);
	fprintf(stderr, "NP2TEST_RUNNING_SLICES=%u\n",
			(unsigned)controller->running_slices);
}

static void print_framebuffer_failure(void)
{
	SCRNMNG_STATUS status;

	scrnmng_getstatus(&status);
	fprintf(stderr,
			"headless_runner: framebuffer failure requested=%dx%d bytes=%zu\n",
			status.requested_width, status.requested_height, status.bytes);
}

static void print_execution_diagnostics(
		np2_execution_outcome guest_outcome,
		np2_execution_outcome final_outcome,
		int have_guest_outcome,
		int have_snapshot,
		const uint8_t *snapshot,
		const np2_result_v1_result *parsed,
		const np2_execution_controller *controller,
		int cleanup_failed)
{
	if (!have_guest_outcome) {
		fprintf(stderr,
				"NP2TEST_HARNESS_ERROR before first execution slice\n");
		return;
	}

	print_counter_diagnostics(controller);
	switch (guest_outcome) {
		case NP2_EXECUTION_PASS:
			if (have_snapshot) {
				fprintf(stderr,
						"NP2TEST_PASS completed=%u passed=%u failed=%u "
						"stored_crc=0x%08x\n",
						(unsigned)parsed->completed_count,
						(unsigned)parsed->passed_count,
						(unsigned)parsed->failed_count,
						(unsigned)read_u32le(snapshot + NP2_RESULT_V1_CRC_OFFSET));
			}
			break;
		case NP2_EXECUTION_FAIL:
			if (have_snapshot) {
				size_t index;

				fprintf(stderr,
						"NP2TEST_FAIL first_failed_id=0x%04x completed=%u "
						"passed=%u failed=%u diagnostic_length=%u "
						"diagnostic_hex=",
						(unsigned)parsed->first_failed_id,
						(unsigned)parsed->completed_count,
						(unsigned)parsed->passed_count,
						(unsigned)parsed->failed_count,
						(unsigned)parsed->diagnostic_length);
				for (index = 0; index < parsed->diagnostic_length; ++index) {
					fprintf(stderr, "%02x", (unsigned)parsed->diagnostic[index]);
				}
				fputc('\n', stderr);
			}
			break;
		case NP2_EXECUTION_INVALID:
		case NP2_EXECUTION_NOT_REACHED:
		case NP2_EXECUTION_RUNNING_TIMEOUT:
		case NP2_EXECUTION_HARNESS_ERROR:
			if (have_snapshot) {
				fprintf(stderr, "NP2TEST_%s raw_state=0x%02x\n",
						execution_outcome_name(guest_outcome),
						(unsigned)snapshot[NP2_RESULT_V1_STATE_OFFSET]);
				print_snapshot_hex(snapshot);
			}
			break;
		default:
			fprintf(stderr, "NP2TEST_HARNESS_ERROR invalid final controller state\n");
			break;
	}

	if (cleanup_failed) {
		fprintf(stderr,
				"guest outcome=%s; final outcome=HARNESS_ERROR due cleanup failure\n",
				execution_outcome_name(guest_outcome));
	} else if (final_outcome != guest_outcome) {
		fprintf(stderr,
				"guest outcome=%s; final outcome=%s\n",
				execution_outcome_name(guest_outcome),
				execution_outcome_name(final_outcome));
	}
}

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

int main(int argc, char **argv)
{
	char *canonical_path = NULL;
	char temp_template[] = "/tmp/esp-np2kai-headless-XXXXXX";
	char *temp_dir = NULL;
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result parsed;
	np2_execution_controller controller;
	np2_execution_outcome guest_outcome = NP2_EXECUTION_HARNESS_ERROR;
	np2_execution_outcome final_outcome = NP2_EXECUTION_HARNESS_ERROR;
	np2_execution_outcome outcome;
	np2_result_v1_observation observation;
	UINT stored_ftype = 0;
	int stored_readonly = 0;
	int core_initialized = 0;
	int have_snapshot = 0;
	int have_guest_outcome = 0;
	int cleanup_failed = 0;
	bool task_exit;

	memset(&parsed, 0, sizeof(parsed));

	if (argc != 2) {
		fprintf(stderr, "usage: headless_runner <stage1-image>\n");
		return 64;
	}
	if (!validate_input(argv[1], &canonical_path)) {
		return 66;
	}

	temp_dir = mkdtemp(temp_template);
	if (!temp_dir) {
		fprintf(stderr, "headless_runner: cannot create isolated CWD\n");
		goto runner_cleanup;
	}
	if (chdir(temp_dir) != 0) {
		fprintf(stderr, "headless_runner: cannot enter isolated CWD\n");
		goto runner_cleanup;
	}
	np2_stage1_configure_machine();
	np2_host_taskmng_reset();
	pccore_init();
	core_initialized = 1;
	pccore_reset();
	if (!scrnmng_initialize()) {
		print_framebuffer_failure();
		goto core_cleanup;
	}

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

	np2_execution_controller_init(&controller);
	outcome = NP2_EXECUTION_CONTINUE;
	while (outcome == NP2_EXECUTION_CONTINUE) {
		pccore_exec(FALSE);
		if (scrnmng_haserror()) {
			print_framebuffer_failure();
			goto core_cleanup;
		}
		memcpy(snapshot, mem + RESULT_PHYSICAL_ADDRESS, sizeof(snapshot));
		have_snapshot = 1;
		observation = np2_result_v1_parse(snapshot, sizeof(snapshot), &parsed);
		task_exit = np2_host_taskmng_exit_requested() != FALSE;
		outcome = np2_execution_controller_step(&controller, observation, task_exit);
	}
	guest_outcome = outcome;
	final_outcome = guest_outcome;
	have_guest_outcome = 1;

core_cleanup:
	if (core_initialized) {
		pccore_term();
	}
	scrnmng_shutdown();
	if (temp_dir && chdir("/") != 0) {
		fprintf(stderr, "headless_runner: cannot leave isolated CWD\n");
		cleanup_failed = 1;
	}
	if (temp_dir && rmdir(temp_dir) != 0) {
		fprintf(stderr, "headless_runner: isolated CWD cleanup failed\n");
		cleanup_failed = 1;
	}
	if (cleanup_failed) {
		final_outcome = NP2_EXECUTION_HARNESS_ERROR;
	}
	print_execution_diagnostics(guest_outcome, final_outcome,
			have_guest_outcome, have_snapshot, snapshot, &parsed, &controller,
			cleanup_failed);
	fprintf(stdout, "NP2TEST_RESULT=%s\n",
			execution_outcome_name(final_outcome));
	fflush(stdout);
	free(canonical_path);
	return execution_outcome_status(final_outcome);

runner_cleanup:
	goto core_cleanup;
}
