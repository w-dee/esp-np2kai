SHELL := /bin/sh

# Explicit compile/link evidence inventories; this is not a native emulator build.
HOST_ROOT := $(abspath $(CURDIR)/..)
VENDOR_ROOT := $(HOST_ROOT)/third_party/np2kai
VENDOR_SRC := $(VENDOR_ROOT)/src
SOURCE_LIST := $(CURDIR)/probe/host_core_sources.txt
HOST_BACKEND_SOURCE_LIST := $(CURDIR)/probe/host_backend_sources.txt
HOST_BACKEND_SOURCE_ROOT := $(HOST_ROOT)
PROBE_SCRIPT := $(CURDIR)/probe/compile_probe.py
LINK_PROBE_SCRIPT := $(CURDIR)/probe/link_probe.py
PREPARER := $(CURDIR)/tools/prepare_step4_sources.py
PATCH_SET := $(CURDIR)/patches/np2kai/step4/patch-set.json
IMPORT_MANIFEST := $(HOST_ROOT)/third_party/np2kai/import-manifest.json
VENDOR_VERIFY := $(HOST_ROOT)/tools/np2kai/verify_np2kai.py
BUILD_DIR ?= $(CURDIR)/build/phase2
PROBE_OUTPUT := $(BUILD_DIR)/compile
PATCH_OUTPUT := $(BUILD_DIR)/patches
MAPPED_PROBE_OUTPUT := $(BUILD_DIR)/compile-mapped
LINK_PROBE_OUTPUT := $(BUILD_DIR)/link-mapped
HEADLESS_MOUSE_SOURCE := $(CURDIR)/backend/headless/mousemng.c
HEADLESS_MOUSE_TEST_SOURCE := $(CURDIR)/tests/headless_mouse_test.c
HEADLESS_MOUSE_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_mouse_test
HEADLESS_SOUND_SOURCE := $(CURDIR)/backend/headless/soundmng.c
HEADLESS_SOUND_TEST_SOURCE := $(CURDIR)/tests/headless_sound_test.c
HEADLESS_SOUND_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_sound_test
HEADLESS_TIMING_SOURCE := $(CURDIR)/backend/headless/timing.c
HEADLESS_TIMING_TEST_SOURCE := $(CURDIR)/tests/headless_timing_test.c
HEADLESS_TIMING_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_timing_test
HEADLESS_TIMEMNG_SOURCE := $(CURDIR)/backend/headless/timemng.c
HEADLESS_TIMEMNG_TEST_SOURCE := $(CURDIR)/tests/headless_timemng_test.c
HEADLESS_TIMEMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_timemng_test
HEADLESS_SYSMNG_SOURCE := $(CURDIR)/backend/headless/sysmng.c
HEADLESS_SYSMNG_TEST_SOURCE := $(CURDIR)/tests/headless_sysmng_test.c
HEADLESS_SYSMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_sysmng_test
HEADLESS_TASKMNG_SOURCE := $(CURDIR)/backend/headless/taskmng.c
HEADLESS_TASKMNG_TEST_SOURCE := $(CURDIR)/tests/headless_taskmng_test.c
HEADLESS_TASKMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_taskmng_test
HEADLESS_RUNNER_SOURCE := $(CURDIR)/runner/headless_main.c
HEADLESS_RUNNER_OBJECT := $(BUILD_DIR)/runner/headless_main.o
HEADLESS_RUNNER_DEPENDENCY := $(BUILD_DIR)/runner/headless_main.d
STAGE1_MACHINE_CONFIG_SOURCE := $(CURDIR)/runner/stage1_machine_config.c
STAGE1_MACHINE_CONFIG_OBJECT := $(BUILD_DIR)/runner/stage1_machine_config.o
STAGE1_MACHINE_CONFIG_DEPENDENCY := $(BUILD_DIR)/runner/stage1_machine_config.d
RESULT_V1_PARSER_OBJECT := $(BUILD_DIR)/runner/result_v1_parser.o
RESULT_V1_PARSER_DEPENDENCY := $(BUILD_DIR)/runner/result_v1_parser.d
EXECUTION_CONTROLLER_OBJECT := $(BUILD_DIR)/runner/execution_controller.o
EXECUTION_CONTROLLER_DEPENDENCY := $(BUILD_DIR)/runner/execution_controller.d
HEADLESS_RUNNER_OUTPUT := $(BUILD_DIR)/runner/headless_runner
HEADLESS_RUNNER_STAGE1_IMAGE := $(abspath $(CURDIR)/../tests/guest/np2test/golden/np2test-fd1232.image)
HEADLESS_RUNNER_SUPERVISOR := $(CURDIR)/tests/run_headless_runner.py
VIDEO_RUNNER_SOURCE := $(CURDIR)/runner/video_main.c
VIDEO_RUNNER_OBJECT := $(BUILD_DIR)/runner/video_main.o
VIDEO_RUNNER_DEPENDENCY := $(BUILD_DIR)/runner/video_main.d
VIDEO_RUNNER_OUTPUT := $(BUILD_DIR)/runner/video_runner
VIDEO_RUNNER_FIXTURE := $(abspath $(CURDIR)/../build/guest/np2video/np2video-fd1232.image)
VIDEO_RUNNER_SUPERVISOR := $(CURDIR)/tests/run_video_runner.py
VIDEO_RUNNER_BMP := $(BUILD_DIR)/runner/np2video-reference.bmp
VIDEO_CONTROL_SOURCE := $(CURDIR)/runner/video_control_v1.c
VIDEO_CONTROL_OBJECT := $(BUILD_DIR)/runner/video_control_v1.o
VIDEO_CONTROL_DEPENDENCY := $(BUILD_DIR)/runner/video_control_v1.d
VIDEO_CONTROL_TEST_SOURCE := $(CURDIR)/tests/video_control_v1_test.c
VIDEO_CONTROL_TEST_OUTPUT := $(BUILD_DIR)/tests/video_control_v1_test
NP2_SHA256_SOURCE := $(CURDIR)/compat/np2_sha256.c
NP2_SHA256_OBJECT := $(BUILD_DIR)/compat/np2_sha256.o
NP2_SHA256_DEPENDENCY := $(BUILD_DIR)/compat/np2_sha256.d
NP2_SHA256_TEST_SOURCE := $(CURDIR)/tests/np2_sha256_test.c
NP2_SHA256_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_sha256_test
VIDEO_BMP_VALIDATOR := $(CURDIR)/tests/validate_video_bmp.py
VIDEO_CLI_TEST := $(CURDIR)/tests/test_video_runner_cli.py
HEADLESS_BMP_OBJECT := $(BUILD_DIR)/backend/headless/scrnmng_bmp.o
HEADLESS_BMP_DEPENDENCY := $(BUILD_DIR)/backend/headless/scrnmng_bmp.d
RESULT_V1_PARSER_SOURCE := $(CURDIR)/runner/result_v1_parser.c
RESULT_V1_PARSER_TEST_SOURCE := $(CURDIR)/tests/result_v1_parser_test.c
RESULT_V1_PARSER_TEST_OUTPUT := $(BUILD_DIR)/tests/result_v1_parser_test
EXECUTION_CONTROLLER_SOURCE := $(CURDIR)/runner/execution_controller.c
EXECUTION_CONTROLLER_TEST_SOURCE := $(CURDIR)/tests/execution_controller_test.c
EXECUTION_CONTROLLER_TEST_OUTPUT := $(BUILD_DIR)/tests/execution_controller_test
HEADLESS_DOSIO_SOURCE := $(CURDIR)/backend/headless/dosio.c
HEADLESS_DOSIO_TEST_SOURCE := $(CURDIR)/tests/headless_dosio_test.c
HEADLESS_DOSIO_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_dosio_test
HEADLESS_FONT_SOURCE := $(CURDIR)/backend/headless/fontmng.c
HEADLESS_FONT_TEST_SOURCE := $(CURDIR)/tests/headless_fontmng_test.c
HEADLESS_FONT_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_fontmng_test
HEADLESS_SCRNMNG_SOURCE := $(CURDIR)/backend/headless/scrnmng.c
HEADLESS_SCRNMNG_TEST_SOURCE := $(CURDIR)/tests/headless_scrnmng_test.c
HEADLESS_SCRNMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_scrnmng_test
HEADLESS_SCRNMNG_SNAPSHOT_TEST_SOURCE := $(CURDIR)/tests/headless_scrnmng_snapshot_test.c
HEADLESS_SCRNMNG_SNAPSHOT_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_scrnmng_snapshot_test
HEADLESS_BMP_SOURCE := $(CURDIR)/backend/headless/scrnmng_bmp.c
HEADLESS_BMP_TEST_SOURCE := $(CURDIR)/tests/headless_scrnmng_bmp_test.c
HEADLESS_BMP_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_scrnmng_bmp_test
NP2_CRC32_SOURCE := $(CURDIR)/compat/np2_crc32.c
NP2_CRC32_TEST_SOURCE := $(CURDIR)/tests/np2_crc32_test.c
NP2_CRC32_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_crc32_test
HEADLESS_COMMNG_SOURCE := $(CURDIR)/backend/headless/commng.c
HEADLESS_COMMNG_TEST_SOURCE := $(CURDIR)/tests/headless_commng_test.c
HEADLESS_COMMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_commng_test
HEADLESS_COMMNG_TEST_VAEG_OUTPUT := $(BUILD_DIR)/tests/headless_commng_test_vaeg
PYTHON ?= python3
CC ?= cc
NM ?= nm
PROBE_INCLUDE_DIRS := \
	$(CURDIR)/compat \
	$(VENDOR_SRC) \
	$(VENDOR_SRC)/i286c

# Exact shared probe definitions. CPUCORE_IA32, SUPPORT_IA32_HAXM,
# USE_SDL, and all i386/i286x selectors are intentionally absent.
PROBE_DEFINES := \
	DISABLE_SOUND \
	SUPPORT_KAI_IMAGES \
	SUPPORT_FAST_MEMORYCHECK \
	SUPPORT_16BPP \
	BYTESEX_LITTLE \
	OSLANG_UTF8 \
	SUPPORT_UTF8
PROBE_STRING_DEFINES := \
	NP2KAI_GIT_TAG=0.86.0.22 \
	NP2KAI_GIT_HASH=e2dc9046aa5c786fcfbfb87e883457e421026e31
PROBE_CFLAGS := \
	-std=c11 \
	-Wall \
	-Wextra \
	-fno-common \
	-Werror=implicit-function-declaration

RUNNER_DEPENDENCIES := \
	$(HEADLESS_RUNNER_DEPENDENCY) \
	$(STAGE1_MACHINE_CONFIG_DEPENDENCY) \
	$(RESULT_V1_PARSER_DEPENDENCY) \
	$(EXECUTION_CONTROLLER_DEPENDENCY)

VIDEO_RUNNER_DEPENDENCIES := \
	$(VIDEO_RUNNER_DEPENDENCY) \
	$(VIDEO_CONTROL_DEPENDENCY) \
	$(NP2_SHA256_DEPENDENCY) \
	$(HEADLESS_BMP_DEPENDENCY)

.PHONY: phase1-probe phase1-config phase1-prepare-sources phase1-probe-mapped phase1-link-probe-mapped headless-runner test-headless-runner-np2test test-headless-runner-attach test-headless-runner-lifecycle video-runner test-video-runner-reference test-video-runner-reference-bmp test-video-runner-cli test-video-control-v1 test-sha256 test-crc32 test-result-v1-parser test-execution-controller test-headless-mouse test-headless-sound test-headless-timing test-headless-timemng test-headless-sysmng test-headless-taskmng test-headless-dosio test-headless-fontmng test-headless-scrnmng test-headless-scrnmng-snapshot test-headless-scrnmng-bmp test-headless-commng

phase1-probe:
	@mkdir -p "$(PROBE_OUTPUT)"
	@$(PYTHON) "$(PROBE_SCRIPT)" \
		--compiler "$(CC)" \
		--source-root "$(VENDOR_SRC)" \
		--source-list "$(SOURCE_LIST)" \
		--host-source-root "$(HOST_BACKEND_SOURCE_ROOT)" \
		--host-source-list "$(HOST_BACKEND_SOURCE_LIST)" \
		--output-dir "$(PROBE_OUTPUT)" \
		$(foreach i,$(PROBE_INCLUDE_DIRS),--include "$(i)") \
		$(foreach d,$(PROBE_DEFINES),--define "$(d)") \
		$(foreach d,$(PROBE_STRING_DEFINES),--string-define "$(d)") \
		$(foreach f,$(PROBE_CFLAGS),--cflag="$(f)")
	@printf '%s\n' 'Headless compiler-contract syntax inventory written to $(PROBE_OUTPUT)' \
		'No link, import, executable, or runtime probe was performed.'

phase1-config:
	@mkdir -p "$(PROBE_OUTPUT)"
	@{ \
		printf '%s\n' 'compiler=$(CC)' 'source_root=$(VENDOR_SRC)' \
			'source_list=$(SOURCE_LIST)' 'compile_only=1' 'link_probe=0'; \
		printf '%s\n' 'defines:'; \
		$(foreach d,$(PROBE_DEFINES),printf '  -D%s\n' '$(d)';) \
		printf '%s\n' 'string_defines:'; \
		$(foreach d,$(PROBE_STRING_DEFINES),printf '  -D%s="..."\n' '$(firstword $(subst =, ,$(d)))';) \
		printf '%s\n' 'forbidden selectors: CPUCORE_IA32 SUPPORT_IA32_HAXM USE_SDL i386 i286x'; \
	} > "$(PROBE_OUTPUT)/probe-config.txt"

phase1-prepare-sources:
	@mkdir -p "$(PATCH_OUTPUT)"
	@$(PYTHON) "$(VENDOR_VERIFY)" \
		--vendor-root "$(VENDOR_ROOT)"
	@$(PYTHON) "$(PREPARER)" \
		--vendor-root "$(VENDOR_ROOT)" \
		--import-manifest "$(IMPORT_MANIFEST)" \
		--patch-set "$(PATCH_SET)" \
		--output-root "$(PATCH_OUTPUT)"
	@printf '%s\n' 'Step 4 source preparation written to $(PATCH_OUTPUT)' \
		'No link, executable, or runtime probe was performed.'

phase1-probe-mapped: phase1-prepare-sources
	@mkdir -p "$(MAPPED_PROBE_OUTPUT)"
	@$(PYTHON) "$(PROBE_SCRIPT)" \
		--compiler "$(CC)" \
		--source-root "$(VENDOR_SRC)" \
		--source-list "$(SOURCE_LIST)" \
		--host-source-root "$(HOST_BACKEND_SOURCE_ROOT)" \
		--host-source-list "$(HOST_BACKEND_SOURCE_LIST)" \
		--output-dir "$(MAPPED_PROBE_OUTPUT)" \
		--source-map "$(PATCH_OUTPUT)/source-map.json" \
		$(foreach i,$(PROBE_INCLUDE_DIRS),--include "$(i)") \
		$(foreach d,$(PROBE_DEFINES),--define "$(d)") \
		$(foreach d,$(PROBE_STRING_DEFINES),--string-define "$(d)") \
		$(foreach f,$(PROBE_CFLAGS),--cflag="$(f)")
	@printf '%s\n' 'Mapped headless compiler-contract syntax inventory written to $(MAPPED_PROBE_OUTPUT)' \
		'No link, import, executable, or runtime probe was performed.'

phase1-link-probe-mapped: phase1-prepare-sources
	@mkdir -p "$(LINK_PROBE_OUTPUT)"
	@$(PYTHON) "$(LINK_PROBE_SCRIPT)" \
		--compiler "$(CC)" \
		--nm "$(NM)" \
		--source-root "$(VENDOR_SRC)" \
		--source-list "$(SOURCE_LIST)" \
		--host-source-root "$(HOST_BACKEND_SOURCE_ROOT)" \
		--host-source-list "$(HOST_BACKEND_SOURCE_LIST)" \
		--source-map "$(PATCH_OUTPUT)/source-map.json" \
		--output-dir "$(LINK_PROBE_OUTPUT)" \
		$(foreach i,$(PROBE_INCLUDE_DIRS),--include "$(i)") \
		$(foreach d,$(PROBE_DEFINES),--define "$(d)") \
		$(foreach d,$(PROBE_STRING_DEFINES),--string-define "$(d)") \
		$(foreach f,$(PROBE_CFLAGS),--cflag="$(f)")
	@printf '%s\n' 'Mapped relocatable link-closure evidence written to $(LINK_PROBE_OUTPUT)' \
		'No executable, runtime, FDD, or NP2TEST probe was performed.'

$(HEADLESS_RUNNER_OBJECT): $(HEADLESS_RUNNER_SOURCE)
	@mkdir -p "$(dir $(HEADLESS_RUNNER_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(HEADLESS_RUNNER_DEPENDENCY)" \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/backend/headless" -I"$(CURDIR)/runner" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		-c "$(HEADLESS_RUNNER_SOURCE)" -o "$(HEADLESS_RUNNER_OBJECT)"

$(STAGE1_MACHINE_CONFIG_OBJECT): $(STAGE1_MACHINE_CONFIG_SOURCE)
	@mkdir -p "$(dir $(STAGE1_MACHINE_CONFIG_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(STAGE1_MACHINE_CONFIG_DEPENDENCY)" \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/runner" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		-c "$(STAGE1_MACHINE_CONFIG_SOURCE)" -o "$(STAGE1_MACHINE_CONFIG_OBJECT)"

$(RESULT_V1_PARSER_OBJECT): $(RESULT_V1_PARSER_SOURCE)
	@mkdir -p "$(dir $(RESULT_V1_PARSER_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(RESULT_V1_PARSER_DEPENDENCY)" \
		-I"$(CURDIR)/runner" -I"$(CURDIR)/compat" \
		-c "$(RESULT_V1_PARSER_SOURCE)" -o "$(RESULT_V1_PARSER_OBJECT)"

$(EXECUTION_CONTROLLER_OBJECT): $(EXECUTION_CONTROLLER_SOURCE)
	@mkdir -p "$(dir $(EXECUTION_CONTROLLER_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(EXECUTION_CONTROLLER_DEPENDENCY)" \
		-I"$(CURDIR)/runner" \
		-c "$(EXECUTION_CONTROLLER_SOURCE)" -o "$(EXECUTION_CONTROLLER_OBJECT)"

$(VIDEO_RUNNER_OBJECT): $(VIDEO_RUNNER_SOURCE)
	@mkdir -p "$(dir $(VIDEO_RUNNER_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(VIDEO_RUNNER_DEPENDENCY)" \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/backend/headless" -I"$(CURDIR)/runner" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		-c "$(VIDEO_RUNNER_SOURCE)" -o "$(VIDEO_RUNNER_OBJECT)"

$(VIDEO_CONTROL_OBJECT): $(VIDEO_CONTROL_SOURCE)
	@mkdir -p "$(dir $(VIDEO_CONTROL_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(VIDEO_CONTROL_DEPENDENCY)" \
		-I"$(CURDIR)/runner" \
		-c "$(VIDEO_CONTROL_SOURCE)" -o "$(VIDEO_CONTROL_OBJECT)"

$(NP2_SHA256_OBJECT): $(NP2_SHA256_SOURCE)
	@mkdir -p "$(dir $(NP2_SHA256_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-MMD -MP -MF "$(NP2_SHA256_DEPENDENCY)" \
		-I"$(CURDIR)/compat" \
		-c "$(NP2_SHA256_SOURCE)" -o "$(NP2_SHA256_OBJECT)"

$(HEADLESS_BMP_OBJECT): $(HEADLESS_BMP_SOURCE)
	@mkdir -p "$(dir $(HEADLESS_BMP_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror -DBYTESEX_LITTLE \
		-MMD -MP -MF "$(HEADLESS_BMP_DEPENDENCY)" \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/backend/headless" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		-c "$(HEADLESS_BMP_SOURCE)" -o "$(HEADLESS_BMP_OBJECT)"

headless-runner: phase1-link-probe-mapped $(HEADLESS_RUNNER_OBJECT) \
		$(STAGE1_MACHINE_CONFIG_OBJECT) \
		$(RESULT_V1_PARSER_OBJECT) $(EXECUTION_CONTROLLER_OBJECT)
	@$(CC) "$(LINK_PROBE_OUTPUT)/combined.o" "$(HEADLESS_RUNNER_OBJECT)" \
		"$(STAGE1_MACHINE_CONFIG_OBJECT)" \
		"$(RESULT_V1_PARSER_OBJECT)" "$(EXECUTION_CONTROLLER_OBJECT)" \
		-lm -o "$(HEADLESS_RUNNER_OUTPUT)"

test-headless-runner-np2test: headless-runner
	@$(PYTHON) "$(HEADLESS_RUNNER_SUPERVISOR)" \
		--timeout 30 --expect-result PASS \
		"$(HEADLESS_RUNNER_OUTPUT)" "$(HEADLESS_RUNNER_STAGE1_IMAGE)"
	@printf '%s\n' 'Headless runner supervised NP2TEST smoke passed.'

test-headless-runner-attach: test-headless-runner-np2test

test-headless-runner-lifecycle: test-headless-runner-np2test

video-runner: phase1-link-probe-mapped $(VIDEO_RUNNER_OBJECT) \
		$(STAGE1_MACHINE_CONFIG_OBJECT) $(VIDEO_CONTROL_OBJECT) \
		$(NP2_SHA256_OBJECT) $(HEADLESS_BMP_OBJECT)
	@mkdir -p "$(dir $(VIDEO_RUNNER_OUTPUT))"
	@$(CC) "$(LINK_PROBE_OUTPUT)/combined.o" "$(VIDEO_RUNNER_OBJECT)" \
		"$(STAGE1_MACHINE_CONFIG_OBJECT)" "$(VIDEO_CONTROL_OBJECT)" \
		"$(NP2_SHA256_OBJECT)" "$(HEADLESS_BMP_OBJECT)" -lm -o "$(VIDEO_RUNNER_OUTPUT)"

test-video-runner-reference: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_FIXTURE)"

test-video-runner-reference-bmp: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		--dump-framebuffer "$(VIDEO_RUNNER_BMP)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_FIXTURE)"
	@$(PYTHON) "$(VIDEO_BMP_VALIDATOR)" "$(VIDEO_RUNNER_BMP)"

test-video-runner-cli: video-runner
	@$(PYTHON) "$(VIDEO_CLI_TEST)" "$(VIDEO_RUNNER_OUTPUT)"

-include $(RUNNER_DEPENDENCIES) $(VIDEO_RUNNER_DEPENDENCIES)

test-video-control-v1:
	@mkdir -p "$(dir $(VIDEO_CONTROL_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/runner" "$(VIDEO_CONTROL_SOURCE)" \
		"$(VIDEO_CONTROL_TEST_SOURCE)" -o "$(VIDEO_CONTROL_TEST_OUTPUT)"
	@"$(VIDEO_CONTROL_TEST_OUTPUT)"
	@printf '%s\n' 'NP2V v1 parser tests passed.'

test-sha256:
	@mkdir -p "$(dir $(NP2_SHA256_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_SHA256_TEST_SOURCE)" -o "$(NP2_SHA256_TEST_OUTPUT)"
	@"$(NP2_SHA256_TEST_OUTPUT)"
	@printf '%s\n' 'Portable SHA-256 utility test passed.'

test-crc32:
	@mkdir -p "$(dir $(NP2_CRC32_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_CRC32_TEST_SOURCE)" -o "$(NP2_CRC32_TEST_OUTPUT)"
	@"$(NP2_CRC32_TEST_OUTPUT)"
	@printf '%s\n' 'Portable CRC-32/ISO-HDLC utility test passed.'

test-result-v1-parser:
	@mkdir -p "$(dir $(RESULT_V1_PARSER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/runner" -I"$(CURDIR)/compat" \
		"$(NP2_CRC32_SOURCE)" "$(RESULT_V1_PARSER_SOURCE)" \
		"$(RESULT_V1_PARSER_TEST_SOURCE)" \
		-o "$(RESULT_V1_PARSER_TEST_OUTPUT)"
	@"$(RESULT_V1_PARSER_TEST_OUTPUT)"

test-execution-controller:
	@mkdir -p "$(dir $(EXECUTION_CONTROLLER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/runner" \
		"$(EXECUTION_CONTROLLER_SOURCE)" "$(EXECUTION_CONTROLLER_TEST_SOURCE)" \
		-o "$(EXECUTION_CONTROLLER_TEST_OUTPUT)"
	@"$(EXECUTION_CONTROLLER_TEST_OUTPUT)"

test-headless-mouse:
	@mkdir -p "$(dir $(HEADLESS_MOUSE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_MOUSE_SOURCE)" "$(HEADLESS_MOUSE_TEST_SOURCE)" \
		-o "$(HEADLESS_MOUSE_TEST_OUTPUT)"
	@"$(HEADLESS_MOUSE_TEST_OUTPUT)"
	@printf '%s\n' 'Headless mouse backend test passed (four deterministic cases).'

test-headless-sound:
	@mkdir -p "$(dir $(HEADLESS_SOUND_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_SOUND_SOURCE)" "$(HEADLESS_SOUND_TEST_SOURCE)" \
		-o "$(HEADLESS_SOUND_TEST_OUTPUT)"
	@"$(HEADLESS_SOUND_TEST_OUTPUT)"
	@printf '%s\n' 'Headless sound backend test passed (all deterministic cases).'

test-headless-timing:
	@mkdir -p "$(dir $(HEADLESS_TIMING_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_TIMING_SOURCE)" "$(HEADLESS_TIMING_TEST_SOURCE)" \
		-o "$(HEADLESS_TIMING_TEST_OUTPUT)"
	@"$(HEADLESS_TIMING_TEST_OUTPUT)"
	@printf '%s\n' 'Headless timing backend test passed (modulo-forward samples).'

test-headless-timemng:
	@mkdir -p "$(dir $(HEADLESS_TIMEMNG_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_TIMEMNG_SOURCE)" "$(HEADLESS_TIMEMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_TIMEMNG_TEST_OUTPUT)"
	@"$(HEADLESS_TIMEMNG_TEST_OUTPUT)"
	@printf '%s\n' 'Headless calendar-time backend test passed (local civil-time ranges).'

test-headless-sysmng:
	@mkdir -p "$(dir $(HEADLESS_SYSMNG_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_SYSMNG_SOURCE)" "$(HEADLESS_SYSMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_SYSMNG_TEST_OUTPUT)"
	@"$(HEADLESS_SYSMNG_TEST_OUTPUT)"
	@printf '%s\n' 'Headless sysmng backend test passed (stateless notifications).'

test-headless-taskmng:
	@mkdir -p "$(dir $(HEADLESS_TASKMNG_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/backend/headless" \
		"$(HEADLESS_TASKMNG_SOURCE)" "$(HEADLESS_TASKMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_TASKMNG_TEST_OUTPUT)"
	@"$(HEADLESS_TASKMNG_TEST_OUTPUT)"
	@printf '%s\n' 'Headless taskmng backend test passed (sticky cooperative exit request).'

test-headless-dosio:
	@mkdir -p "$(dir $(HEADLESS_DOSIO_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_DOSIO_SOURCE)" "$(HEADLESS_DOSIO_TEST_SOURCE)" \
		-o "$(HEADLESS_DOSIO_TEST_OUTPUT)"
	@"$(HEADLESS_DOSIO_TEST_OUTPUT)"
	@printf '%s\n' 'Headless DOSIO backend test passed (non-directory contract).'

test-headless-fontmng:
	@mkdir -p "$(dir $(HEADLESS_FONT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_FONT_SOURCE)" "$(HEADLESS_FONT_TEST_SOURCE)" \
		-o "$(HEADLESS_FONT_TEST_OUTPUT)"
	@"$(HEADLESS_FONT_TEST_OUTPUT)"
	@printf '%s\n' 'Headless fontmng backend test passed (deterministic unavailable-font contract).'

test-headless-scrnmng:
	@mkdir -p "$(dir $(HEADLESS_SCRNMNG_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -DBYTESEX_LITTLE \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(NP2_CRC32_SOURCE)" "$(HEADLESS_SCRNMNG_SOURCE)" \
		"$(CURDIR)/backend/headless/scrnmng_storage.c" \
		"$(HEADLESS_SCRNMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_SCRNMNG_TEST_OUTPUT)"
	@"$(HEADLESS_SCRNMNG_TEST_OUTPUT)"
	@printf '%s\n' 'Headless scrnmng backend test passed (persistent RGB565 surface contract).'

test-headless-scrnmng-snapshot:
	@mkdir -p "$(dir $(HEADLESS_SCRNMNG_SNAPSHOT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -DBYTESEX_LITTLE \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(NP2_CRC32_SOURCE)" "$(HEADLESS_SCRNMNG_SOURCE)" \
		"$(CURDIR)/backend/headless/scrnmng_storage.c" \
		"$(HEADLESS_SCRNMNG_SNAPSHOT_TEST_SOURCE)" \
		-o "$(HEADLESS_SCRNMNG_SNAPSHOT_TEST_OUTPUT)"
	@"$(HEADLESS_SCRNMNG_SNAPSHOT_TEST_OUTPUT)"
	@printf '%s\n' 'Headless scrnmng snapshot test passed (CRC, metadata, sequence, resize).'

test-headless-scrnmng-bmp:
	@mkdir -p "$(dir $(HEADLESS_BMP_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -DBYTESEX_LITTLE \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(NP2_CRC32_SOURCE)" "$(HEADLESS_SCRNMNG_SOURCE)" \
		"$(CURDIR)/backend/headless/scrnmng_storage.c" \
		"$(HEADLESS_BMP_SOURCE)" "$(HEADLESS_BMP_TEST_SOURCE)" \
		-o "$(HEADLESS_BMP_TEST_OUTPUT)"
	@"$(HEADLESS_BMP_TEST_OUTPUT)"
	@printf '%s\n' 'Headless scrnmng BMP test passed (24-bit BGR, orientation, padding).'

test-headless-commng:
	@mkdir -p "$(dir $(HEADLESS_COMMNG_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_COMMNG_SOURCE)" "$(HEADLESS_COMMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_COMMNG_TEST_OUTPUT)"
	@"$(HEADLESS_COMMNG_TEST_OUTPUT)"
	@$(CC) -std=c11 -Wall -Wextra -Werror -DVAEG_FIX \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		"$(HEADLESS_COMMNG_SOURCE)" "$(HEADLESS_COMMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_COMMNG_TEST_VAEG_OUTPUT)"
	@"$(HEADLESS_COMMNG_TEST_VAEG_OUTPUT)"
	@printf '%s\n' 'Headless commng backend tests passed (normal and VAEG_FIX ABI configurations).'
