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

.PHONY: phase1-probe phase1-config phase1-prepare-sources phase1-probe-mapped phase1-link-probe-mapped test-headless-mouse test-headless-sound test-headless-timing test-headless-timemng test-headless-sysmng

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
