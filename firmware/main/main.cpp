#include <cstdio>
#include <cstddef>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#if defined(P4_NANO_DISPLAY_FOUNDATION_PROFILE)
#include "p4_nano_display/p4_nano_display.hpp"
#elif defined(P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE)
#include "p4_nano_display/p4_nano_display_transform_diagnostic.hpp"
#elif defined(P4_NANO_LIVE_DISPLAY_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
#include "p4_nano_live_display/p4_nano_live_display.hpp"
#elif defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
#include "p4_nano_pc98_runtime/p4_nano_pc98_runtime.hpp"
#else
#if defined(NP2_PRESENTATION_PROFILE)
#include "np2presentation_probe.h"
#elif defined(NP2_VIDEO_PROFILE)
#include "np2video_runner/np2video_runner.h"
#else
#if !defined(NP2_SD_NP2TEST_PROFILE)
#include "np2_fixture_probe.h"
#endif
#include "np2_memory_probe.h"
#if defined(NP2_DOSIO_PROBE)
#include "np2dosio_probe/np2dosio_probe.h"
#endif
#include "np2test_runner/np2test_runner.h"
#endif
#if !defined(NP2_PRESENTATION_PROFILE)
#include "uart_control_transport/uart_control_transport.h"
#endif
#if !defined(NP2_PRESENTATION_PROFILE)
#if !defined(NP2_VIDEO_PROFILE)
#if defined(UART_FATFS_PROFILE) && defined(UART_SDMMC_PROFILE)
#error "UART_FATFS_PROFILE and UART_SDMMC_PROFILE are mutually exclusive"
#endif
#if defined(NP2_VFS_FIXTURE_PROFILE) && \
    (defined(UART_FATFS_PROFILE) || defined(UART_SDMMC_PROFILE))
#error "NP2_VFS_FIXTURE_PROFILE and UART storage profiles are mutually exclusive"
#endif
#if defined(NP2_SD_NP2TEST_PROFILE) && \
    (defined(UART_FATFS_PROFILE) || defined(UART_SDMMC_PROFILE) || \
     defined(NP2_VFS_FIXTURE_PROFILE) || defined(NP2_VIDEO_PROFILE) || \
     defined(NP2_PRESENTATION_PROFILE) || defined(NP2_DOSIO_PROBE) || \
     defined(STORAGE_FATFS_PROBE) || defined(STORAGE_FATFS_CI_BOUNDED) || \
     defined(NP2_REDUCED_EXTMEM8))
#error "NP2_SD_NP2TEST_PROFILE is mutually exclusive with other profiles, probes, and reduced EXTMEM"
#endif
#if defined(UART_FATFS_PROFILE)
#include "file_transfer/file_transfer.hpp"
#include "storage_fatfs/storage_fatfs.hpp"
#include "uart_control_transport/uart_control_transport.hpp"
#endif
#if defined(UART_SDMMC_PROFILE)
#include "file_transfer/file_transfer.hpp"
#include "storage_fatfs/storage_fatfs.hpp"
#include "storage_sdmmc/storage_sdmmc.hpp"
#include "uart_control_transport/uart_control_transport.hpp"
#endif
#if defined(NP2_SD_NP2TEST_PROFILE)
#include "storage_fatfs/storage_fatfs.hpp"
#include "storage_sdmmc/storage_sdmmc.hpp"
#endif
#if defined(NP2_VFS_FIXTURE_PROFILE)
#include "storage_fatfs/storage_fatfs.hpp"
#endif
#if defined(STORAGE_FATFS_PROBE)
#include "storage_fatfs_probe/storage_fatfs_probe.h"
#endif
#endif
#endif

#endif

#if !defined(P4_NANO_DISPLAY_FOUNDATION_PROFILE) && \
    !defined(P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE) && \
    !defined(P4_NANO_REAL_RUNTIME_PROFILE) && \
    !defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) && \
    !defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) && \
    !defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
namespace {

#if !defined(NP2_PRESENTATION_PROFILE)
bool write_np2_runner_output(void *, const char *data, std::size_t length)
{
    return uart_control_transport_write(data, length);
}
#endif

#if !defined(NP2_PRESENTATION_PROFILE) && \
    (defined(UART_FATFS_PROFILE) || defined(NP2_VFS_FIXTURE_PROFILE))
storage_fatfs::MountProvider s_fatfs_provider;
storage_fatfs::StorageFatfs s_fatfs_storage(s_fatfs_provider);
#endif
#if defined(UART_SDMMC_PROFILE)
storage_sdmmc::SdmmcMountProvider s_sd_provider;
storage_fatfs::StorageFatfs s_sd_storage(
    s_sd_provider, storage_sdmmc::kSdmmcRootConfig);
#endif
#if defined(NP2_SD_NP2TEST_PROFILE)
constexpr char kSdNp2TestFixturePath[] =
    "/sdcard/files/np2-fixtures/"
    "np2test-a2-20260821-r1-"
    "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm";
storage_sdmmc::SdmmcMountProvider s_sd_np2test_provider;
storage_fatfs::StorageFatfs s_sd_np2test_storage(
    s_sd_np2test_provider, storage_sdmmc::kSdmmcRootConfig);
#endif

} // namespace
#endif

extern "C" void app_main(void)
{
    std::printf("ESP-NP2KAI HELLO WORLD OK\n");
    std::fflush(stdout);

#if defined(P4_NANO_DISPLAY_FOUNDATION_PROFILE)
    const esp_err_t foundation_result = p4_nano_display::run_foundation();
    std::printf("P4_NANO_DISPLAY_FOUNDATION_RESULT=%s\n",
                foundation_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE)
#if defined(P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_ROTATION_CW)
    constexpr auto transform_rotation =
        p4_nano_display::QuarterTurn::Clockwise;
#else
    constexpr auto transform_rotation =
        p4_nano_display::QuarterTurn::CounterClockwise;
#endif
    const esp_err_t transform_result =
        p4_nano_display::run_transform_diagnostic(transform_rotation);
    std::printf("P4_NANO_TRANSFORM_DIAGNOSTIC_RESULT=%s\n",
                transform_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_LIVE_DISPLAY_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE) || \
    defined(P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE) || \
    defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)
    const esp_err_t live_result = p4_nano_live_display::run();
    std::printf("P4_NANO_LIVE_DISPLAY_RESULT=%s\n",
                live_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    const esp_err_t keyboard_result =
        p4_nano_pc98_runtime::run_usb_keyboard_validation();
    std::printf("P4_NANO_USB_KEYBOARD_VALIDATION_EXIT=%s\n",
                keyboard_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_REAL_RUNTIME_PROFILE)
    const esp_err_t runtime_result = p4_nano_pc98_runtime::run_production();
    std::printf("P4_NANO_RUNTIME_PRODUCTION_RESULT=%s\n",
                runtime_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    const esp_err_t runtime_result = p4_nano_pc98_runtime::run_validation();
    std::printf("P4_NANO_RUNTIME_VALIDATION_EXIT=%s\n",
                runtime_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    const esp_err_t keyboard_result =
        p4_nano_pc98_runtime::run_keyboard_validation();
    std::printf("P4_NANO_KEYBOARD_VALIDATION_EXIT=%s\n",
                keyboard_result == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    return;
#elif defined(NP2_PRESENTATION_PROFILE)
    (void)np2presentation_probe_run();
    return;
#elif defined(NP2_VIDEO_PROFILE)
    const esp_app_desc_t *video_app = esp_app_get_description();
    const uart_control_metadata_t video_metadata{
        video_app->project_name,
        video_app->version,
        esp_get_idf_version(),
        "esp32p4",
    };
    const esp_err_t video_start_result =
        uart_control_transport_start(&video_metadata);
    if (video_start_result != ESP_OK) {
        std::printf("NP2VIDEO_GOLDEN_RESULT=HARNESS_ERROR reason=uart_start_%s\n",
                    esp_err_to_name(video_start_result));
        std::fflush(stdout);
        return;
    }
    const esp_err_t video_runner_result =
        np2video_runner_start(write_np2_runner_output, nullptr);
    if (video_runner_result != ESP_OK) {
        std::printf("NP2VIDEO_GOLDEN_RESULT=HARNESS_ERROR reason=task_start_%s\n",
                    esp_err_to_name(video_runner_result));
        std::fflush(stdout);
    }
    return;
#else
#if defined(NP2_DOSIO_PROBE)
    if (np2dosio_probe_run() != ESP_OK) {
        return;
    }
    return;
#endif

#if defined(STORAGE_FATFS_PROBE)
    if (storage_fatfs_probe_run() != ESP_OK) {
        return;
    }
#endif

#if defined(NP2_VFS_FIXTURE_PROFILE)
    if (s_fatfs_storage.mount() != ESP_OK) {
        std::printf("NP2REDUCED_RESULT=HARNESS_ERROR reason=storage_mount\n");
        std::fflush(stdout);
        return;
    }
    std::printf("NP2REDUCED_VFS_MOUNT path=%s partition=%s\n",
                storage_fatfs::kMountPath, storage_fatfs::kPartitionLabel);
    std::fflush(stdout);
    const esp_err_t fixture_result = ESP_OK;
    const esp_err_t memory_result = ESP_OK;
#elif defined(NP2_SD_NP2TEST_PROFILE)
    const esp_err_t storage_mount_result = s_sd_np2test_storage.mount();
    if (storage_mount_result != ESP_OK) {
        std::printf("NP2TEST_SD_MOUNT=FAIL reason=%s\n",
                    esp_err_to_name(storage_mount_result));
        std::printf("NP2TEST_RESULT=HARNESS_ERROR reason=sd_mount\n");
        std::fflush(stdout);
        return;
    }
    std::printf("NP2TEST_SD_MOUNTED path=%s fixture=%s\n",
                storage_sdmmc::kMountPath, kSdNp2TestFixturePath);
    std::fflush(stdout);
    const esp_err_t fixture_result = ESP_OK;
    const esp_err_t memory_result = np2_memory_probe_run();
#else
    const esp_err_t fixture_result = np2_fixture_probe_run();
    if (fixture_result != ESP_OK) {
        std::fflush(stdout);
    }

    const esp_err_t memory_result = np2_memory_probe_run();
#endif

    const esp_app_desc_t *app = esp_app_get_description();
    const uart_control_metadata_t metadata{
        app->project_name,
        app->version,
        esp_get_idf_version(),
        "esp32p4",
    };
#if defined(UART_FATFS_PROFILE)
    storage::Storage file_storage{};
    file_transfer::Limits file_limits{};
    if (s_fatfs_storage.mount() != ESP_OK) {
        std::printf("ESP-NP2KAI UART FATFS MOUNT FAILED\n");
        std::fflush(stdout);
        return;
    }
    std::printf("ESP-NP2KAI UART FATFS MOUNTED path=%s partition=%s\n",
                storage_fatfs::kMountPath, storage_fatfs::kPartitionLabel);
    std::fflush(stdout);
    file_storage = s_fatfs_storage.api();
    file_limits.max_file_bytes = 2 * 1024 * 1024;
    const esp_err_t start_result =
        uart_control_transport::start_with_storage(&metadata, file_storage, file_limits);
#elif defined(UART_SDMMC_PROFILE)
    storage::Storage file_storage{};
    file_transfer::Limits file_limits{};
    const esp_err_t storage_mount_result = s_sd_storage.mount();
    if (storage_mount_result == ESP_OK) {
        std::printf("ESP-NP2KAI UART SDMMC STORAGE MOUNTED path=%s\n",
                    storage_sdmmc::kFileTransferRoot);
    } else {
        std::printf("ESP-NP2KAI UART SDMMC STORAGE UNAVAILABLE; CONTROL CONTINUING\n");
    }
    std::fflush(stdout);
    file_storage = s_sd_storage.api();
    file_limits.max_file_bytes = 2 * 1024 * 1024;
    const esp_err_t start_result =
        uart_control_transport::start_with_storage(&metadata, file_storage, file_limits);
#elif defined(NP2_SD_NP2TEST_PROFILE)
    // The formal SD image exposes only the control plane. The SD-backed
    // Storage API is intentionally not registered with File Transfer.
    const esp_err_t start_result = uart_control_transport_start(&metadata);
#else
    const esp_err_t start_result = uart_control_transport_start(&metadata);
#endif
    if (start_result != ESP_OK) {
        std::printf("ESP-NP2KAI UART CONTROL START FAILED: %s\n",
                    esp_err_to_name(start_result));
        std::fflush(stdout);
#if defined(UART_FATFS_PROFILE) || defined(NP2_VFS_FIXTURE_PROFILE)
        s_fatfs_storage.unmount();
#endif
#if defined(UART_SDMMC_PROFILE)
        if (s_sd_storage.mounted()) s_sd_storage.unmount();
#endif
#if defined(NP2_SD_NP2TEST_PROFILE)
        if (s_sd_np2test_storage.mounted()) s_sd_np2test_storage.unmount();
#endif
        return;
    }

    np2test_runner_config runner_config{
#if defined(NP2_REDUCED_EXTMEM8)
        NP2TEST_PROFILE_REDUCED_EXTMEM8,
#else
        NP2TEST_PROFILE_FORMAL,
#endif
#if defined(NP2_VFS_FIXTURE_PROFILE)
        NP2TEST_DISK_SOURCE_VFS_FILE,
        storage_fatfs::kFixturePath,
#elif defined(NP2_SD_NP2TEST_PROFILE)
        NP2TEST_DISK_SOURCE_VFS_FILE,
        kSdNp2TestFixturePath,
#else
        NP2TEST_DISK_SOURCE_RAW_FIXTURE,
        nullptr,
#endif
        write_np2_runner_output,
        nullptr,
    };
    const bool fixture_ready = fixture_result == ESP_OK;
#if defined(NP2_REDUCED_EXTMEM8)
    (void)memory_result;
    if (fixture_ready) {
        const esp_err_t runner_result = np2test_runner_start(&runner_config);
        if (runner_result != ESP_OK) {
            std::printf("NP2REDUCED_RESULT=HARNESS_ERROR reason=task_start_%s\n",
                        esp_err_to_name(runner_result));
            std::fflush(stdout);
        }
    } else {
        std::printf("NP2REDUCED_RESULT=NOT_REACHED reason=fixture_unavailable\n");
        std::fflush(stdout);
    }
#else
    const bool formal_memory_ready = memory_result == ESP_OK;
    if (fixture_ready && formal_memory_ready) {
        const esp_err_t runner_result = np2test_runner_start(&runner_config);
        if (runner_result != ESP_OK) {
            std::printf("NP2TEST_RESULT=HARNESS_ERROR reason=task_start_%s\n",
                        esp_err_to_name(runner_result));
            std::fflush(stdout);
        }
    } else {
        std::printf("NP2TEST_RUNNER_BLOCKED profile=formal formal_extmem=13 "
                    "reason=%s\n",
                    !fixture_ready ? "fixture_unavailable" :
                                      "formal_extmem_preflight");
        std::fflush(stdout);
    }
#endif
    return;
#endif
}
