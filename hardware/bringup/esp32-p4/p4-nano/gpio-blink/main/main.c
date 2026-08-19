#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4_nano_gpio_blink";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "P4-NANO GPIO BLINK START");
    ESP_LOGI(TAG, "target=ESP32-P4 revision=v%d.%d",
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "external LED output=GPIO20");

    /* GPIO20 is the fixed external LED output for this hardware diagnostic. */
    const gpio_config_t gpio20_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_20,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio20_config));

    int led_level = 0;
    unsigned int complete_cycles = 0;

    while (true) {
        led_level = !led_level;
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_20, led_level));

        vTaskDelay(pdMS_TO_TICKS(500));

        if (led_level == 0) {
            ++complete_cycles;
            if ((complete_cycles % 10U) == 0U) {
                ESP_LOGI(TAG, "alive: %u blink cycles", complete_cycles);
            }
        }
    }
}
