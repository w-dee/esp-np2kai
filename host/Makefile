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
HEADLESS_RUNNER_OUTPUT := $(BUILD_DIR)/runner/headless_runner
HEADLESS_RUNNER_STAGE1_IMAGE := $(abspath $(CURDIR)/../tests/guest/np2test/golden/np2test-fd1232.image)
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

.PHONY: phase1-probe phase1-config phase1-prepare-sources phase1-probe-mapped phase1-link-probe-mapped headless-runner test-headless-runner-attach test-headless-runner-lifecycle test-result-v1-parser test-execution-controller test-headless-mouse test-headless-sound test-headless-timing test-headless-timemng test-headless-sysmng test-headless-taskmng test-headless-dosio test-headless-fontmng test-headless-scrnmng test-headless-commng

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

headless-runner: phase1-link-probe-mapped
	@mkdir -p "$(dir $(HEADLESS_RUNNER_OBJECT))"
	@$(CC) $(PROBE_CFLAGS) -Werror \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/backend/headless" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		-c "$(HEADLESS_RUNNER_SOURCE)" -o "$(HEADLESS_RUNNER_OBJECT)"
	@$(CC) "$(LINK_PROBE_OUTPUT)/combined.o" "$(HEADLESS_RUNNER_OBJECT)" \
		-lm -o "$(HEADLESS_RUNNER_OUTPUT)"

test-headless-runner-attach: headless-runner
	@"$(HEADLESS_RUNNER_OUTPUT)" "$(HEADLESS_RUNNER_STAGE1_IMAGE)"
	@printf '%s\n' 'Headless runner reset/FDD-attach smoke passed.'

test-headless-runner-lifecycle: test-headless-runner-attach

test-result-v1-parser:
	@mkdir -p "$(dir $(RESULT_V1_PARSER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/runner" \
		"$(RESULT_V1_PARSER_SOURCE)" "$(RESULT_V1_PARSER_TEST_SOURCE)" \
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
		"$(HEADLESS_SCRNMNG_SOURCE)" "$(HEADLESS_SCRNMNG_TEST_SOURCE)" \
		-o "$(HEADLESS_SCRNMNG_TEST_OUTPUT)"
	@"$(HEADLESS_SCRNMNG_TEST_OUTPUT)"
	@printf '%s\n' 'Headless scrnmng backend test passed (NULL-surface contract).'

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
