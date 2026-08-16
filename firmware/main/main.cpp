#include <cstdio>
#include <cstddef>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "np2_fixture_probe.h"
#include "np2_memory_probe.h"
#include "np2test_runner/np2test_runner.h"
#include "uart_control_transport/uart_control_transport.h"
#if defined(STORAGE_FATFS_PROBE)
#include "storage_fatfs_probe/storage_fatfs_probe.h"
#endif

namespace {

bool write_np2_runner_output(void *, const char *data, std::size_t length)
{
    return uart_control_transport_write(data, length);
}

} // namespace

extern "C" void app_main(void)
{
    std::printf("ESP-NP2KAI HELLO WORLD OK\n");
    std::fflush(stdout);

#if defined(STORAGE_FATFS_PROBE)
    if (storage_fatfs_probe_run() != ESP_OK) {
        return;
    }
#endif

    const esp_err_t fixture_result = np2_fixture_probe_run();
    if (fixture_result != ESP_OK) {
        std::fflush(stdout);
    }

    const esp_err_t memory_result = np2_memory_probe_run();

    const esp_app_desc_t *app = esp_app_get_description();
    const uart_control_metadata_t metadata{
        app->project_name,
        app->version,
        esp_get_idf_version(),
        "esp32p4",
    };
    const esp_err_t start_result = uart_control_transport_start(&metadata);
    if (start_result != ESP_OK) {
        std::printf("ESP-NP2KAI UART CONTROL START FAILED: %s\n",
                    esp_err_to_name(start_result));
        std::fflush(stdout);
        return;
    }

    np2test_runner_config runner_config{
#if defined(NP2_REDUCED_EXTMEM8)
        NP2TEST_PROFILE_REDUCED_EXTMEM8,
#else
        NP2TEST_PROFILE_FORMAL,
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
}
