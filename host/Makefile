SHELL := /bin/sh

# Explicit compile/link evidence inventories; this is not a native emulator build.
HOST_ROOT := $(abspath $(CURDIR)/..)
VENDOR_ROOT := $(HOST_ROOT)/third_party/np2kai
VENDOR_SRC := $(VENDOR_ROOT)/src
SOURCE_LIST := $(CURDIR)/probe/host_core_sources.txt
HOST_BACKEND_SOURCE_LIST := $(CURDIR)/probe/host_backend_sources.txt
HOST_BACKEND_SOURCE_ROOT := $(HOST_ROOT)
NP2CORE_INCLUDE := $(HOST_ROOT)/firmware/components/np2core/include
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
HOST_SOURCE_OVERLAY := $(PATCH_OUTPUT)/host-overlay
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
VIDEO_RUNNER_GFX_FIXTURE := $(abspath $(CURDIR)/../build/guest/np2video-gfx-vram/np2video-gfx-vram-fd1232.image)
VIDEO_RUNNER_GFX_BMP := $(BUILD_DIR)/runner/np2video-gfx-vram-reference.bmp
VIDEO_RUNNER_GDC_FIXTURE := $(abspath $(CURDIR)/../build/guest/np2video-gdc/np2video-gdc-fd1232.image)
VIDEO_RUNNER_GDC_BMP := $(BUILD_DIR)/runner/np2video-gdc-reference.bmp
VIDEO_RUNNER_LIVE_FIXTURE := $(abspath $(CURDIR)/../build/guest/np2video-live/np2video-live-fd1232.image)
VIDEO_RUNNER_LIVE_BMP := $(BUILD_DIR)/runner/np2video-live-reference.bmp
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
VIDEO_GFX_BMP_VALIDATOR := $(HOST_ROOT)/tools/guest/test_np2video_gfx_vram_bmp.py
VIDEO_GDC_BMP_VALIDATOR := $(HOST_ROOT)/tools/guest/test_np2video_gdc_bmp.py
VIDEO_CLI_TEST := $(CURDIR)/tests/test_video_runner_cli.py
VIDEO_GOLDEN_DESCRIPTOR := $(abspath $(CURDIR)/../tests/guest/np2video/golden.json)
VIDEO_GFX_GOLDEN_DESCRIPTOR := $(abspath $(CURDIR)/../tests/guest/np2video-gfx-vram/golden.json)
VIDEO_GDC_GOLDEN_DESCRIPTOR := $(abspath $(CURDIR)/../tests/guest/np2video-gdc/golden.json)
VIDEO_LIVE_GOLDEN_DESCRIPTOR := $(abspath $(CURDIR)/../tests/guest/np2video-live/golden.json)
VIDEO_GOLDEN_SUPERVISOR := $(CURDIR)/tests/run_video_golden.py
VIDEO_GOLDEN_TEST := $(CURDIR)/tests/test_video_golden.py
VIDEO_GOLDEN_HEADER_TEST := $(HOST_ROOT)/tools/guest/test_np2video_golden_header.py
VIDEO_GOLDEN_LOG_TEST := $(HOST_ROOT)/tools/guest/test_np2video_esp_log.py
HEADLESS_BMP_OBJECT := $(BUILD_DIR)/backend/headless/scrnmng_bmp.o
HEADLESS_BMP_DEPENDENCY := $(BUILD_DIR)/backend/headless/scrnmng_bmp.d
RESULT_V1_PARSER_SOURCE := $(CURDIR)/runner/result_v1_parser.c
RESULT_V1_PARSER_TEST_SOURCE := $(CURDIR)/tests/result_v1_parser_test.c
RESULT_V1_PARSER_TEST_OUTPUT := $(BUILD_DIR)/tests/result_v1_parser_test
NP2KBD_CONTROL_V1_SOURCE := $(CURDIR)/runner/np2kbd_control_v1_parser.c
NP2KBD_CONTROL_V1_HEADER_DIR := $(CURDIR)/runner
NP2KBD_CONTROL_V1_TEST_SOURCE := $(CURDIR)/tests/np2kbd_control_v1_parser_test.c
NP2KBD_CONTROL_V1_TEST_OUTPUT := $(BUILD_DIR)/tests/np2kbd_control_v1_parser_test
NP2KBD_RESULT_V1_SOURCE := $(CURDIR)/runner/np2kbd_result_v1_parser.c
NP2KBD_RESULT_V1_TEST_SOURCE := $(CURDIR)/tests/np2kbd_result_v1_parser_test.c
NP2KBD_RESULT_V1_TEST_OUTPUT := $(BUILD_DIR)/tests/np2kbd_result_v1_parser_test
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
PRESENTATION_SOURCE := $(CURDIR)/presentation/np2_presentation.c
PRESENTATION_TEST_SOURCE := $(CURDIR)/tests/np2_presentation_test.c
PRESENTATION_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_presentation_test
P4_NANO_PATTERN_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_display/p4_nano_display_pattern.cpp
P4_NANO_PATTERN_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_display/include
P4_NANO_PATTERN_TEST_SOURCE := $(CURDIR)/tests/p4_nano_display_pattern_test.cpp
P4_NANO_PATTERN_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_display_pattern_test
P4_NANO_TRANSFORM_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_display/p4_nano_display_transform.cpp
P4_NANO_TRANSFORM_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_display/include
P4_NANO_TRANSFORM_TEST_SOURCE := $(CURDIR)/tests/p4_nano_display_transform_test.cpp
P4_NANO_TRANSFORM_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_display_transform_test
P4_NANO_TRANSFORM_PATTERN_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_display/p4_nano_display_transform_pattern.cpp
P4_NANO_TRANSFORM_PATTERN_TEST_SOURCE := $(CURDIR)/tests/p4_nano_display_transform_diagnostic_test.cpp
P4_NANO_TRANSFORM_PATTERN_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_display_transform_diagnostic_test
P4_NANO_PSRAM_BANDWIDTH_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_live_display/p4_nano_psram_bandwidth.cpp
P4_NANO_PSRAM_BANDWIDTH_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display/include
P4_NANO_PSRAM_BANDWIDTH_TEST_SOURCE := $(CURDIR)/tests/p4_nano_psram_bandwidth_test.cpp
P4_NANO_PSRAM_BANDWIDTH_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_psram_bandwidth_test
P4_NANO_PSRAM_READ_CONTROL_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display/include
P4_NANO_PSRAM_READ_CONTROL_TEST_SOURCE := $(CURDIR)/tests/p4_nano_psram_read_control_test.cpp
P4_NANO_PSRAM_READ_CONTROL_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_psram_read_control_test
P4_NANO_PPA_ROTATION_REFERENCE_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display/include
P4_NANO_PPA_ROTATION_REFERENCE_TEST_SOURCE := $(CURDIR)/tests/p4_nano_ppa_rotation_reference_test.cpp
P4_NANO_PPA_ROTATION_REFERENCE_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_ppa_rotation_reference_test
P4_NANO_LIVE_CONTRACT_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display/include
P4_NANO_LIVE_SESSION_CONTRACT_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display_session/include
P4_NANO_LIVE_CONTRACT_TEST_SOURCE := $(CURDIR)/tests/p4_nano_live_display_contract_test.cpp
P4_NANO_LIVE_CONTRACT_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_live_display_contract_test
P4_NANO_OVERLAP_TRACE_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_live_display/include
P4_NANO_PCCORE_TRACE_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2video_runner/include
NP2_PCCORE_PROFILER_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2core/include
P4_NANO_OVERLAP_TRACE_TEST_SOURCE := $(CURDIR)/tests/p4_nano_overlap_trace_test.cpp
P4_NANO_OVERLAP_TRACE_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_overlap_trace_test
P4_NANO_PCCORE_PROFILER_SOURCE := $(HOST_ROOT)/firmware/components/np2core/np2_pccore_profiler.c
P4_NANO_LIVE_CONTRACT_PRESENTATION_OBJECT := $(BUILD_DIR)/tests/np2_presentation_for_live_contract.o
NP2RUNTIME_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2runtime/include
NP2RUNTIME_LIFECYCLE_SOURCE := $(HOST_ROOT)/firmware/components/np2runtime/np2runtime_lifecycle.cpp
NP2RUNTIME_LIFECYCLE_TEST_SOURCE := $(CURDIR)/tests/np2runtime_lifecycle_test.cpp
NP2RUNTIME_LIFECYCLE_TEST_OUTPUT := $(BUILD_DIR)/tests/np2runtime_lifecycle_test
NP2RUNTIME_MACHINE_CONFIG_TEST_SOURCE := $(CURDIR)/tests/np2runtime_machine_config_test.cpp
NP2RUNTIME_MACHINE_CONFIG_TEST_OUTPUT := $(BUILD_DIR)/tests/np2runtime_machine_config_test
NP2RUNTIME_RUNTIME_TEST_SOURCE := $(CURDIR)/tests/np2runtime_runtime_test.cpp
NP2RUNTIME_RUNTIME_TEST_OUTPUT := $(BUILD_DIR)/tests/np2runtime_runtime_test
NP2_KEYBOARD_INPUT_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2_keyboard_input/include
NP2_KEYBOARD_INPUT_SOURCE := $(HOST_ROOT)/firmware/components/np2_keyboard_input/keyboard_input.cpp
NP2_KEYBOARD_INPUT_OWNERSHIP_SOURCE := $(HOST_ROOT)/firmware/components/np2_keyboard_input/ownership.cpp
NP2_KEYBOARD_INPUT_TEST_SOURCE := $(CURDIR)/tests/np2_keyboard_input_test.cpp
NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_SOURCE := $(CURDIR)/tests/np2_keyboard_ownership_test.cpp
NP2_KEYBOARD_INPUT_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_keyboard_input_test
NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_keyboard_ownership_test
HID_BOOT_KEYBOARD_HEADER_DIR := $(HOST_ROOT)/firmware/components/hid_boot_keyboard/include
HID_BOOT_KEYBOARD_SOURCE := $(HOST_ROOT)/firmware/components/hid_boot_keyboard/hid_boot_keyboard.c
USB_HID_KEYBOARD_ADAPTER_HEADER_DIR := $(HOST_ROOT)/firmware/components/usb_hid_keyboard_adapter/include
USB_HID_KEYBOARD_ADAPTER_SOURCE := $(HOST_ROOT)/firmware/components/usb_hid_keyboard_adapter/adapter.cpp
USB_HID_KEYBOARD_ADAPTER_TEST_SOURCE := $(CURDIR)/tests/usb_hid_keyboard_adapter_test.cpp
USB_HID_KEYBOARD_ADAPTER_PARSER_OBJECT := $(BUILD_DIR)/tests/hid_boot_keyboard.o
USB_HID_KEYBOARD_ADAPTER_TEST_OUTPUT := $(BUILD_DIR)/tests/usb_hid_keyboard_adapter_test
P4_NANO_PC98_RUNTIME_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_pc98_runtime/include
P4_NANO_PC98_RUNTIME_CONTRACT_TEST_SOURCE := $(CURDIR)/tests/p4_nano_pc98_runtime_contract_test.cpp
P4_NANO_PC98_RUNTIME_CONTRACT_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_pc98_runtime_contract_test
P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_SOURCE := $(CURDIR)/tests/p4_nano_usb_keyboard_teardown_contract_test.cpp
P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_usb_keyboard_teardown_contract_test
NP2_KEYBOARD_VALIDATION_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2_keyboard_validation/include
NP2_KEYBOARD_VALIDATION_SOURCE := $(HOST_ROOT)/firmware/components/np2_keyboard_validation/validation_controller.cpp
NP2_KEYBOARD_VALIDATION_TEST_SOURCE := $(CURDIR)/tests/np2_keyboard_validation_test.cpp
NP2_KEYBOARD_VALIDATION_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_keyboard_validation_test
SCRNMNG_PRESENTATION_TEST_SOURCE := $(CURDIR)/tests/headless_scrnmng_presentation_test.c
SCRNMNG_PRESENTATION_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_scrnmng_presentation_test
HEADLESS_BMP_SOURCE := $(CURDIR)/backend/headless/scrnmng_bmp.c
HEADLESS_BMP_TEST_SOURCE := $(CURDIR)/tests/headless_scrnmng_bmp_test.c
HEADLESS_BMP_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_scrnmng_bmp_test
NP2_CRC32_SOURCE := $(CURDIR)/compat/np2_crc32.c
NP2_CRC32_TEST_SOURCE := $(CURDIR)/tests/np2_crc32_test.c
NP2VIDEO_MOTION_ORACLE_SOURCE := $(CURDIR)/compat/np2video_motion_oracle.c
NP2VIDEO_MOTION_ORACLE_HEADER_DIR := $(CURDIR)/compat
NP2VIDEO_MOTION_ORACLE_TEST_SOURCE := $(CURDIR)/tests/np2video_motion_oracle_test.c
NP2VIDEO_MOTION_ORACLE_TEST_OUTPUT := $(BUILD_DIR)/tests/np2video_motion_oracle_test
NP2_CRC32_TEST_OUTPUT := $(BUILD_DIR)/tests/np2_crc32_test
OPNGEN_FIXTURE_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_fixture.c
OPNGEN_FIXTURE_MAIN_SOURCE := $(CURDIR)/tests/opngen_fixture_main.c
OPNGEN_FIXTURE_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_fixture
OPNGEN_FIXTURE_LOG_OUTPUT := $(BUILD_DIR)/tests/opngen_fixture.log
OPNGEN_FIXTURE_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_opngen_fixture_log.py
OPNGEN_FIXTURE_GOLDEN := $(HOST_ROOT)/tools/emu/opngen_fixture_golden.json
OPNGEN_SYNTH_EVENT_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_synth_event.c
OPNGEN_SYNTH_EVENT_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2opngen_fixture/include
OPNGEN_SYNTH_EVENT_TEST_SOURCE := $(CURDIR)/tests/opngen_synth_event_test.c
OPNGEN_SYNTH_EVENT_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_synth_event_test
OPNGEN_S98_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_s98.c
OPNGEN_S98_TEST_SOURCE := $(CURDIR)/tests/opngen_s98_test.c
OPNGEN_S98_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_s98_test
OPNGEN_S98_FIXTURE_GENERATOR := $(CURDIR)/tests/generate_opngen_s98_fixtures.py
OPNGEN_S98_FIXTURE_DIR := $(BUILD_DIR)/tests/opngen_s98_fixtures
OPNGEN_S98_S3_TEST_SOURCE := $(CURDIR)/tests/opngen_s98_s3_test.c
OPNGEN_S98_S3_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_s98_s3_test
OPNGEN_S98_S3_RUNNER := $(CURDIR)/tests/run_opngen_s98_s3.py
OPNGEN_PCM_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c
OPNGEN_SPSC_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_spsc.c
OPNGEN_E1B_STREAM_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_e1b_stream.c
OPNGEN_E1B_TEST_SOURCE := $(CURDIR)/tests/opngen_e1b_spsc_test.c
OPNGEN_E1B_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1b_spsc_test
OPNGEN_E1B_TSAN_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1b_spsc_test_tsan
OPNGEN_E1B_FORMAL_RUNNER := $(CURDIR)/tests/run_opngen_e1b_formal.py
OPNGEN_SYNTHETIC_WORKLOAD_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_synthetic_workload.c
OPNGEN_E1C_TEST_SOURCE := $(CURDIR)/tests/opngen_e1c_sustained_test.c
OPNGEN_E1C_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1c_sustained_test
OPNGEN_E1C_RUNNER := $(CURDIR)/tests/run_opngen_e1c.py
OPNGEN_E1C_FORMAL_RUNNER := $(CURDIR)/tests/run_opngen_e1c_formal.py
OPNGEN_E1C_DESCRIPTOR := $(HOST_ROOT)/tools/emu/opngen_sustained_workload_golden.json
OPNGEN_E1C_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_opngen_sustained_log.py
OPNGEN_E1C_VALIDATOR_TEST := $(HOST_ROOT)/tools/emu/test_opngen_sustained_validator.py
HEADLESS_COMMNG_SOURCE := $(CURDIR)/backend/headless/commng.c
HEADLESS_COMMNG_TEST_SOURCE := $(CURDIR)/tests/headless_commng_test.c
HEADLESS_COMMNG_TEST_OUTPUT := $(BUILD_DIR)/tests/headless_commng_test
HEADLESS_COMMNG_TEST_VAEG_OUTPUT := $(BUILD_DIR)/tests/headless_commng_test_vaeg
PYTHON ?= python3
CC ?= cc
CXX ?= c++
NM ?= nm
FASTPATH ?= 0

ifneq ($(filter 0 1,$(FASTPATH)),$(FASTPATH))
$(error FASTPATH must be exactly 0 or 1)
endif

PROBE_INCLUDE_DIRS := \
	$(CURDIR)/compat \
	$(NP2CORE_INCLUDE) \
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
	SUPPORT_UTF8 \
	NP2_I286C_INLINE_MEM_FASTPATH=$(FASTPATH)
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

.PHONY: phase1-probe phase1-config phase1-prepare-sources phase1-probe-mapped phase1-link-probe-mapped headless-runner test-headless-runner-np2test test-headless-runner-attach test-headless-runner-lifecycle video-runner test-video-runner-reference test-video-runner-reference-bmp test-video-runner-gfx-vram-reference test-video-runner-gfx-vram-reference-bmp test-video-gdc-reference test-video-gdc-reference-bmp test-video-runner-live-reference test-video-runner-live-reference-bmp test-video-runner-cli test-video-runner-golden test-video-gfx-vram-golden test-video-gdc-golden test-video-golden-checker test-video-golden-header test-video-golden-log test-video-control-v1 test-sha256 test-crc32 test-opngen-fixture test-opngen-synth-event test-opngen-s98-s3 build-opngen-e1b test-opngen-e1b test-opngen-e1b-formal build-opngen-e1b-tsan test-opngen-e1b-tsan build-opngen-e1c test-opngen-e1c test-opngen-e1c-validator test-opngen-e1c-formal test-result-v1-parser test-np2kbd-control-v1 test-np2kbd-result-v1 test-np2kbd test-execution-controller test-headless-mouse test-headless-sound test-headless-timing test-headless-timemng test-headless-sysmng test-headless-taskmng test-headless-dosio test-headless-fontmng test-headless-scrnmng test-headless-scrnmng-snapshot test-presentation-publisher test-headless-scrnmng-presentation test-presentation-contract test-p4-nano-display-pattern test-p4-nano-display-transform test-p4-nano-display-transform-diagnostic test-p4-nano-psram-bandwidth test-p4-nano-psram-read-control test-p4-nano-live-display-contract test-p4-nano-overlap-trace test-p4-nano-pc98-runtime-contract test-p4-nano-usb-keyboard-teardown-contract test-np2video-motion-oracle test-headless-scrnmng-bmp test-headless-commng test-np2runtime test-np2runtime-lifecycle test-np2runtime-machine-config test-np2runtime-runtime test-np2-keyboard-input test-np2-keyboard-ownership test-keyboard-input test-np2-keyboard-validation test-usb-hid-keyboard-adapter

.PHONY: test-p4-nano-ppa-rotation-reference

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
			'NP2_I286C_INLINE_MEM_FASTPATH=$(FASTPATH)' \
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
		--source-overlay "$(HOST_SOURCE_OVERLAY)" \
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
		--source-overlay "$(HOST_SOURCE_OVERLAY)" \
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

test-video-runner-gfx-vram-reference: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gfx-vram" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		--fixture-id np2video-7a3c-gfx-vram --scene-id 2 \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GFX_FIXTURE)"

test-video-runner-gfx-vram-reference-bmp: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gfx-vram" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		--fixture-id np2video-7a3c-gfx-vram --scene-id 2 \
		--dump-framebuffer "$(VIDEO_RUNNER_GFX_BMP)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GFX_FIXTURE)"
	@$(PYTHON) "$(VIDEO_BMP_VALIDATOR)" "$(VIDEO_RUNNER_GFX_BMP)"
	@$(PYTHON) "$(VIDEO_GFX_BMP_VALIDATOR)" "$(VIDEO_RUNNER_GFX_BMP)"

test-video-gdc-reference: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gdc" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		--fixture-id np2video-7a3d-gdc --scene-id 3 \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GDC_FIXTURE)"
	@printf '%s\n' 'NP2VIDEO_GDC_RESULT=REFERENCE_READY'

test-video-gdc-reference-bmp: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gdc" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 30 \
		--fixture-id np2video-7a3d-gdc --scene-id 3 \
		--dump-framebuffer "$(VIDEO_RUNNER_GDC_BMP)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GDC_FIXTURE)"
	@$(PYTHON) "$(VIDEO_BMP_VALIDATOR)" "$(VIDEO_RUNNER_GDC_BMP)"
	@$(PYTHON) "$(VIDEO_GDC_BMP_VALIDATOR)" "$(VIDEO_RUNNER_GDC_BMP)"
	@printf '%s\n' 'NP2VIDEO_GDC_RESULT=REFERENCE_READY'

test-video-runner-live-reference: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-live" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 45 \
		--fixture-id np2video-7b2d-live-vram --scene-id 2 --multi-frame 20 \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_LIVE_FIXTURE)"
	@printf '%s\n' 'NP2VIDEO_LIVE_MULTIFRAME_RESULT=PASS'

test-video-runner-live-reference-bmp: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-live" image
	@$(PYTHON) "$(VIDEO_RUNNER_SUPERVISOR)" --timeout 45 \
		--fixture-id np2video-7b2d-live-vram --scene-id 2 \
		--dump-framebuffer "$(VIDEO_RUNNER_LIVE_BMP)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_LIVE_FIXTURE)"
	@$(PYTHON) "$(VIDEO_BMP_VALIDATOR)" "$(VIDEO_RUNNER_LIVE_BMP)"

test-video-runner-cli: video-runner
	@$(PYTHON) "$(VIDEO_CLI_TEST)" "$(VIDEO_RUNNER_OUTPUT)"

test-video-runner-golden: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video" image
	@$(PYTHON) "$(VIDEO_GOLDEN_SUPERVISOR)" --timeout 30 \
		--descriptor "$(VIDEO_GOLDEN_DESCRIPTOR)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_FIXTURE)"

test-video-gfx-vram-golden: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gfx-vram" image
	@$(PYTHON) "$(VIDEO_GOLDEN_SUPERVISOR)" --timeout 30 \
		--descriptor "$(VIDEO_GFX_GOLDEN_DESCRIPTOR)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GFX_FIXTURE)"
	@printf '%s\n' 'NP2VIDEO_GFX_VRAM_GOLDEN_RESULT=PASS'

test-video-gdc-golden: video-runner
	@$(MAKE) --no-print-directory -C "$(CURDIR)/../tests/guest/np2video-gdc" image
	@$(PYTHON) "$(VIDEO_GOLDEN_SUPERVISOR)" --timeout 30 \
		--descriptor "$(VIDEO_GDC_GOLDEN_DESCRIPTOR)" \
		"$(VIDEO_RUNNER_OUTPUT)" "$(VIDEO_RUNNER_GDC_FIXTURE)"
	@printf '%s\n' 'NP2VIDEO_GDC_GOLDEN_RESULT=PASS'

test-video-golden-checker:
	@$(PYTHON) "$(VIDEO_GOLDEN_TEST)"

test-video-golden-header:
	@$(PYTHON) "$(VIDEO_GOLDEN_HEADER_TEST)"

test-video-golden-log:
	@$(PYTHON) "$(VIDEO_GOLDEN_LOG_TEST)"

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

test-opngen-fixture:
	@mkdir -p "$(dir $(OPNGEN_FIXTURE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(OPNGEN_FIXTURE_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_FIXTURE_MAIN_SOURCE)" \
		-lm -o "$(OPNGEN_FIXTURE_TEST_OUTPUT)"
	@"$(OPNGEN_FIXTURE_TEST_OUTPUT)" > "$(OPNGEN_FIXTURE_LOG_OUTPUT)"
	@cat "$(OPNGEN_FIXTURE_LOG_OUTPUT)"
	@$(PYTHON) "$(OPNGEN_FIXTURE_VALIDATOR)" \
		--log "$(OPNGEN_FIXTURE_LOG_OUTPUT)" \
		--golden "$(OPNGEN_FIXTURE_GOLDEN)"
	@printf '%s\n' 'OPNGEN fixture native run and golden validation completed.'

test-opngen-synth-event:
	@mkdir -p "$(dir $(OPNGEN_SYNTH_EVENT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_SYNTH_EVENT_SOURCE)" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_SYNTH_EVENT_TEST_SOURCE)" \
		-o "$(OPNGEN_SYNTH_EVENT_TEST_OUTPUT)"
	@"$(OPNGEN_SYNTH_EVENT_TEST_OUTPUT)"
	@printf '%s\n' 'OPNGEN synth-event validation/interpreter tests passed.'

test-opngen-s98:
	@mkdir -p "$(dir $(OPNGEN_S98_TEST_OUTPUT))" "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(PYTHON) "$(OPNGEN_S98_FIXTURE_GENERATOR)" \
		--output-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_S98_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(OPNGEN_S98_TEST_SOURCE)" -o "$(OPNGEN_S98_TEST_OUTPUT)"
	@"$(OPNGEN_S98_TEST_OUTPUT)" --fixture-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@printf '%s\n' 'OPNGEN S98 parser validation passed.'

test-opngen-s98-s3:
	@mkdir -p "$(dir $(OPNGEN_S98_S3_TEST_OUTPUT))" "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(PYTHON) "$(OPNGEN_S98_FIXTURE_GENERATOR)" \
		--output-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_S98_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_S98_S3_TEST_SOURCE)" \
		-lm -pthread -o "$(OPNGEN_S98_S3_TEST_OUTPUT)"
	@$(PYTHON) "$(OPNGEN_S98_S3_RUNNER)" \
		--binary "$(OPNGEN_S98_S3_TEST_OUTPUT)" \
		--fixture-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@printf '%s\n' 'OPNGEN S98 S3 worker integration and repeatability passed.'

build-opngen-e1b:
	@mkdir -p "$(dir $(OPNGEN_E1B_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_FIXTURE_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_E1B_TEST_SOURCE)" \
		-lm -pthread -o "$(OPNGEN_E1B_TEST_OUTPUT)"

test-opngen-e1b: build-opngen-e1b
	@"$(OPNGEN_E1B_TEST_OUTPUT)"
	@printf '%s\n' 'OPNGEN E1B SPSC worker test passed.'

test-opngen-e1b-formal: build-opngen-e1b
	@$(PYTHON) "$(OPNGEN_E1B_FORMAL_RUNNER)" \
		--binary "$(OPNGEN_E1B_TEST_OUTPUT)" --repetitions 10 --timeout-seconds 10

build-opngen-e1b-tsan:
	@mkdir -p "$(dir $(OPNGEN_E1B_TSAN_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -O1 -g \
		-fsanitize=thread -fno-omit-frame-pointer -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_FIXTURE_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_E1B_TEST_SOURCE)" \
		-lm -pthread -fsanitize=thread -o "$(OPNGEN_E1B_TSAN_TEST_OUTPUT)"

test-opngen-e1b-tsan: build-opngen-e1b-tsan
	@$(PYTHON) "$(OPNGEN_E1B_FORMAL_RUNNER)" \
		--binary "$(OPNGEN_E1B_TSAN_TEST_OUTPUT)" --repetitions 1 --timeout-seconds 10 \
		--tsan-status

build-opngen-e1c:
	@mkdir -p "$(dir $(OPNGEN_E1C_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_FIXTURE_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(OPNGEN_SYNTHETIC_WORKLOAD_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(OPNGEN_E1C_TEST_SOURCE)" -lm -pthread -o "$(OPNGEN_E1C_TEST_OUTPUT)"

test-opngen-e1c: build-opngen-e1c
	@$(PYTHON) "$(OPNGEN_E1C_RUNNER)" --binary "$(OPNGEN_E1C_TEST_OUTPUT)"

test-opngen-e1c-validator:
	@$(PYTHON) "$(OPNGEN_E1C_VALIDATOR_TEST)"

test-opngen-e1c-formal: build-opngen-e1c test-opngen-e1c-validator
	@$(PYTHON) "$(OPNGEN_E1C_FORMAL_RUNNER)" \
		--binary "$(OPNGEN_E1C_TEST_OUTPUT)" \
		--descriptor "$(OPNGEN_E1C_DESCRIPTOR)" --timeout-seconds 90

test-result-v1-parser:
	@mkdir -p "$(dir $(RESULT_V1_PARSER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/runner" -I"$(CURDIR)/compat" \
		"$(NP2_CRC32_SOURCE)" "$(RESULT_V1_PARSER_SOURCE)" \
		"$(RESULT_V1_PARSER_TEST_SOURCE)" \
		-o "$(RESULT_V1_PARSER_TEST_OUTPUT)"
	@"$(RESULT_V1_PARSER_TEST_OUTPUT)"

test-np2kbd-control-v1:
	@mkdir -p "$(dir $(NP2KBD_CONTROL_V1_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(NP2KBD_CONTROL_V1_HEADER_DIR)" -I"$(CURDIR)/compat" \
		"$(NP2_CRC32_SOURCE)" "$(NP2KBD_CONTROL_V1_SOURCE)" \
		"$(NP2KBD_CONTROL_V1_TEST_SOURCE)" -o "$(NP2KBD_CONTROL_V1_TEST_OUTPUT)"
	@"$(NP2KBD_CONTROL_V1_TEST_OUTPUT)"
	@printf '%s\n' 'NP2 keyboard control-v1 parser/tracker tests passed.'

test-np2kbd-result-v1:
	@mkdir -p "$(dir $(NP2KBD_RESULT_V1_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(NP2KBD_CONTROL_V1_HEADER_DIR)" -I"$(CURDIR)/compat" \
		"$(NP2_CRC32_SOURCE)" "$(NP2KBD_RESULT_V1_SOURCE)" \
		"$(NP2KBD_RESULT_V1_TEST_SOURCE)" -o "$(NP2KBD_RESULT_V1_TEST_OUTPUT)"
	@"$(NP2KBD_RESULT_V1_TEST_OUTPUT)"
	@printf '%s\n' 'NP2 keyboard result-v1 parser tests passed.'

test-np2kbd: test-np2kbd-control-v1 test-np2kbd-result-v1

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
	@printf '%s\n' 'Headless taskmng backend test passed (exit state and no-op cooperation).'

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

test-presentation-publisher:
	@mkdir -p "$(dir $(PRESENTATION_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/presentation" "$(PRESENTATION_SOURCE)" \
		"$(PRESENTATION_TEST_SOURCE)" -o "$(PRESENTATION_TEST_OUTPUT)"
	@"$(PRESENTATION_TEST_OUTPUT)"

test-p4-nano-display-pattern:
	@mkdir -p "$(dir $(P4_NANO_PATTERN_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(P4_NANO_PATTERN_HEADER_DIR)" "$(P4_NANO_PATTERN_SOURCE)" \
		"$(P4_NANO_PATTERN_TEST_SOURCE)" -o "$(P4_NANO_PATTERN_TEST_OUTPUT)"
	@"$(P4_NANO_PATTERN_TEST_OUTPUT)"

test-p4-nano-display-transform:
	@mkdir -p "$(dir $(P4_NANO_TRANSFORM_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_TRANSFORM_HEADER_DIR)" "$(P4_NANO_TRANSFORM_SOURCE)" \
		"$(P4_NANO_TRANSFORM_TEST_SOURCE)" -o "$(P4_NANO_TRANSFORM_TEST_OUTPUT)"
	@"$(P4_NANO_TRANSFORM_TEST_OUTPUT)"

test-p4-nano-display-transform-diagnostic:
	@mkdir -p "$(dir $(P4_NANO_TRANSFORM_PATTERN_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_TRANSFORM_HEADER_DIR)" \
		"$(P4_NANO_PATTERN_SOURCE)" "$(P4_NANO_TRANSFORM_SOURCE)" \
		"$(P4_NANO_TRANSFORM_PATTERN_SOURCE)" \
		"$(P4_NANO_TRANSFORM_PATTERN_TEST_SOURCE)" \
		-o "$(P4_NANO_TRANSFORM_PATTERN_TEST_OUTPUT)"
	@"$(P4_NANO_TRANSFORM_PATTERN_TEST_OUTPUT)"

test-p4-nano-psram-bandwidth:
	@mkdir -p "$(dir $(P4_NANO_PSRAM_BANDWIDTH_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-fno-builtin-memcpy -I"$(P4_NANO_PSRAM_BANDWIDTH_HEADER_DIR)" \
		"$(P4_NANO_PSRAM_BANDWIDTH_SOURCE)" \
		"$(P4_NANO_PSRAM_BANDWIDTH_TEST_SOURCE)" \
		-o "$(P4_NANO_PSRAM_BANDWIDTH_TEST_OUTPUT)"
	@"$(P4_NANO_PSRAM_BANDWIDTH_TEST_OUTPUT)"

test-p4-nano-psram-read-control:
	@mkdir -p "$(dir $(P4_NANO_PSRAM_READ_CONTROL_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_PSRAM_READ_CONTROL_HEADER_DIR)" \
		"$(P4_NANO_PSRAM_READ_CONTROL_TEST_SOURCE)" \
		-o "$(P4_NANO_PSRAM_READ_CONTROL_TEST_OUTPUT)"
	@"$(P4_NANO_PSRAM_READ_CONTROL_TEST_OUTPUT)"

test-p4-nano-ppa-rotation-reference: test-video-runner-live-reference-bmp
	@mkdir -p "$(dir $(P4_NANO_PPA_ROTATION_REFERENCE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_PPA_ROTATION_REFERENCE_HEADER_DIR)" \
		"$(P4_NANO_PPA_ROTATION_REFERENCE_TEST_SOURCE)" \
		-o "$(P4_NANO_PPA_ROTATION_REFERENCE_TEST_OUTPUT)"
	@"$(P4_NANO_PPA_ROTATION_REFERENCE_TEST_OUTPUT)"
	@$(PYTHON) "$(HOST_ROOT)/tools/test_display_p9_ppa_rotation.py" \
		--fixture-bmp "$(VIDEO_RUNNER_LIVE_BMP)"

test-p4-nano-live-display-contract:
	@mkdir -p "$(dir $(P4_NANO_LIVE_CONTRACT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(CURDIR)/presentation" "$(PRESENTATION_SOURCE)" \
		-c -o "$(P4_NANO_LIVE_CONTRACT_PRESENTATION_OBJECT)"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(CURDIR)/presentation" -I"$(P4_NANO_LIVE_CONTRACT_HEADER_DIR)" \
		-I"$(P4_NANO_LIVE_SESSION_CONTRACT_HEADER_DIR)" \
		"$(P4_NANO_LIVE_CONTRACT_TEST_SOURCE)" \
		"$(P4_NANO_LIVE_CONTRACT_PRESENTATION_OBJECT)" \
		-o "$(P4_NANO_LIVE_CONTRACT_TEST_OUTPUT)"
	@"$(P4_NANO_LIVE_CONTRACT_TEST_OUTPUT)"

test-p4-nano-overlap-trace:
	@mkdir -p "$(dir $(P4_NANO_OVERLAP_TRACE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(P4_NANO_OVERLAP_TRACE_HEADER_DIR)" \
		-I"$(P4_NANO_PCCORE_TRACE_HEADER_DIR)" \
		-I"$(NP2_PCCORE_PROFILER_HEADER_DIR)" \
		"$(P4_NANO_PCCORE_PROFILER_SOURCE)" \
		"$(P4_NANO_OVERLAP_TRACE_TEST_SOURCE)" \
		-o "$(P4_NANO_OVERLAP_TRACE_TEST_OUTPUT)"
	@"$(P4_NANO_OVERLAP_TRACE_TEST_OUTPUT)"

test-np2runtime-lifecycle:
	@mkdir -p "$(dir $(NP2RUNTIME_LIFECYCLE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2RUNTIME_HEADER_DIR)" \
		"$(NP2RUNTIME_LIFECYCLE_SOURCE)" \
		"$(NP2RUNTIME_LIFECYCLE_TEST_SOURCE)" \
		-pthread \
		-o "$(NP2RUNTIME_LIFECYCLE_TEST_OUTPUT)"
	@"$(NP2RUNTIME_LIFECYCLE_TEST_OUTPUT)"

test-np2runtime-machine-config:
	@mkdir -p "$(dir $(NP2RUNTIME_MACHINE_CONFIG_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2RUNTIME_HEADER_DIR)" \
		"$(NP2RUNTIME_MACHINE_CONFIG_TEST_SOURCE)" \
		-o "$(NP2RUNTIME_MACHINE_CONFIG_TEST_OUTPUT)"
	@"$(NP2RUNTIME_MACHINE_CONFIG_TEST_OUTPUT)"

test-np2runtime: test-np2runtime-lifecycle test-np2runtime-machine-config test-np2runtime-runtime test-p4-nano-pc98-runtime-contract
	@printf '%s\n' 'NP2 runtime lifecycle and machine configuration tests passed.'

test-p4-nano-pc98-runtime-contract: test-p4-nano-usb-keyboard-teardown-contract
	@mkdir -p "$(dir $(P4_NANO_PC98_RUNTIME_CONTRACT_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_PC98_RUNTIME_HEADER_DIR)" \
		"$(P4_NANO_PC98_RUNTIME_CONTRACT_TEST_SOURCE)" \
		-o "$(P4_NANO_PC98_RUNTIME_CONTRACT_TEST_OUTPUT)"
	@"$(P4_NANO_PC98_RUNTIME_CONTRACT_TEST_OUTPUT)"

test-p4-nano-usb-keyboard-teardown-contract:
	@mkdir -p "$(dir $(P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		"$(P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_SOURCE)" \
		-o "$(P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_OUTPUT)"
	@cd "$(CURDIR)" && "$(P4_NANO_USB_KEYBOARD_TEARDOWN_CONTRACT_TEST_OUTPUT)"

test-np2-keyboard-validation:
	@mkdir -p "$(dir $(NP2_KEYBOARD_VALIDATION_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2_KEYBOARD_VALIDATION_HEADER_DIR)" \
		"$(NP2_KEYBOARD_VALIDATION_SOURCE)" \
		"$(NP2_KEYBOARD_VALIDATION_TEST_SOURCE)" \
		-o "$(NP2_KEYBOARD_VALIDATION_TEST_OUTPUT)"
	@"$(NP2_KEYBOARD_VALIDATION_TEST_OUTPUT)"
	@printf '%s\n' 'NP2 keyboard validation controller tests passed.'

test-np2runtime-runtime: phase1-link-probe-mapped
	@mkdir -p "$(dir $(NP2RUNTIME_RUNTIME_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2RUNTIME_HEADER_DIR)" -I"$(CURDIR)/compat" \
		-I"$(CURDIR)/presentation" -I"$(VENDOR_SRC)" \
		-I"$(VENDOR_SRC)/i286c" -I"$(HOST_ROOT)/firmware/components/np2host/include" \
		$(foreach d,$(PROBE_DEFINES),-D$(d)) \
		"$(HOST_ROOT)/firmware/components/np2runtime/np2runtime.cpp" \
		"$(HOST_ROOT)/firmware/components/np2runtime/np2runtime_lifecycle.cpp" \
		"$(HOST_ROOT)/firmware/components/np2runtime/np2runtime_machine_config.cpp" \
		"$(NP2RUNTIME_RUNTIME_TEST_SOURCE)" \
		"$(LINK_PROBE_OUTPUT)/combined.o" -pthread \
		-o "$(NP2RUNTIME_RUNTIME_TEST_OUTPUT)"
	@"$(NP2RUNTIME_RUNTIME_TEST_OUTPUT)"

test-np2-keyboard-input: test-np2-keyboard-ownership
	@mkdir -p "$(dir $(NP2_KEYBOARD_INPUT_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2_KEYBOARD_INPUT_HEADER_DIR)" \
		"$(NP2_KEYBOARD_INPUT_SOURCE)" \
		"$(NP2_KEYBOARD_INPUT_TEST_SOURCE)" \
		-o "$(NP2_KEYBOARD_INPUT_TEST_OUTPUT)"
	@"$(NP2_KEYBOARD_INPUT_TEST_OUTPUT)"
	@printf '%s\n' 'NP2 keyboard input contract and mapping tests passed.'

test-np2-keyboard-ownership:
	@mkdir -p "$(dir $(NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(NP2_KEYBOARD_INPUT_HEADER_DIR)" \
		"$(NP2_KEYBOARD_INPUT_SOURCE)" \
		"$(NP2_KEYBOARD_INPUT_OWNERSHIP_SOURCE)" \
		"$(NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_SOURCE)" \
		-o "$(NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_OUTPUT)"
	@"$(NP2_KEYBOARD_INPUT_OWNERSHIP_TEST_OUTPUT)"
	@printf '%s\n' 'NP2 keyboard ownership tests passed.'

test-usb-hid-keyboard-adapter:
	@mkdir -p "$(dir $(USB_HID_KEYBOARD_ADAPTER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(HID_BOOT_KEYBOARD_HEADER_DIR)" \
		-c "$(HID_BOOT_KEYBOARD_SOURCE)" \
		-o "$(USB_HID_KEYBOARD_ADAPTER_PARSER_OBJECT)"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(USB_HID_KEYBOARD_ADAPTER_HEADER_DIR)" \
		-I"$(HID_BOOT_KEYBOARD_HEADER_DIR)" \
		-I"$(NP2_KEYBOARD_INPUT_HEADER_DIR)" \
		"$(USB_HID_KEYBOARD_ADAPTER_SOURCE)" \
		"$(USB_HID_KEYBOARD_ADAPTER_TEST_SOURCE)" \
		"$(USB_HID_KEYBOARD_ADAPTER_PARSER_OBJECT)" \
		-o "$(USB_HID_KEYBOARD_ADAPTER_TEST_OUTPUT)"
	@"$(USB_HID_KEYBOARD_ADAPTER_TEST_OUTPUT)"
	@printf '%s\n' 'USB HID keyboard adapter tests passed.'

test-keyboard-input: test-np2-keyboard-input

test-np2video-motion-oracle:
	@mkdir -p "$(dir $(NP2VIDEO_MOTION_ORACLE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(NP2VIDEO_MOTION_ORACLE_HEADER_DIR)" \
		"$(NP2VIDEO_MOTION_ORACLE_SOURCE)" \
		"$(NP2VIDEO_MOTION_ORACLE_TEST_SOURCE)" \
		-o "$(NP2VIDEO_MOTION_ORACLE_TEST_OUTPUT)"
	@"$(NP2VIDEO_MOTION_ORACLE_TEST_OUTPUT)"

test-headless-scrnmng-presentation:
	@mkdir -p "$(dir $(SCRNMNG_PRESENTATION_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -DBYTESEX_LITTLE \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(CURDIR)/presentation" "$(NP2_CRC32_SOURCE)" \
		"$(PRESENTATION_SOURCE)" "$(HEADLESS_SCRNMNG_SOURCE)" \
		"$(CURDIR)/backend/headless/scrnmng_storage.c" \
		"$(SCRNMNG_PRESENTATION_TEST_SOURCE)" \
		-o "$(SCRNMNG_PRESENTATION_TEST_OUTPUT)"
	@"$(SCRNMNG_PRESENTATION_TEST_OUTPUT)"

test-presentation-contract: test-presentation-publisher test-headless-scrnmng-presentation test-p4-nano-live-display-contract
	@printf '%s\n' 'Portable presentation contract tests passed.'

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
