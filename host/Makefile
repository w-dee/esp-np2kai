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
P4_NANO_BOARD_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_board/include
P4_NANO_BOARD_TEST_SOURCE := $(CURDIR)/tests/p4_nano_board_test.cpp
P4_NANO_BOARD_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_board_test
P4_NANO_BOARD_STATIC_TEST := $(HOST_ROOT)/tools/test_p4_nano_board_service.py
P4_NANO_AUDIO_TONE_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_audio_output/include
P4_NANO_AUDIO_TONE_TEST_SOURCE := $(CURDIR)/tests/p4_nano_audio_tone_test.cpp
P4_NANO_AUDIO_TONE_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio_tone_test
P4_NANO_AUDIO_TONE_STATIC_TEST := $(HOST_ROOT)/tools/test_p4_nano_audio_output.py
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
OPNGEN_S98_S3_FORMAL_RUNNER := $(CURDIR)/tests/run_opngen_s98_s3_formal.py
OPNGEN_S98_S3_DESCRIPTOR := $(HOST_ROOT)/tools/emu/opngen_s98_fixture_golden.json
OPNGEN_S98_S3_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_opngen_s98_fixture_log.py
OPNGEN_S98_S3_VALIDATOR_TEST := $(HOST_ROOT)/tools/emu/test_opngen_s98_fixture_validator.py
RETROFM_S98_TEST_SOURCE := $(CURDIR)/tests/opngen_s98_s3_test.c
RETROFM_S98_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_retrofm_s98
RETROFM_S98_FIXTURE := $(HOST_ROOT)/testdata/s98/retrofm-pocket-demo-strict.s98
RETROFM_S98_MANIFEST := $(HOST_ROOT)/tools/emu/retrofm_pocket_fixture_provenance.json
RETROFM_S98_RUNNER := $(HOST_ROOT)/tools/emu/run_retrofm_s98.py
RETROFM_S98_PROVENANCE_TEST := $(HOST_ROOT)/tools/emu/test_retrofm_pocket_fixture.py
RETROFM_S98_DESCRIPTOR := $(HOST_ROOT)/tools/emu/opngen_retrofm_s98_golden.json
RETROFM_S98_PLAYBACK_VALIDATOR_TEST := $(HOST_ROOT)/tools/emu/test_retrofm_s98_validator.py
OPNGEN_PCM_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c
OPNGEN_SPSC_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_spsc.c
OPNGEN_E1B_STREAM_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_e1b_stream.c
OPNGEN_E1B_TEST_SOURCE := $(CURDIR)/tests/opngen_e1b_spsc_test.c
OPNGEN_E1B_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1b_spsc_test
OPNGEN_E1B_TSAN_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1b_spsc_test_tsan
OPNGEN_E1B_FORMAL_RUNNER := $(CURDIR)/tests/run_opngen_e1b_formal.py
OPNGEN_E1B_OBSERVER_TEST_SOURCE := $(CURDIR)/tests/opngen_e1b_observer_test.c
OPNGEN_E1B_OBSERVER_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1b_observer_test
OPNGEN_PCM_RING_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_ring.c
OPNGEN_PCM_RING_TEST_SOURCE := $(CURDIR)/tests/opngen_pcm_ring_test.c
OPNGEN_PCM_RING_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_pcm_ring_test
OPNGEN_PCM_RING_ASAN_OUTPUT := $(BUILD_DIR)/tests/opngen_pcm_ring_test_asan
OPNGEN_PCM_RING_UBSAN_OUTPUT := $(BUILD_DIR)/tests/opngen_pcm_ring_test_ubsan
OPNGEN_PCM_RING_TSAN_OUTPUT := $(BUILD_DIR)/tests/opngen_pcm_ring_test_tsan
NP2_PCM_OUTPUT_SOURCE := $(HOST_ROOT)/firmware/components/np2pcm_output/np2pcm_output.c
NP2_PCM_OUTPUT_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2pcm_output/include
NP2_PCM_OUTPUT_TEST_SOURCE := $(CURDIR)/tests/np2pcm_output_test.c
NP2_PCM_OUTPUT_TEST_OUTPUT := $(BUILD_DIR)/tests/np2pcm_output_test
NP2_PCM_OUTPUT_ASAN_OUTPUT := $(BUILD_DIR)/tests/np2pcm_output_test_asan
NP2_PCM_OUTPUT_UBSAN_OUTPUT := $(BUILD_DIR)/tests/np2pcm_output_test_ubsan
NP2_PCM_OUTPUT_TSAN_OUTPUT := $(BUILD_DIR)/tests/np2pcm_output_test_tsan
P4_NANO_AUDIO86_PHYSICAL_SINK_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.c
P4_NANO_AUDIO86_PHYSICAL_SINK_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_audio86_physical_sink/include
P4_NANO_AUDIO86_PHYSICAL_SINK_TEST_SOURCE := $(CURDIR)/tests/p4_nano_audio86_physical_sink_test.c
P4_NANO_AUDIO86_PHYSICAL_SINK_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_physical_sink_test
P4_NANO_AUDIO86_PHYSICAL_SINK_ASAN_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_physical_sink_test_asan
P4_NANO_AUDIO86_PHYSICAL_SINK_UBSAN_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_physical_sink_test_ubsan
P4_NANO_AUDIO86_PHYSICAL_SINK_TSAN_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_physical_sink_test_tsan
P4_NANO_AUDIO86_PHYSICAL_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_p4_audio86_physical_sink_log.py
P4_NANO_AUDIO86_PHYSICAL_MUTATIONS := $(HOST_ROOT)/tools/emu/test_validate_p4_audio86_physical_sink_log.py
P4_NANO_AUDIO86_PHYSICAL_MANIFEST := $(HOST_ROOT)/tools/emu/test_p4_audio86_physical_sink_manifest.py
P4_NANO_AUDIO86_PHYSICAL_S1_MUTATIONS := $(HOST_ROOT)/tools/emu/test_validate_p4_audio86_physical_s1_log.py
P4_NANO_AUDIO86_PHYSICAL_S2_MUTATIONS := $(HOST_ROOT)/tools/emu/test_validate_p4_audio86_physical_s2_log.py
P4_NANO_AUDIO86_PHYSICAL_5D3_S1_MUTATIONS := $(HOST_ROOT)/tools/emu/test_validate_p4_audio86_physical_5d3_s1_log.py
P4_NANO_AUDIO86_PHYSICAL_S1_SOURCE := $(HOST_ROOT)/tools/test_p4_audio86_physical_s1_evidence.py
P4_NANO_AUDIO86_PHYSICAL_S2A_SOURCE := $(HOST_ROOT)/tools/test_p4_audio86_physical_s2_runtime.py
P4_NANO_AUDIO86_PHYSICAL_5D3_S1_SOURCE := $(HOST_ROOT)/tools/test_p4_audio86_physical_5d3_s1_runtime.py
P4_NANO_CAPTURE_TEST := $(HOST_ROOT)/tools/test_p4_nano_capture.py
P4_NANO_AUDIO86_TERMINAL_PREDICATE_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_audio86_guest_binding/include
P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_SOURCE := $(CURDIR)/tests/p4_nano_audio86_terminal_predicate_test.cpp
P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_terminal_predicate_test
P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_TEST_SOURCE := $(CURDIR)/tests/p4_nano_audio86_terminal_worker_timing_test.cpp
P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_terminal_worker_timing_test
P4_NANO_AUDIO86_PHYSICAL_IDF_SOURCE := $(HOST_ROOT)/tools/emu/check_p4_audio86_callback_idf_barrier.py
P4_NANO_AUDIO86_PHYSICAL_IDF_SOURCE_TEST := $(HOST_ROOT)/tools/emu/test_check_p4_audio86_callback_idf_source.py
P4_NANO_AUDIO86_PHYSICAL_PROJECT_INTERVAL := $(HOST_ROOT)/tools/emu/check_p4_audio86_project_interval.py
P4_NANO_AUDIO86_PHYSICAL_PROJECT_INTERVAL_TEST := $(HOST_ROOT)/tools/emu/test_check_p4_audio86_project_interval.py
P4_NANO_AUDIO86_SOURCE_PROVENANCE := $(HOST_ROOT)/tools/test_p4_audio86_source_provenance.py
OPNGEN_SYNTHETIC_WORKLOAD_SOURCE := $(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_synthetic_workload.c
OPNGEN_E1C_TEST_SOURCE := $(CURDIR)/tests/opngen_e1c_sustained_test.c
OPNGEN_E1C_TEST_OUTPUT := $(BUILD_DIR)/tests/opngen_e1c_sustained_test
OPNGEN_E1C_RUNNER := $(CURDIR)/tests/run_opngen_e1c.py
OPNGEN_E1C_FORMAL_RUNNER := $(CURDIR)/tests/run_opngen_e1c_formal.py
OPNGEN_E1C_DESCRIPTOR := $(HOST_ROOT)/tools/emu/opngen_sustained_workload_golden.json
OPNGEN_E1C_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_opngen_sustained_log.py
OPNGEN_E1C_VALIDATOR_TEST := $(HOST_ROOT)/tools/emu/test_opngen_sustained_validator.py
NP2_AUDIO86_FIXTURE_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_fixture/np2audio86_fixture.c
NP2_AUDIO86_CORE_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_core/np2audio86_core.c
NP2_AUDIO86_CORE_TRANSPORT_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_core/np2audio86_transport.c
NP2_AUDIO86_CORE_SOURCES := $(NP2_AUDIO86_CORE_SOURCE) $(NP2_AUDIO86_CORE_TRANSPORT_SOURCE)
NP2_AUDIO86_CORE_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2audio86_core/include
NP2_AUDIO86_CORE_TEST_SOURCE := $(CURDIR)/tests/np2audio86_core_test.c
NP2_AUDIO86_CORE_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_core_test
NP2_AUDIO86_PROGRESS_TEST_SOURCE := $(CURDIR)/tests/np2audio86_progress_test.c
NP2_AUDIO86_PROGRESS_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_progress_test
P4_NANO_AUDIO86_LIVE_SERVICE_SOURCE := $(HOST_ROOT)/firmware/components/p4_nano_audio86_live_service/p4_nano_audio86_live_service.c
P4_NANO_AUDIO86_LIVE_SERVICE_HEADER_DIR := $(HOST_ROOT)/firmware/components/p4_nano_audio86_live_service/include
P4_NANO_AUDIO86_LIVE_SERVICE_TEST_SOURCE := $(CURDIR)/tests/p4_nano_audio86_live_service_test.c
P4_NANO_AUDIO86_LIVE_SERVICE_TEST_OUTPUT := $(BUILD_DIR)/tests/p4_nano_audio86_live_service_test
NP2_AUDIO86_GUEST_PROGRAM_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_fixture/np2audio86_guest_program.c
NP2_AUDIO86_GUEST_EVIDENCE_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_fixture/np2audio86_guest_evidence.c
NP2_AUDIO86_TRANSACTION_TEST_SOURCE := $(CURDIR)/tests/np2audio86_guest_transaction_test.c
NP2_AUDIO86_TRANSACTION_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_transaction
NP2_AUDIO86_FIXTURE_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2audio86_fixture/include
NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR := $(HOST_ROOT)/firmware/components/np2audio86_fixture
NP2_AUDIO86_FIXTURE_MAIN_SOURCE := $(CURDIR)/tests/np2audio86_fixture_main.c
NP2_AUDIO86_FIXTURE_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_fixture
NP2_AUDIO86_FIXTURE_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_fixture.log
NP2_AUDIO86_FIXTURE_GOLDEN := $(HOST_ROOT)/tools/emu/np2audio86_fixture_golden.json
NP2_AUDIO86_RUNTIME_SOURCE := $(CURDIR)/tests/np2audio86_guest_runtime.c
NP2_AUDIO86_RUNTIME_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_runtime
NP2_AUDIO86_RUNTIME_PREPARED := $(BUILD_DIR)/audio86-runtime-prepared
NP2_AUDIO86_RUNTIME_BOARD := $(NP2_AUDIO86_RUNTIME_PREPARED)/patched-src/cbus/board86.c
NP2_AUDIO86_RUNTIME_PCM := $(NP2_AUDIO86_RUNTIME_PREPARED)/patched-src/cbus/pcm86io.c
NP2_AUDIO86_ADAPTER_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/np2audio86_guest_adapter.c
AUDIO86_PATCH_SET := $(CURDIR)/patches/np2kai/audio86/patch-set.json
NP2_AUDIO86_RUNTIME_RUNNER := $(CURDIR)/tests/run_np2audio86_guest_runtime.py
NP2_AUDIO86_RUNTIME_GOLDEN := $(CURDIR)/probe/audio86_guest_runtime_golden.json
NP2_AUDIO86_RUNTIME_EXTRA_CFLAGS ?=
NP2_AUDIO86_RUNTIME_EXTRA_LDFLAGS ?=
NP2_AUDIO86_SYNC_PCM_SOURCE := $(CURDIR)/tests/np2audio86_guest_sync_pcm.c
NP2_AUDIO86_SYNC_PCM_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_sync_pcm
NP2_AUDIO86_SYNC_PCM_GOLDEN := $(CURDIR)/probe/audio86_guest_sync_pcm_golden.json
NP2_AUDIO86_SYNC_PCM_RUNNER := $(CURDIR)/tests/run_np2audio86_guest_sync_pcm.py
NP2_AUDIO86_SYNC_PCM_EXTRA_CFLAGS ?=
NP2_AUDIO86_SYNC_PCM_EXTRA_LDFLAGS ?=
NP2_AUDIO86_SYNC_PCM_EXTRA_SOURCES ?=
NP2_AUDIO86_SUSTAINED_2S_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_sustained_2s
NP2_AUDIO86_SUSTAINED_2S_GOLDEN := $(CURDIR)/probe/audio86_guest_sustained_2s_golden.json
NP2_AUDIO86_SUSTAINED_2S_RUNNER := $(CURDIR)/tests/run_np2audio86_guest_sustained_2s.py
NP2_AUDIO86_SUSTAINED_EVIDENCE_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_fixture/np2audio86_sustained_evidence.c
NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_SOURCE := $(CURDIR)/tests/np2audio86_sustained_evidence_test.c
NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_sustained_evidence_test
NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_CFLAGS ?=
NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_LDFLAGS ?=
NP2_AUDIO86_GUEST_ASYNC_SOURCE := $(CURDIR)/tests/np2audio86_guest_async_test.c
NP2_AUDIO86_GUEST_ASYNC_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_async
NP2_AUDIO86_GUEST_ASYNC_RUNNER := $(CURDIR)/tests/run_np2audio86_guest_async.py
NP2_AUDIO86_GUEST_ASYNC_EXTRA_CFLAGS ?=
NP2_AUDIO86_GUEST_ASYNC_EXTRA_LDFLAGS ?=
NP2_AUDIO86_GUEST_ASYNC_SHARED_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_fixture/np2audio86_guest_async.c
NP2_AUDIO86_RUNTIME_TRANSPORT_SOURCE := $(HOST_ROOT)/firmware/components/np2audio86_core/np2audio86_runtime_transport.c
NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_SOURCE := $(CURDIR)/tests/np2audio86_runtime_transport_test.c
NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_runtime_transport
NP2_AUDIO86_GUEST_ASYNC_HARDENING_SOURCE := $(CURDIR)/tests/np2audio86_guest_async_hardening_test.c
NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT := $(BUILD_DIR)/tests/np2audio86_guest_async_hardening
NP2_AUDIO86_GUEST_ASYNC_HARDENING_RUNNER := $(CURDIR)/tests/run_np2audio86_guest_async_hardening.py
NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_CFLAGS ?=
NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_LDFLAGS ?=
AUDIO86_RUNTIME_CPU_SOURCES := \
	$(VENDOR_SRC)/i286c/i286c.c \
	$(VENDOR_SRC)/i286c/i286c_0f.c \
	$(VENDOR_SRC)/i286c/i286c_8x.c \
	$(VENDOR_SRC)/i286c/i286c_ea.c \
	$(VENDOR_SRC)/i286c/i286c_f6.c \
	$(VENDOR_SRC)/i286c/i286c_fe.c \
	$(VENDOR_SRC)/i286c/i286c_mn.c \
	$(VENDOR_SRC)/i286c/i286c_rp.c \
	$(VENDOR_SRC)/i286c/i286c_sf.c \
	$(VENDOR_SRC)/i286c/v30patch.c
NP2_AUDIO86_FIXTURE_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_np2audio86_fixture_log.py
NP2_AUDIO86_ASYNC_TEST_SOURCE := $(CURDIR)/tests/np2audio86_async_test.c
NP2_AUDIO86_ASYNC_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async
NP2_AUDIO86_ASYNC_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async.log
NP2_AUDIO86_ASYNC_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_np2audio86_async_log.py
NP2_AUDIO86_ASYNC_FORMAL_RUNNER := $(CURDIR)/tests/run_np2audio86_async_formal.py
NP2_AUDIO86_HARDENING_TEST_SOURCE := $(CURDIR)/tests/np2audio86_async_hardening_test.c
NP2_AUDIO86_HARDENING_TEST_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening
NP2_AUDIO86_HARDENING_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening.log
NP2_AUDIO86_HARDENING_RUNNER := $(CURDIR)/tests/run_np2audio86_hardening.py
NP2_AUDIO86_HARDENING_VALIDATOR := $(HOST_ROOT)/tools/emu/validate_np2audio86_hardening_log.py
NP2_AUDIO86_HARDENING_ASAN_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening_asan
NP2_AUDIO86_HARDENING_ASAN_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening_asan.log
NP2_AUDIO86_HARDENING_UBSAN_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening_ubsan
NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening_ubsan.log
NP2_AUDIO86_HARDENING_SOAK_LOG_OUTPUT := $(BUILD_DIR)/tests/np2audio86_async_hardening_soak.log
AUDIO86_GUEST_CONTRACT_SCRIPT := $(CURDIR)/probe/audio86_guest_contract.py
AUDIO86_GUEST_CONTRACT_OUTPUT := $(BUILD_DIR)/audio86-contract
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

.PHONY: phase1-probe phase1-config phase1-prepare-sources phase1-probe-mapped phase1-link-probe-mapped test-audio86-guest-contract headless-runner test-headless-runner-np2test test-headless-runner-attach test-headless-runner-lifecycle video-runner test-video-runner-reference test-video-runner-reference-bmp test-video-runner-gfx-vram-reference test-video-runner-gfx-vram-reference-bmp test-video-gdc-reference test-video-gdc-reference-bmp test-video-runner-live-reference test-video-runner-live-reference-bmp test-video-runner-cli test-video-runner-golden test-video-gfx-vram-golden test-video-gdc-golden test-video-golden-checker test-video-golden-header test-video-golden-log test-video-control-v1 test-sha256 test-crc32 test-opngen-fixture test-np2audio86-core test-audio86-progress test-p4-nano-audio86-live-service test-np2audio86-fixture test-np2audio86-runtime-transport build-np2audio86-async test-np2audio86-async test-np2audio86-async-formal build-np2audio86-hardening test-np2audio86-hardening build-np2audio86-hardening-asan test-np2audio86-hardening-asan build-np2audio86-hardening-ubsan test-np2audio86-hardening-ubsan test-np2audio86-hardening-soak test-opngen-synth-event test-opngen-s98-s3 build-opngen-retrofm-s98 test-opngen-retrofm-s98-formal test-opngen-retrofm-s98-validator test-retrofm-provenance build-opngen-s98-s3 test-opngen-s98-s3-formal test-opngen-s98-s3-validator test-opngen-s98-accepted build-opngen-e1b test-opngen-e1b test-opngen-e1b-formal build-opngen-e1b-tsan test-opngen-e1b-tsan build-opngen-e1c test-opngen-e1c test-opngen-e1c-validator test-opngen-e1c-formal test-result-v1-parser test-np2kbd-control-v1 test-np2kbd-result-v1 test-np2kbd test-execution-controller test-headless-mouse test-headless-sound test-headless-timing test-headless-timemng test-headless-sysmng test-headless-taskmng test-headless-dosio test-headless-fontmng test-headless-scrnmng test-headless-scrnmng-snapshot test-presentation-publisher test-headless-scrnmng-presentation test-presentation-contract test-p4-nano-board test-p4-nano-board-static test-p4-nano-audio-tone test-p4-nano-display-pattern test-p4-nano-display-transform test-p4-nano-display-transform-diagnostic test-p4-nano-psram-bandwidth test-p4-nano-psram-read-control test-p4-nano-live-display-contract test-p4-nano-overlap-trace test-p4-nano-pc98-runtime-contract test-p4-nano-usb-keyboard-teardown-contract test-np2video-motion-oracle test-headless-scrnmng-bmp test-headless-commng test-np2runtime test-np2runtime-lifecycle test-np2runtime-machine-config test-np2runtime-runtime test-np2-keyboard-input test-np2-keyboard-ownership test-keyboard-input test-np2-keyboard-validation test-usb-hid-keyboard-adapter

.PHONY: build-audio86-guest-runtime test-audio86-guest-runtime build-audio86-guest-runtime-asan test-audio86-guest-runtime-asan build-audio86-guest-runtime-ubsan test-audio86-guest-runtime-ubsan build-audio86-guest-sync-pcm test-audio86-guest-sync-pcm build-audio86-guest-sync-pcm-asan test-audio86-guest-sync-pcm-asan build-audio86-guest-sync-pcm-ubsan test-audio86-guest-sync-pcm-ubsan build-audio86-guest-sustained-2s test-audio86-guest-sustained-2s build-audio86-sustained-evidence test-audio86-sustained-evidence build-audio86-sustained-evidence-asan test-audio86-sustained-evidence-asan build-audio86-sustained-evidence-ubsan test-audio86-sustained-evidence-ubsan build-audio86-guest-async-runtime test-audio86-guest-async-runtime build-audio86-guest-async-runtime-asan test-audio86-guest-async-runtime-asan build-audio86-guest-async-runtime-ubsan test-audio86-guest-async-runtime-ubsan build-audio86-guest-async-hardening test-audio86-guest-async-hardening build-audio86-guest-async-hardening-asan test-audio86-guest-async-hardening-asan build-audio86-guest-async-hardening-ubsan test-audio86-guest-async-hardening-ubsan build-audio86-guest-transaction test-audio86-guest-transaction build-audio86-guest-transaction-asan test-audio86-guest-transaction-asan build-audio86-guest-transaction-ubsan test-audio86-guest-transaction-ubsan
.PHONY: test-p4-nano-ppa-rotation-reference
.PHONY: test-p4-nano-board-static
.PHONY: build-opngen-pcm-ring test-opngen-pcm-ring build-opngen-pcm-ring-asan test-opngen-pcm-ring-asan build-opngen-pcm-ring-ubsan test-opngen-pcm-ring-ubsan build-opngen-pcm-ring-tsan test-opngen-pcm-ring-tsan
.PHONY: build-np2pcm-output test-np2pcm-output test-np2pcm-output-stress build-np2pcm-output-asan test-np2pcm-output-asan build-np2pcm-output-ubsan test-np2pcm-output-ubsan build-np2pcm-output-tsan test-np2pcm-output-tsan
.PHONY: build-p4-audio86-physical-sink test-p4-audio86-physical-sink build-p4-audio86-physical-sink-asan test-p4-audio86-physical-sink-asan build-p4-audio86-physical-sink-ubsan test-p4-audio86-physical-sink-ubsan build-p4-audio86-physical-sink-tsan test-p4-audio86-physical-sink-tsan test-p4-audio86-physical-s1 test-p4-audio86-physical-s2 test-p4-audio86-physical-5d3-s1

test-audio86-guest-contract:
	@$(PYTHON) "$(AUDIO86_GUEST_CONTRACT_SCRIPT)" \
		--output-dir "$(AUDIO86_GUEST_CONTRACT_OUTPUT)" \
		--compiler "$(CC)" --nm "$(NM)"
	@printf '%s\n' 'PC-9801-86 narrow S3 compile/link and ownership contract passed.'

build-audio86-guest-runtime:
	@mkdir -p "$(dir $(NP2_AUDIO86_RUNTIME_OUTPUT))"
	@$(PYTHON) "$(PREPARER)" \
		--vendor-root "$(VENDOR_ROOT)" \
		--import-manifest "$(IMPORT_MANIFEST)" \
		--patch-set "$(AUDIO86_PATCH_SET)" \
		--output-root "$(NP2_AUDIO86_RUNTIME_PREPARED)"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration \
		-DDISABLE_SOUND=1 -DNP2_ASYNC_AUDIO86=1 -DNP2AUDIO86_GUEST_TEST=1 -DBYTESEX_LITTLE \
		-DOSLANG_UTF8 -DSUPPORT_UTF8 -DMEMOPTIMIZE=2 \
		-ffunction-sections -fdata-sections -fcommon $(NP2_AUDIO86_RUNTIME_EXTRA_CFLAGS) \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_fixture/include" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" "$(NP2_AUDIO86_GUEST_PROGRAM_SOURCE)" "$(NP2_AUDIO86_GUEST_EVIDENCE_SOURCE)" "$(NP2_AUDIO86_RUNTIME_BOARD)" \
		"$(NP2_AUDIO86_RUNTIME_PCM)" "$(VENDOR_SRC)/cbus/cbuscore.c" \
		"$(VENDOR_SRC)/io/iocore.c" "$(VENDOR_SRC)/io/pic.c" \
		"$(VENDOR_SRC)/nevent.c" "$(VENDOR_SRC)/common/lstarray.c" \
		$(AUDIO86_RUNTIME_CPU_SOURCES) \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_SOURCE)" -Wl,--gc-sections -lm \
		$(NP2_AUDIO86_RUNTIME_EXTRA_LDFLAGS) \
		-o "$(NP2_AUDIO86_RUNTIME_OUTPUT)"

test-audio86-guest-runtime: build-audio86-guest-runtime
	@$(PYTHON) "$(NP2_AUDIO86_RUNTIME_RUNNER)" \
		--binary "$(NP2_AUDIO86_RUNTIME_OUTPUT)" \
		--golden "$(NP2_AUDIO86_RUNTIME_GOLDEN)" --repetitions 3
	@$(MAKE) --no-print-directory test-audio86-guest-runtime-asan
	@$(MAKE) --no-print-directory test-audio86-guest-runtime-ubsan

build-audio86-guest-runtime-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_RUNTIME_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_runtime_asan" \
		NP2_AUDIO86_RUNTIME_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_RUNTIME_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-guest-runtime

test-audio86-guest-runtime-asan: build-audio86-guest-runtime-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		"$(BUILD_DIR)/tests/np2audio86_guest_runtime_asan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_RUNTIME_ASAN=PASS'

build-audio86-guest-runtime-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_RUNTIME_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_runtime_ubsan" \
		NP2_AUDIO86_RUNTIME_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined" \
		NP2_AUDIO86_RUNTIME_EXTRA_LDFLAGS="-fsanitize=undefined" \
		build-audio86-guest-runtime

test-audio86-guest-runtime-ubsan: build-audio86-guest-runtime-ubsan
	@"$(BUILD_DIR)/tests/np2audio86_guest_runtime_ubsan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_RUNTIME_UBSAN=PASS'

build-audio86-guest-transaction:
	@mkdir -p "$(dir $(NP2_AUDIO86_TRANSACTION_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror $(NP2_AUDIO86_TRANSACTION_TEST_EXTRA_CFLAGS) \
		-I"$(CURDIR)/compat" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_fixture/include" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-DNP2AUDIO86_GUEST_TEST=1 \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" "$(NP2_AUDIO86_GUEST_PROGRAM_SOURCE)" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_AUDIO86_TRANSACTION_TEST_SOURCE)" \
		$(NP2_AUDIO86_TRANSACTION_TEST_EXTRA_LDFLAGS) -o "$(NP2_AUDIO86_TRANSACTION_TEST_OUTPUT)"

test-audio86-guest-transaction: build-audio86-guest-transaction
	@"$(NP2_AUDIO86_TRANSACTION_TEST_OUTPUT)"

build-audio86-guest-transaction-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_TRANSACTION_TEST_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_transaction_asan" \
		NP2_AUDIO86_TRANSACTION_TEST_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_TRANSACTION_TEST_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-guest-transaction

test-audio86-guest-transaction-asan: build-audio86-guest-transaction-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "$(BUILD_DIR)/tests/np2audio86_guest_transaction_asan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_TRANSACTION_ASAN=PASS'

build-audio86-guest-transaction-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_TRANSACTION_TEST_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_transaction_ubsan" \
		NP2_AUDIO86_TRANSACTION_TEST_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined" \
		NP2_AUDIO86_TRANSACTION_TEST_EXTRA_LDFLAGS="-fsanitize=undefined" \
		build-audio86-guest-transaction

test-audio86-guest-transaction-ubsan: build-audio86-guest-transaction-ubsan
	@"$(BUILD_DIR)/tests/np2audio86_guest_transaction_ubsan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_TRANSACTION_UBSAN=PASS'

build-audio86-guest-sync-pcm:
	@mkdir -p "$(dir $(NP2_AUDIO86_SYNC_PCM_OUTPUT))"
	@$(PYTHON) "$(PREPARER)" \
		--vendor-root "$(VENDOR_ROOT)" \
		--import-manifest "$(IMPORT_MANIFEST)" \
		--patch-set "$(AUDIO86_PATCH_SET)" \
		--output-root "$(BUILD_DIR)/audio86-sync-pcm-prepared"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration \
		-DDISABLE_SOUND=1 -DNP2_ASYNC_AUDIO86=1 -DNP2AUDIO86_GUEST_TEST=1 \
		-DNP2AUDIO86_GUEST_RUNTIME_NO_MAIN=1 -DBYTESEX_LITTLE \
		-DOSLANG_UTF8 -DSUPPORT_UTF8 -DMEMOPTIMIZE=2 \
		-ffunction-sections -fdata-sections -fcommon $(NP2_AUDIO86_SYNC_PCM_EXTRA_CFLAGS) \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_fixture/include" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		-I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_LIVE_SERVICE_HEADER_DIR)" \
	"$(NP2_AUDIO86_ADAPTER_SOURCE)" "$(NP2_AUDIO86_GUEST_PROGRAM_SOURCE)" "$(NP2_AUDIO86_GUEST_EVIDENCE_SOURCE)" \
		"$(BUILD_DIR)/audio86-sync-pcm-prepared/patched-src/cbus/board86.c" \
		"$(BUILD_DIR)/audio86-sync-pcm-prepared/patched-src/cbus/pcm86io.c" \
		"$(VENDOR_SRC)/cbus/cbuscore.c" "$(VENDOR_SRC)/io/iocore.c" \
		"$(VENDOR_SRC)/io/pic.c" "$(VENDOR_SRC)/nevent.c" \
		"$(VENDOR_SRC)/common/lstarray.c" $(AUDIO86_RUNTIME_CPU_SOURCES) \
	"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) "$(NP2_AUDIO86_GUEST_ASYNC_SHARED_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(OPNGEN_PCM_SOURCE)" "$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_SOURCE)" "$(NP2_AUDIO86_SYNC_PCM_SOURCE)" \
		$(NP2_AUDIO86_SYNC_PCM_EXTRA_SOURCES) \
		-Wl,--gc-sections -lm $(NP2_AUDIO86_SYNC_PCM_EXTRA_LDFLAGS) \
		-o "$(NP2_AUDIO86_SYNC_PCM_OUTPUT)"

test-audio86-guest-sync-pcm: build-audio86-guest-sync-pcm
	@$(PYTHON) "$(NP2_AUDIO86_SYNC_PCM_RUNNER)" \
		--binary "$(NP2_AUDIO86_SYNC_PCM_OUTPUT)" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 3
	@$(MAKE) --no-print-directory test-audio86-guest-sync-pcm-asan
	@$(MAKE) --no-print-directory test-audio86-guest-sync-pcm-ubsan

build-audio86-guest-sync-pcm-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_SYNC_PCM_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_sync_pcm_asan" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-guest-sync-pcm

test-audio86-guest-sync-pcm-asan: build-audio86-guest-sync-pcm-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		"$(BUILD_DIR)/tests/np2audio86_guest_sync_pcm_asan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_SYNC_PCM_ASAN=PASS'

build-audio86-guest-sync-pcm-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_SYNC_PCM_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_sync_pcm_ubsan" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_LDFLAGS="-fsanitize=undefined -fno-sanitize=signed-integer-overflow,shift" \
		build-audio86-guest-sync-pcm

test-audio86-guest-sync-pcm-ubsan: build-audio86-guest-sync-pcm-ubsan
	@"$(BUILD_DIR)/tests/np2audio86_guest_sync_pcm_ubsan" >/dev/null
	@printf '%s\n' 'AUDIO86_GUEST_SYNC_PCM_UBSAN=PASS'

build-audio86-guest-sustained-2s:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_SYNC_PCM_OUTPUT="$(NP2_AUDIO86_SUSTAINED_2S_OUTPUT)" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_CFLAGS="-DNP2AUDIO86_GUEST_SUSTAINED_2S=1 -pthread" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_LDFLAGS="-pthread" \
		NP2_AUDIO86_SYNC_PCM_EXTRA_SOURCES="$(NP2_AUDIO86_SUSTAINED_EVIDENCE_SOURCE) $(OPNGEN_PCM_RING_SOURCE) $(NP2_PCM_OUTPUT_SOURCE) $(NP2_AUDIO86_RUNTIME_TRANSPORT_SOURCE) $(P4_NANO_AUDIO86_LIVE_SERVICE_SOURCE)" \
		build-audio86-guest-sync-pcm

test-audio86-guest-sustained-2s: build-audio86-guest-sustained-2s
	@$(PYTHON) "$(NP2_AUDIO86_SUSTAINED_2S_RUNNER)" \
		--binary "$(NP2_AUDIO86_SUSTAINED_2S_OUTPUT)" \
		--golden "$(NP2_AUDIO86_SUSTAINED_2S_GOLDEN)" --repetitions 3

build-audio86-sustained-evidence:
	@mkdir -p "$(dir $(NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		$(NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_CFLAGS) \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(CURDIR)/compat" \
		"$(NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_SOURCE)" \
		"$(NP2_AUDIO86_SUSTAINED_EVIDENCE_SOURCE)" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		$(NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_LDFLAGS) \
		-o "$(NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT)"

test-audio86-sustained-evidence: build-audio86-sustained-evidence
	@"$(NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT)"
	@$(MAKE) --no-print-directory test-audio86-sustained-evidence-asan
	@$(MAKE) --no-print-directory test-audio86-sustained-evidence-ubsan

test-audio86-sustained-evidence-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT="$(BUILD_DIR)/tests/np2audio86_sustained_evidence_asan" \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-sustained-evidence
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		"$(BUILD_DIR)/tests/np2audio86_sustained_evidence_asan" >/dev/null
	@printf '%s\n' 'SUSTAINED_EVIDENCE_ASAN=PASS'

test-audio86-sustained-evidence-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_TEST_OUTPUT="$(BUILD_DIR)/tests/np2audio86_sustained_evidence_ubsan" \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
		NP2_AUDIO86_SUSTAINED_EVIDENCE_EXTRA_LDFLAGS="-fsanitize=undefined" \
		build-audio86-sustained-evidence
	@"$(BUILD_DIR)/tests/np2audio86_sustained_evidence_ubsan" >/dev/null
	@printf '%s\n' 'SUSTAINED_EVIDENCE_UBSAN=PASS'

build-audio86-guest-async-runtime:
	@mkdir -p "$(dir $(NP2_AUDIO86_GUEST_ASYNC_OUTPUT))"
	@$(PYTHON) "$(PREPARER)" \
		--vendor-root "$(VENDOR_ROOT)" \
		--import-manifest "$(IMPORT_MANIFEST)" \
		--patch-set "$(AUDIO86_PATCH_SET)" \
		--output-root "$(BUILD_DIR)/audio86-guest-async-prepared"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND=1 -DNP2_ASYNC_AUDIO86=1 -DNP2AUDIO86_GUEST_TEST=1 \
		-DNP2AUDIO86_GUEST_RUNTIME_NO_MAIN=1 -DBYTESEX_LITTLE \
		-DOSLANG_UTF8 -DSUPPORT_UTF8 -DMEMOPTIMIZE=2 \
		-ffunction-sections -fdata-sections -fcommon $(NP2_AUDIO86_GUEST_ASYNC_EXTRA_CFLAGS) \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_fixture/include" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" "$(NP2_AUDIO86_GUEST_PROGRAM_SOURCE)" "$(NP2_AUDIO86_GUEST_EVIDENCE_SOURCE)" \
		"$(BUILD_DIR)/audio86-guest-async-prepared/patched-src/cbus/board86.c" \
		"$(BUILD_DIR)/audio86-guest-async-prepared/patched-src/cbus/pcm86io.c" \
		"$(VENDOR_SRC)/cbus/cbuscore.c" "$(VENDOR_SRC)/io/iocore.c" \
		"$(VENDOR_SRC)/io/pic.c" "$(VENDOR_SRC)/nevent.c" \
		"$(VENDOR_SRC)/common/lstarray.c" $(AUDIO86_RUNTIME_CPU_SOURCES) \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) "$(NP2_AUDIO86_GUEST_ASYNC_SHARED_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(OPNGEN_PCM_SOURCE)" "$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_SOURCE)" "$(NP2_AUDIO86_GUEST_ASYNC_SOURCE)" \
		-Wl,--gc-sections -lm $(NP2_AUDIO86_GUEST_ASYNC_EXTRA_LDFLAGS) \
		-o "$(NP2_AUDIO86_GUEST_ASYNC_OUTPUT)"

test-audio86-guest-async-runtime: build-audio86-guest-async-runtime
	@$(PYTHON) "$(NP2_AUDIO86_GUEST_ASYNC_RUNNER)" \
		--binary "$(NP2_AUDIO86_GUEST_ASYNC_OUTPUT)" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 10
	@$(MAKE) --no-print-directory test-audio86-guest-async-runtime-asan
	@$(MAKE) --no-print-directory test-audio86-guest-async-runtime-ubsan

build-audio86-guest-async-runtime-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_GUEST_ASYNC_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_async_asan" \
		NP2_AUDIO86_GUEST_ASYNC_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_GUEST_ASYNC_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-guest-async-runtime

test-audio86-guest-async-runtime-asan: build-audio86-guest-async-runtime-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $(PYTHON) \
		"$(NP2_AUDIO86_GUEST_ASYNC_RUNNER)" \
		--binary "$(BUILD_DIR)/tests/np2audio86_guest_async_asan" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 10
	@printf '%s\n' 'AUDIO86_GUEST_ASYNC_ASAN=PASS'

build-audio86-guest-async-runtime-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_GUEST_ASYNC_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_async_ubsan" \
		NP2_AUDIO86_GUEST_ASYNC_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
		NP2_AUDIO86_GUEST_ASYNC_EXTRA_LDFLAGS="-fsanitize=undefined -fno-sanitize=signed-integer-overflow,shift" \
		build-audio86-guest-async-runtime

test-audio86-guest-async-runtime-ubsan: build-audio86-guest-async-runtime-ubsan
	@$(PYTHON) "$(NP2_AUDIO86_GUEST_ASYNC_RUNNER)" \
		--binary "$(BUILD_DIR)/tests/np2audio86_guest_async_ubsan" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 10
	@printf '%s\n' 'AUDIO86_GUEST_ASYNC_UBSAN=PASS'

build-audio86-guest-async-hardening:
	@mkdir -p "$(dir $(NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT))"
	@$(PYTHON) "$(PREPARER)" \
		--vendor-root "$(VENDOR_ROOT)" \
		--import-manifest "$(IMPORT_MANIFEST)" \
		--patch-set "$(AUDIO86_PATCH_SET)" \
		--output-root "$(BUILD_DIR)/audio86-guest-async-hardening-prepared"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND=1 -DNP2_ASYNC_AUDIO86=1 -DNP2AUDIO86_GUEST_TEST=1 \
		-DNP2AUDIO86_GUEST_RUNTIME_NO_MAIN=1 \
		-DNP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST=1 \
		-DNP2_AUDIO86_ASYNC_EVENT_CAPACITY=4U -DNP2_AUDIO86_ASYNC_BYTE_CAPACITY=16U \
		-DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 -DMEMOPTIMIZE=2 \
		-ffunction-sections -fdata-sections -fcommon $(NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_CFLAGS) \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_fixture/include" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" "$(NP2_AUDIO86_GUEST_PROGRAM_SOURCE)" "$(NP2_AUDIO86_GUEST_EVIDENCE_SOURCE)" \
		"$(BUILD_DIR)/audio86-guest-async-hardening-prepared/patched-src/cbus/board86.c" \
		"$(BUILD_DIR)/audio86-guest-async-hardening-prepared/patched-src/cbus/pcm86io.c" \
		"$(VENDOR_SRC)/cbus/cbuscore.c" "$(VENDOR_SRC)/io/iocore.c" \
		"$(VENDOR_SRC)/io/pic.c" "$(VENDOR_SRC)/nevent.c" \
		"$(VENDOR_SRC)/common/lstarray.c" $(AUDIO86_RUNTIME_CPU_SOURCES) \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) "$(NP2_AUDIO86_GUEST_ASYNC_SHARED_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(OPNGEN_PCM_SOURCE)" "$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_SOURCE)" "$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_SOURCE)" \
		-Wl,--gc-sections -lm $(NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_LDFLAGS) \
		-o "$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT)"

test-audio86-guest-async-hardening: build-audio86-guest-async-hardening
	@$(PYTHON) "$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_RUNNER)" \
		--binary "$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT)" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 25

build-audio86-guest-async-hardening-asan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_async_hardening_asan" \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_LDFLAGS="-fsanitize=address" \
		build-audio86-guest-async-hardening

test-audio86-guest-async-hardening-asan: build-audio86-guest-async-hardening-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $(PYTHON) \
		"$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_RUNNER)" \
		--binary "$(BUILD_DIR)/tests/np2audio86_guest_async_hardening_asan" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 25
	@printf '%s\n' 'AUDIO86_GUEST_ASYNC_HARDENING_ASAN=PASS'

build-audio86-guest-async-hardening-ubsan:
	@$(MAKE) --no-print-directory \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_OUTPUT="$(BUILD_DIR)/tests/np2audio86_guest_async_hardening_ubsan" \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
		NP2_AUDIO86_GUEST_ASYNC_HARDENING_EXTRA_LDFLAGS="-fsanitize=undefined -fno-sanitize=signed-integer-overflow,shift" \
		build-audio86-guest-async-hardening

test-audio86-guest-async-hardening-ubsan: build-audio86-guest-async-hardening-ubsan
	@$(PYTHON) "$(NP2_AUDIO86_GUEST_ASYNC_HARDENING_RUNNER)" \
		--binary "$(BUILD_DIR)/tests/np2audio86_guest_async_hardening_ubsan" \
		--golden "$(NP2_AUDIO86_SYNC_PCM_GOLDEN)" --repetitions 25
	@printf '%s\n' 'AUDIO86_GUEST_ASYNC_HARDENING_UBSAN=PASS'

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

test-np2audio86-core:
	@mkdir -p "$(dir $(NP2_AUDIO86_CORE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration \
		-DNP2AUDIO86_GUEST_TEST=1 -DDISABLE_SOUND -DBYTESEX_LITTLE \
		-DOSLANG_UTF8 -DSUPPORT_UTF8 -ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		"$(NP2_AUDIO86_CORE_SOURCE)" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_AUDIO86_CORE_TEST_SOURCE)" -Wl,--gc-sections -lm \
		-o "$(NP2_AUDIO86_CORE_TEST_OUTPUT)"
	@"$(NP2_AUDIO86_CORE_TEST_OUTPUT)"

test-audio86-progress:
	@mkdir -p "$(dir $(NP2_AUDIO86_PROGRESS_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror \
		-DNP2AUDIO86_GUEST_TEST=1 \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" \
		"$(NP2_AUDIO86_PROGRESS_TEST_SOURCE)" \
		-o "$(NP2_AUDIO86_PROGRESS_TEST_OUTPUT)"
	@"$(NP2_AUDIO86_PROGRESS_TEST_OUTPUT)"

test-p4-nano-audio86-live-service:
	@mkdir -p "$(dir $(P4_NANO_AUDIO86_LIVE_SERVICE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DP4_NANO_AUDIO86_LIVE_SERVICE_TESTING=1 \
		-DNP2AUDIO86_GUEST_TEST=1 -DDISABLE_SOUND -DBYTESEX_LITTLE \
		-DOSLANG_UTF8 -DSUPPORT_UTF8 -ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2audio86_guest_adapter/include" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		-I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_LIVE_SERVICE_HEADER_DIR)" \
		"$(P4_NANO_AUDIO86_LIVE_SERVICE_SOURCE)" \
		"$(NP2_AUDIO86_ADAPTER_SOURCE)" \
		$(NP2_AUDIO86_CORE_SOURCES) "$(NP2_AUDIO86_RUNTIME_TRANSPORT_SOURCE)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(P4_NANO_AUDIO86_LIVE_SERVICE_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(P4_NANO_AUDIO86_LIVE_SERVICE_TEST_OUTPUT)"
	@"$(P4_NANO_AUDIO86_LIVE_SERVICE_TEST_OUTPUT)"

test-np2audio86-fixture:
	@mkdir -p "$(dir $(NP2_AUDIO86_FIXTURE_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_FIXTURE_MAIN_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_FIXTURE_TEST_OUTPUT)"
	@"$(NP2_AUDIO86_FIXTURE_TEST_OUTPUT)" > "$(NP2_AUDIO86_FIXTURE_LOG_OUTPUT)"
	@cat "$(NP2_AUDIO86_FIXTURE_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_FIXTURE_VALIDATOR)" \
		--log "$(NP2_AUDIO86_FIXTURE_LOG_OUTPUT)" \
		--golden "$(NP2_AUDIO86_FIXTURE_GOLDEN)"
	@printf '%s\n' 'NP2audio86 synchronous native fixture completed.'

test-np2audio86-runtime-transport:
	@mkdir -p "$(dir $(NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DNP2_AUDIO86_RESET_ORDINAL_TEST=1 \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_TRANSPORT_SOURCE)" \
		"$(OPNGEN_PCM_RING_SOURCE)" \
		"$(NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_OUTPUT)"
	@"$(NP2_AUDIO86_RUNTIME_TRANSPORT_TEST_OUTPUT)"

build-np2audio86-async:
	@mkdir -p "$(dir $(NP2_AUDIO86_ASYNC_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DNP2_AUDIO86_ASYNC_HOST \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_ASYNC_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_ASYNC_TEST_OUTPUT)"

test-np2audio86-async: build-np2audio86-async
	@"$(NP2_AUDIO86_ASYNC_TEST_OUTPUT)" > "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)"
	@cat "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_ASYNC_VALIDATOR)" \
		--log "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)" \
		--golden "$(NP2_AUDIO86_FIXTURE_GOLDEN)"
	@printf '%s\n' 'NP2audio86 asynchronous native proof completed.'

test-np2audio86-async-formal: build-np2audio86-async
	@$(PYTHON) "$(NP2_AUDIO86_ASYNC_FORMAL_RUNNER)" \
		--binary "$(NP2_AUDIO86_ASYNC_TEST_OUTPUT)" \
		--output "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)" --timeout-seconds 30
	@cat "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_ASYNC_VALIDATOR)" \
		--log "$(NP2_AUDIO86_ASYNC_LOG_OUTPUT)" \
		--golden "$(NP2_AUDIO86_FIXTURE_GOLDEN)"
	@printf '%s\n' 'NP2audio86 asynchronous formal timeout proof completed.'

build-np2audio86-hardening:
	@mkdir -p "$(dir $(NP2_AUDIO86_HARDENING_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DNP2_AUDIO86_ASYNC_HOST -DNP2_AUDIO86_ASYNC_HARDENING_TEST \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_HARDENING_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_HARDENING_TEST_OUTPUT)"

test-np2audio86-hardening: test-np2audio86-runtime-transport build-np2audio86-hardening
	@$(PYTHON) "$(NP2_AUDIO86_HARDENING_RUNNER)" \
		--binary "$(NP2_AUDIO86_HARDENING_TEST_OUTPUT)" \
		--output "$(NP2_AUDIO86_HARDENING_LOG_OUTPUT)"
	@cat "$(NP2_AUDIO86_HARDENING_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_HARDENING_VALIDATOR)" \
		--log "$(NP2_AUDIO86_HARDENING_LOG_OUTPUT)"
	@printf '%s\n' 'NP2audio86 86H.4 hardening suite completed.'

build-np2audio86-hardening-asan:
	@mkdir -p "$(dir $(NP2_AUDIO86_HARDENING_ASAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-fsanitize=address -fno-omit-frame-pointer \
		-DNP2_AUDIO86_ASYNC_HOST -DNP2_AUDIO86_ASYNC_HARDENING_TEST \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_HARDENING_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_HARDENING_ASAN_OUTPUT)"

test-np2audio86-hardening-asan: build-np2audio86-hardening-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $(PYTHON) \
		"$(NP2_AUDIO86_HARDENING_RUNNER)" \
		--binary "$(NP2_AUDIO86_HARDENING_ASAN_OUTPUT)" \
		--output "$(NP2_AUDIO86_HARDENING_ASAN_LOG_OUTPUT)" \
		--short-timeout-seconds 15 --full-timeout-seconds 30
	@cat "$(NP2_AUDIO86_HARDENING_ASAN_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_HARDENING_VALIDATOR)" \
		--log "$(NP2_AUDIO86_HARDENING_ASAN_LOG_OUTPUT)"
	@printf '%s\n' 'ASAN=PASS'

build-np2audio86-hardening-ubsan:
	@mkdir -p "$(dir $(NP2_AUDIO86_HARDENING_UBSAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-fsanitize=undefined -fno-sanitize-recover=undefined \
		-DNP2_AUDIO86_ASYNC_HOST -DNP2_AUDIO86_ASYNC_HARDENING_TEST \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-ffunction-sections -fdata-sections \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(NP2_AUDIO86_FIXTURE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_CORE_HEADER_DIR)" \
		-I"$(NP2_AUDIO86_FIXTURE_GOLDEN_HEADER_DIR)" \
		-I"$(HOST_ROOT)/firmware/components/np2opngen_fixture/include" \
		"$(NP2_AUDIO86_FIXTURE_SOURCE)" $(NP2_AUDIO86_CORE_SOURCES) \
		"$(HOST_ROOT)/firmware/components/np2opngen_fixture/np2opngen_pcm_canonical.c" \
		"$(VENDOR_SRC)/sound/opngenc.c" "$(VENDOR_SRC)/sound/opngeng.c" \
		"$(VENDOR_SRC)/sound/psggenc.c" "$(VENDOR_SRC)/sound/psggeng.c" \
		"$(VENDOR_SRC)/sound/pcmmix.c" "$(VENDOR_SRC)/sound/pcm86g.c" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(NP2_AUDIO86_HARDENING_TEST_SOURCE)" \
		-Wl,--gc-sections -lm -o "$(NP2_AUDIO86_HARDENING_UBSAN_OUTPUT)"

test-np2audio86-hardening-ubsan: build-np2audio86-hardening-ubsan
	@"$(NP2_AUDIO86_HARDENING_UBSAN_OUTPUT)" --case R01 > "$(NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT)"
	@"$(NP2_AUDIO86_HARDENING_UBSAN_OUTPUT)" --case R02 >> "$(NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT)"
	@"$(NP2_AUDIO86_HARDENING_UBSAN_OUTPUT)" --case R03 >> "$(NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT)"
	@printf '%s\n' 'TRANSPORT_UBSAN=PASS' >> "$(NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT)"
	@cat "$(NP2_AUDIO86_HARDENING_UBSAN_LOG_OUTPUT)"
	@printf '%s\n' 'TRANSPORT_UBSAN=PASS'

test-np2audio86-hardening-soak: build-np2audio86-hardening
	@$(PYTHON) "$(NP2_AUDIO86_HARDENING_RUNNER)" \
		--binary "$(NP2_AUDIO86_HARDENING_TEST_OUTPUT)" \
		--output "$(NP2_AUDIO86_HARDENING_SOAK_LOG_OUTPUT)" --soak
	@cat "$(NP2_AUDIO86_HARDENING_SOAK_LOG_OUTPUT)"
	@$(PYTHON) "$(NP2_AUDIO86_HARDENING_VALIDATOR)" \
		--log "$(NP2_AUDIO86_HARDENING_SOAK_LOG_OUTPUT)" --soak
	@printf '%s\n' 'SOAK25=PASS'

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

build-opngen-retrofm-s98:
	@mkdir -p "$(dir $(RETROFM_S98_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_S98_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(RETROFM_S98_TEST_SOURCE)" \
		-lm -pthread -o "$(RETROFM_S98_TEST_OUTPUT)"

test-opngen-retrofm-s98: build-opngen-retrofm-s98
	@$(PYTHON) "$(RETROFM_S98_RUNNER)" \
		--binary "$(RETROFM_S98_TEST_OUTPUT)" \
		--fixture "$(RETROFM_S98_FIXTURE)" \
		--manifest "$(RETROFM_S98_MANIFEST)" --repetitions 3

test-opngen-retrofm-s98-validator:
	@$(PYTHON) "$(RETROFM_S98_PLAYBACK_VALIDATOR_TEST)"

test-opngen-retrofm-s98-formal: build-opngen-retrofm-s98 test-retrofm-provenance test-opngen-retrofm-s98-validator
	@$(PYTHON) "$(RETROFM_S98_RUNNER)" \
		--binary "$(RETROFM_S98_TEST_OUTPUT)" \
		--fixture "$(RETROFM_S98_FIXTURE)" \
		--manifest "$(RETROFM_S98_MANIFEST)" --repetitions 5
	@$(PYTHON) "$(RETROFM_S98_RUNNER)" \
		--binary "$(RETROFM_S98_TEST_OUTPUT)" \
		--fixture "$(RETROFM_S98_FIXTURE)" \
		--manifest "$(RETROFM_S98_MANIFEST)" \
		--descriptor "$(RETROFM_S98_DESCRIPTOR)" --repetitions 3

test-retrofm-provenance:
	@$(PYTHON) "$(HOST_ROOT)/tools/emu/validate_retrofm_pocket_provenance.py"
	@$(PYTHON) "$(RETROFM_S98_PROVENANCE_TEST)"

build-opngen-s98-s3:
	@mkdir -p "$(dir $(OPNGEN_S98_S3_TEST_OUTPUT))"
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

test-opngen-s98-s3: build-opngen-s98-s3
	@mkdir -p "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(PYTHON) "$(OPNGEN_S98_FIXTURE_GENERATOR)" \
		--output-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@$(PYTHON) "$(OPNGEN_S98_S3_RUNNER)" \
		--binary "$(OPNGEN_S98_S3_TEST_OUTPUT)" \
		--fixture-dir "$(OPNGEN_S98_FIXTURE_DIR)"
	@printf '%s\n' 'OPNGEN S98 S3 worker integration and repeatability passed.'

test-opngen-s98-s3-formal: build-opngen-s98-s3
	@$(PYTHON) "$(OPNGEN_S98_S3_FORMAL_RUNNER)" \
		--binary "$(OPNGEN_S98_S3_TEST_OUTPUT)" \
		--generator "$(OPNGEN_S98_FIXTURE_GENERATOR)" --repetitions 5

test-opngen-s98-s3-validator:
	@$(PYTHON) "$(OPNGEN_S98_S3_VALIDATOR_TEST)"

test-opngen-s98-s3-accepted: build-opngen-s98-s3 test-opngen-s98-s3-validator
	@$(PYTHON) "$(OPNGEN_S98_S3_FORMAL_RUNNER)" \
		--binary "$(OPNGEN_S98_S3_TEST_OUTPUT)" \
		--generator "$(OPNGEN_S98_FIXTURE_GENERATOR)" \
		--descriptor "$(OPNGEN_S98_S3_DESCRIPTOR)" --repetitions 1

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

build-opngen-e1b-observer:
	@mkdir -p "$(dir $(OPNGEN_E1B_OBSERVER_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(CURDIR)/compat" -I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_FIXTURE_SOURCE)" "$(OPNGEN_S98_SOURCE)" \
		"$(OPNGEN_SYNTHETIC_WORKLOAD_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_E1B_OBSERVER_TEST_SOURCE)" \
		-lm -pthread -o "$(OPNGEN_E1B_OBSERVER_TEST_OUTPUT)"

test-opngen-e1b-observer: build-opngen-e1b-observer
	@cd "$(CURDIR)" && "$(OPNGEN_E1B_OBSERVER_TEST_OUTPUT)"
	@printf '%s\n' 'OPNGEN E1B observer seam tests passed.'

build-opngen-pcm-ring:
	@mkdir -p "$(dir $(OPNGEN_PCM_RING_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_PCM_RING_TEST_SOURCE)" \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" -lm \
		-o "$(OPNGEN_PCM_RING_TEST_OUTPUT)"

test-opngen-pcm-ring: build-opngen-pcm-ring
	@"$(OPNGEN_PCM_RING_TEST_OUTPUT)"

build-opngen-pcm-ring-asan:
	@mkdir -p "$(dir $(OPNGEN_PCM_RING_ASAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_PCM_RING_TEST_SOURCE)" \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" -lm \
		-fsanitize=address,undefined -o "$(OPNGEN_PCM_RING_ASAN_OUTPUT)"

test-opngen-pcm-ring-asan: build-opngen-pcm-ring-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		"$(OPNGEN_PCM_RING_ASAN_OUTPUT)"

build-opngen-pcm-ring-ubsan:
	@mkdir -p "$(dir $(OPNGEN_PCM_RING_UBSAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread -O1 -g \
		-fsanitize=undefined -fno-omit-frame-pointer \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_PCM_RING_TEST_SOURCE)" \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" -lm \
		-fsanitize=undefined -o "$(OPNGEN_PCM_RING_UBSAN_OUTPUT)"

test-opngen-pcm-ring-ubsan: build-opngen-pcm-ring-ubsan
	@"$(OPNGEN_PCM_RING_UBSAN_OUTPUT)"

build-opngen-pcm-ring-tsan:
	@mkdir -p "$(dir $(OPNGEN_PCM_RING_TSAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -pthread -O1 -g \
		-fsanitize=thread -fno-omit-frame-pointer \
		-I"$(CURDIR)/compat" -I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(OPNGEN_SYNTH_EVENT_SOURCE)" \
		"$(OPNGEN_PCM_SOURCE)" "$(OPNGEN_SPSC_SOURCE)" \
		"$(OPNGEN_E1B_STREAM_SOURCE)" "$(VENDOR_SRC)/sound/opngenc.c" \
		"$(VENDOR_SRC)/sound/opngeng.c" "$(NP2_CRC32_SOURCE)" \
		"$(NP2_SHA256_SOURCE)" "$(OPNGEN_PCM_RING_TEST_SOURCE)" \
		-DDISABLE_SOUND -DBYTESEX_LITTLE -DOSLANG_UTF8 -DSUPPORT_UTF8 \
		-I"$(VENDOR_SRC)" -I"$(VENDOR_SRC)/i286c" -lm \
		-fsanitize=thread -o "$(OPNGEN_PCM_RING_TSAN_OUTPUT)"

test-opngen-pcm-ring-tsan: build-opngen-pcm-ring-tsan
	@"$(OPNGEN_PCM_RING_TSAN_OUTPUT)"

build-np2pcm-output:
	@mkdir -p "$(dir $(NP2_PCM_OUTPUT_TEST_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(NP2_PCM_OUTPUT_TEST_SOURCE)" -o "$(NP2_PCM_OUTPUT_TEST_OUTPUT)"

test-np2pcm-output: build-np2pcm-output
	@"$(NP2_PCM_OUTPUT_TEST_OUTPUT)"

test-np2pcm-output-stress: build-np2pcm-output
	@set -e; i=1; while [ $$i -le 20 ]; do \
		NP2_PCM_STRESS_ONLY=1 "$(NP2_PCM_OUTPUT_TEST_OUTPUT)" >/dev/null; \
		i=$$((i + 1)); \
	done; printf '%s\n' 'PCM_RING_SPSC_STRESS=20/20_PASS'

build-np2pcm-output-asan:
	@mkdir -p "$(dir $(NP2_PCM_OUTPUT_ASAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(NP2_PCM_OUTPUT_TEST_SOURCE)" -fsanitize=address,undefined \
		-o "$(NP2_PCM_OUTPUT_ASAN_OUTPUT)"

test-np2pcm-output-asan: build-np2pcm-output-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		"$(NP2_PCM_OUTPUT_ASAN_OUTPUT)"

build-np2pcm-output-ubsan:
	@mkdir -p "$(dir $(NP2_PCM_OUTPUT_UBSAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -O1 -g \
		-fsanitize=undefined -fno-omit-frame-pointer \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(NP2_PCM_OUTPUT_TEST_SOURCE)" -fsanitize=undefined \
		-o "$(NP2_PCM_OUTPUT_UBSAN_OUTPUT)"

test-np2pcm-output-ubsan: build-np2pcm-output-ubsan
	@"$(NP2_PCM_OUTPUT_UBSAN_OUTPUT)"

build-np2pcm-output-tsan:
	@mkdir -p "$(dir $(NP2_PCM_OUTPUT_TSAN_OUTPUT))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -O1 -g \
		-fsanitize=thread -fno-omit-frame-pointer \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(NP2_PCM_OUTPUT_TEST_SOURCE)" -fsanitize=thread \
		-o "$(NP2_PCM_OUTPUT_TSAN_OUTPUT)"

test-np2pcm-output-tsan: build-np2pcm-output-tsan
	@NP2_PCM_STRESS_ONLY=1 "$(NP2_PCM_OUTPUT_TSAN_OUTPUT)"

define build_p4_audio86_physical_sink
	@mkdir -p "$(dir $(1))"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread $(2) \
		-DP4_NANO_AUDIO86_PHYSICAL_SINK_TESTING=1 \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_PHYSICAL_SINK_HEADER_DIR)" \
		"$(OPNGEN_PCM_RING_SOURCE)" "$(NP2_PCM_OUTPUT_SOURCE)" \
		"$(P4_NANO_AUDIO86_PHYSICAL_SINK_SOURCE)" \
		"$(P4_NANO_AUDIO86_PHYSICAL_SINK_TEST_SOURCE)" $(3) -o "$(1)"
endef

p4_audio86_comma := ,

build-p4-audio86-physical-sink:
	$(call build_p4_audio86_physical_sink,$(P4_NANO_AUDIO86_PHYSICAL_SINK_TEST_OUTPUT),,)

test-p4-audio86-physical-sink: build-p4-audio86-physical-sink
	@set -e; evidence="$$(mktemp)"; trap 'rm -f "$$evidence"' EXIT; \
		"$(P4_NANO_AUDIO86_PHYSICAL_SINK_TEST_OUTPUT)" >"$$evidence"; \
		cat "$$evidence"; \
		$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_VALIDATOR)" "$$evidence"; \
		$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_MUTATIONS)"; \
		$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_MANIFEST)"; \
		$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_MANIFEST)" \
			--self-test-orphan-policy

test-p4-audio86-physical-s1:
	@mkdir -p "$(dir $(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_PHYSICAL_SINK_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_HEADER_DIR)" \
		"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_SOURCE)" \
		-o "$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT)"
	@"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT)"
	@$(PYTHON) "$(P4_NANO_CAPTURE_TEST)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S1_MUTATIONS)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S1_SOURCE)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S2A_SOURCE)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_IDF_SOURCE)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_IDF_SOURCE_TEST)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_PROJECT_INTERVAL)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_PROJECT_INTERVAL_TEST)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_SOURCE_PROVENANCE)"

test-p4-audio86-physical-s2: test-p4-audio86-physical-sink
	@$(PYTHON) "$(P4_NANO_CAPTURE_TEST)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S1_MUTATIONS)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S2_MUTATIONS)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_S2A_SOURCE)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_SOURCE_PROVENANCE)"

test-p4-audio86-physical-5d3-s1: test-p4-audio86-physical-sink
	@mkdir -p "$(dir $(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(OPNGEN_SYNTH_EVENT_HEADER_DIR)" -I"$(NP2_PCM_OUTPUT_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_PHYSICAL_SINK_HEADER_DIR)" \
		-I"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_HEADER_DIR)" \
		"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_SOURCE)" \
		-o "$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT)"
	@"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_TEST_OUTPUT)"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(P4_NANO_AUDIO86_TERMINAL_PREDICATE_HEADER_DIR)" \
		"$(P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_TEST_SOURCE)" \
		-o "$(P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_TEST_OUTPUT)"
	@"$(P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_TEST_OUTPUT)"
	@$(PYTHON) "$(P4_NANO_CAPTURE_TEST)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_5D3_S1_MUTATIONS)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_PHYSICAL_5D3_S1_SOURCE)"
	@$(PYTHON) "$(P4_NANO_AUDIO86_SOURCE_PROVENANCE)"

build-p4-audio86-physical-sink-asan:
	$(call build_p4_audio86_physical_sink,$(P4_NANO_AUDIO86_PHYSICAL_SINK_ASAN_OUTPUT),-O1 -g -fsanitize=address$(p4_audio86_comma)undefined -fno-omit-frame-pointer,-fsanitize=address$(p4_audio86_comma)undefined)

test-p4-audio86-physical-sink-asan: build-p4-audio86-physical-sink-asan
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "$(P4_NANO_AUDIO86_PHYSICAL_SINK_ASAN_OUTPUT)" >/dev/null

build-p4-audio86-physical-sink-ubsan:
	$(call build_p4_audio86_physical_sink,$(P4_NANO_AUDIO86_PHYSICAL_SINK_UBSAN_OUTPUT),-O1 -g -fsanitize=undefined -fno-omit-frame-pointer,-fsanitize=undefined)

test-p4-audio86-physical-sink-ubsan: build-p4-audio86-physical-sink-ubsan
	@"$(P4_NANO_AUDIO86_PHYSICAL_SINK_UBSAN_OUTPUT)" >/dev/null

build-p4-audio86-physical-sink-tsan:
	$(call build_p4_audio86_physical_sink,$(P4_NANO_AUDIO86_PHYSICAL_SINK_TSAN_OUTPUT),-O1 -g -fsanitize=thread -fno-omit-frame-pointer,-fsanitize=thread)

test-p4-audio86-physical-sink-tsan: build-p4-audio86-physical-sink-tsan
	@"$(P4_NANO_AUDIO86_PHYSICAL_SINK_TSAN_OUTPUT)" >/dev/null

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

test-p4-nano-board: test-p4-nano-board-static
	@mkdir -p "$(dir $(P4_NANO_BOARD_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I"$(P4_NANO_BOARD_HEADER_DIR)" "$(P4_NANO_BOARD_TEST_SOURCE)" \
		-o "$(P4_NANO_BOARD_TEST_OUTPUT)"
	@"$(P4_NANO_BOARD_TEST_OUTPUT)"

test-p4-nano-board-static:
	@$(PYTHON) "$(P4_NANO_BOARD_STATIC_TEST)"

test-p4-nano-audio-tone:
	@$(PYTHON) "$(P4_NANO_AUDIO_TONE_STATIC_TEST)"
	@mkdir -p "$(dir $(P4_NANO_AUDIO_TONE_TEST_OUTPUT))"
	@$(CXX) -std=c++20 -Wall -Wextra -Werror \
		-I"$(P4_NANO_AUDIO_TONE_HEADER_DIR)" -I"$(CURDIR)/compat" \
		"$(NP2_CRC32_SOURCE)" "$(NP2_SHA256_SOURCE)" \
		"$(P4_NANO_AUDIO_TONE_TEST_SOURCE)" \
		-o "$(P4_NANO_AUDIO_TONE_TEST_OUTPUT)"
	@"$(P4_NANO_AUDIO_TONE_TEST_OUTPUT)"

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
		-pthread \
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
