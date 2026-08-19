#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4_nano_flash_psram";

static const size_t EXPECTED_FLASH_SIZE = 16U * 1024U * 1024U;
static const size_t PSRAM_SAFETY_MARGIN_BYTES = 256U * 1024U;
static const size_t WORDS_PER_YIELD = 16U * 1024U;
static const size_t WORDS_PER_PROGRESS_LOG = 256U * 1024U;

typedef uint32_t (*pattern_fn_t)(size_t index);

typedef struct {
    bool pass;
    size_t mismatch_count;
    size_t first_mismatch_index;
    uint32_t first_expected;
    uint32_t first_actual;
} pattern_result_t;

static uint32_t pattern_one(size_t index)
{
    uint32_t value = (uint32_t)index;
    value ^= (uint32_t)(index >> 16);
    value *= UINT32_C(0x9E3779B9);
    return value ^ UINT32_C(0xA5A5A5A5);
}

static uint32_t pattern_two(size_t index)
{
    uint32_t value = (uint32_t)index ^ UINT32_C(0xC3C3C3C3);
    value = (value ^ (value >> 16)) * UINT32_C(0x85EBCA6B);
    value = (value ^ (value >> 13)) * UINT32_C(0xC2B2AE35);
    return value ^ (value >> 16) ^ UINT32_C(0x5A5A5A5A);
}

static void service_test_loop(size_t index, size_t words, const char *phase)
{
    if ((index % WORDS_PER_PROGRESS_LOG) == 0U) {
        ESP_LOGI(TAG, "%s progress: %zu/%zu words", phase, index, words);
    }
    if ((index % WORDS_PER_YIELD) == 0U) {
        vTaskDelay(1);
    }
}

static void write_pattern(volatile uint32_t *data, size_t words,
                          pattern_fn_t pattern, const char *phase)
{
    for (size_t index = 0; index < words; ++index) {
        data[index] = pattern(index);
        service_test_loop(index, words, phase);
    }
}

static pattern_result_t verify_pattern(const volatile uint32_t *data, size_t words,
                                       pattern_fn_t pattern, const char *phase)
{
    pattern_result_t result = {
        .pass = true,
        .mismatch_count = 0U,
        .first_mismatch_index = 0U,
        .first_expected = 0U,
        .first_actual = 0U,
    };

    for (size_t index = 0; index < words; ++index) {
        const uint32_t expected = pattern(index);
        const uint32_t actual = data[index];
        if (actual != expected) {
            if (result.pass) {
                result.first_mismatch_index = index;
                result.first_expected = expected;
                result.first_actual = actual;
            }
            result.pass = false;
            ++result.mismatch_count;
        }
        service_test_loop(index, words, phase);
    }

    if (!result.pass) {
        ESP_LOGE(TAG,
                 "%s mismatch: address=0x%" PRIxPTR " index=%zu expected=0x%08" PRIx32
                 " actual=0x%08" PRIx32 " mismatches=%zu",
                 phase,
                 (uintptr_t)&data[result.first_mismatch_index],
                 result.first_mismatch_index,
                 result.first_expected,
                 result.first_actual,
                 result.mismatch_count);
    }
    return result;
}

static bool check_flash(void)
{
    uint32_t observed_size = 0U;
    const esp_err_t err = esp_flash_get_size(NULL, &observed_size);

    ESP_LOGI(TAG, "configured flash size=%s, observed flash size=%" PRIu32 " bytes",
             CONFIG_ESPTOOLPY_FLASHSIZE, observed_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash size read failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "P4-NANO FLASH CHECK: FAIL");
        return false;
    }
    if (observed_size != EXPECTED_FLASH_SIZE) {
        ESP_LOGE(TAG, "unexpected flash size: expected=%zu observed=%" PRIu32,
                 EXPECTED_FLASH_SIZE, observed_size);
        ESP_LOGE(TAG, "P4-NANO FLASH CHECK: FAIL");
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO FLASH CHECK: PASS");
    return true;
}

static bool check_psram_init(size_t *detected_size)
{
#if CONFIG_SPIRAM_MEMTEST
    ESP_LOGI(TAG, "built-in PSRAM memory test: enabled; boot completed after startup test");
#else
    ESP_LOGE(TAG, "built-in PSRAM memory test: disabled");
#endif

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized by ESP-IDF startup");
        ESP_LOGE(TAG, "P4-NANO PSRAM INIT: FAIL");
        return false;
    }

    *detected_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM detected size=%zu bytes (official esp_psram_get_size API)",
             *detected_size);
    if (*detected_size == 0U) {
        ESP_LOGE(TAG, "PSRAM reported zero available bytes");
        ESP_LOGE(TAG, "P4-NANO PSRAM INIT: FAIL");
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO PSRAM INIT: PASS");
    return true;
}

static bool run_psram_data_test(size_t detected_size)
{
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM;
    const size_t total_bytes = heap_caps_get_total_size(psram_caps);
    const size_t free_bytes = heap_caps_get_free_size(psram_caps);
    const size_t largest_free_bytes = heap_caps_get_largest_free_block(psram_caps);

    ESP_LOGI(TAG,
             "PSRAM heap: detected=%zu total=%zu free=%zu largest_free=%zu bytes",
             detected_size, total_bytes, free_bytes, largest_free_bytes);

    if (total_bytes == 0U || largest_free_bytes <= PSRAM_SAFETY_MARGIN_BYTES) {
        ESP_LOGE(TAG, "PSRAM heap has no sufficiently large contiguous block");
        ESP_LOGE(TAG, "P4-NANO PSRAM DATA TEST: FAIL");
        return false;
    }

    const size_t requested_bytes =
        (largest_free_bytes - PSRAM_SAFETY_MARGIN_BYTES) & ~(sizeof(uint32_t) - 1U);
    const size_t words = requested_bytes / sizeof(uint32_t);
    void *allocation = heap_caps_malloc(requested_bytes, psram_caps);
    if (allocation == NULL) {
        ESP_LOGE(TAG, "PSRAM allocation failed for %zu bytes", requested_bytes);
        ESP_LOGE(TAG, "P4-NANO PSRAM DATA TEST: FAIL");
        return false;
    }

    volatile uint32_t *data = (volatile uint32_t *)allocation;
    ESP_LOGI(TAG,
             "PSRAM data test allocation: address=%p tested_bytes=%zu words=%zu safety_margin=%zu",
             allocation, requested_bytes, words, PSRAM_SAFETY_MARGIN_BYTES);

    write_pattern(data, words, pattern_one, "pattern-1 write");
    const pattern_result_t pattern_one_result =
        verify_pattern(data, words, pattern_one, "pattern-1 verify");

    write_pattern(data, words, pattern_two, "pattern-2 write");
    const pattern_result_t pattern_two_result =
        verify_pattern(data, words, pattern_two, "pattern-2 verify");

    const uintptr_t freed_address = (uintptr_t)allocation;
    heap_caps_free(allocation);

    /* The IDF address-scoped check avoids the full generic PSRAM scan, which
     * carries an interrupt-WDT timeout warning in the ESP-IDF API docs. */
    const bool heap_integrity_after_free =
        heap_caps_check_integrity_addr((intptr_t)freed_address, false);
    const size_t free_after_bytes = heap_caps_get_free_size(psram_caps);
    const size_t largest_after_bytes = heap_caps_get_largest_free_block(psram_caps);
    ESP_LOGI(TAG,
             "PSRAM heap after free: integrity=%s free=%zu largest_free=%zu bytes",
             heap_integrity_after_free ? "PASS" : "FAIL",
             free_after_bytes,
             largest_after_bytes);

    const bool data_test_passed = pattern_one_result.pass && pattern_two_result.pass &&
                                  heap_integrity_after_free;
    if (!data_test_passed) {
        ESP_LOGE(TAG, "P4-NANO PSRAM DATA TEST: FAIL");
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO PSRAM DATA TEST: PASS (tested_bytes=%zu)", requested_bytes);
    return true;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "P4-NANO FLASH-PSRAM TEST START");
    ESP_LOGI(TAG, "target=ESP32-P4 revision=v%d.%d",
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG,
             "PSRAM config: mode=HEX speed=%dMHz caps_alloc=%d memtest=%d",
             CONFIG_SPIRAM_SPEED,
             CONFIG_SPIRAM_USE_CAPS_ALLOC,
             CONFIG_SPIRAM_MEMTEST);

    const bool flash_ok = check_flash();
    size_t detected_psram_size = 0U;
    const bool psram_init_ok = check_psram_init(&detected_psram_size);
    const bool psram_data_ok = psram_init_ok && run_psram_data_test(detected_psram_size);
    const bool overall_ok = flash_ok && psram_init_ok && psram_data_ok;

    if (overall_ok) {
        ESP_LOGI(TAG, "P4-NANO FLASH-PSRAM RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO FLASH-PSRAM RESULT: FAIL");
    }

    while (true) {
        ESP_LOGI(TAG, "alive: FLASH-PSRAM diagnostic complete (%s)",
                 overall_ok ? "PASS" : "FAIL");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
