#include <cstdio>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "np2_memory_probe.h"
#include "uart_control_transport/uart_control_transport.h"

extern "C" void app_main(void)
{
    std::printf("ESP-NP2KAI HELLO WORLD OK\n");
    std::fflush(stdout);

    const esp_err_t memory_result = np2_memory_probe_run();
    if (memory_result != ESP_OK) {
        return;
    }

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
    }
    return;
}
