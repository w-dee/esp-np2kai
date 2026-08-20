#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diskio_sdmmc.h"
#include "sdkconfig.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#define MOUNT_POINT "/sdcard"
#define FAT_DRIVE "0:"
#define README_PATH MOUNT_POINT "/README.TXT"
#define SCRATCH_PATH MOUNT_POINT "/P4SDTEST.BIN"
#define SCRATCH_BYTES 4096U
#define SD_LDO_CHANNEL 4
#define COMMAND_LINE_BUFFER_SIZE 32U
#define COMMAND_RX_BUFFER_SIZE 256
#define COMMAND_RX_TIMEOUT_MS 100
#define WRITE_COMMAND "WRITE_TEST"
#define SAFE_OFF_LED_GPIO GPIO_NUM_20
#define SAFE_OFF_LED_ON_MS 250
#define SAFE_OFF_LED_OFF_MS 250
#define SAFE_OFF_LED_TASK_STACK 2048
#define SAFE_OFF_LED_TASK_PRIORITY 5
#define SAFE_OFF_LED_TRANSITION_LOG_LIMIT 6U

static const char *TAG = "p4_nano_sdmmc";
static const uint8_t EXPECTED_README[] = "ESP32-P4 SD TEST CARD\n";

static sdmmc_host_t s_host;
static sdmmc_slot_config_t s_slot_config;
static sdmmc_card_t s_card;
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static FATFS *s_fs;
static uint8_t s_scratch_payload[SCRATCH_BYTES];
static uint8_t s_scratch_readback[SCRATCH_BYTES];
static volatile bool s_safe_off_led_enabled;
static bool s_safe_off_led_ready;
static TaskHandle_t s_safe_off_led_task_handle;
static uint32_t s_safe_off_led_transition_logs;
static bool s_safe_off_led_last_level;
static bool s_safe_off_led_last_level_valid;
static bool s_safe_off_led_gpio_error_logged;
static portMUX_TYPE s_safe_off_led_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    char line[COMMAND_LINE_BUFFER_SIZE];
    size_t length;
    bool invalid;
    bool overflow;
    bool pending_cr;
} command_parser_t;

static esp_err_t safe_off_led_drive_locked(int level, bool log_transition,
                                            bool *log_error, bool *log_level)
{
    const esp_err_t err = gpio_set_level(SAFE_OFF_LED_GPIO, level);
    if (err != ESP_OK) {
        if (!s_safe_off_led_gpio_error_logged) {
            s_safe_off_led_gpio_error_logged = true;
            *log_error = true;
        }
    } else {
        const bool changed = !s_safe_off_led_last_level_valid ||
                             s_safe_off_led_last_level != (level != 0);
        s_safe_off_led_last_level = level != 0;
        s_safe_off_led_last_level_valid = true;
        if (log_transition && changed &&
            s_safe_off_led_transition_logs < SAFE_OFF_LED_TRANSITION_LOG_LIMIT) {
            s_safe_off_led_transition_logs++;
            *log_level = true;
        }
    }
    return err;
}

static void safe_off_led_report_drive(int level, esp_err_t err,
                                      bool log_error, bool log_level)
{
    if (log_error) {
        ESP_LOGE(TAG, "P4-NANO SAFE-OFF LED gpio_set_level(%d): FAIL (%s, 0x%x)",
                 level, esp_err_to_name(err), err);
    }
    if (log_level) {
        ESP_LOGI(TAG, "P4-NANO SAFE-OFF LED GPIO20=%d", level != 0 ? 1 : 0);
    }
}

static esp_err_t safe_off_led_drive(int level, bool log_transition)
{
    bool log_error = false;
    bool log_level = false;

    portENTER_CRITICAL(&s_safe_off_led_lock);
    const esp_err_t err = safe_off_led_drive_locked(level, log_transition,
                                                     &log_error, &log_level);
    portEXIT_CRITICAL(&s_safe_off_led_lock);

    safe_off_led_report_drive(level, err, log_error, log_level);
    return err;
}

static esp_err_t safe_off_led_drive_high_if_enabled(void)
{
    bool log_error = false;
    bool log_level = false;

    portENTER_CRITICAL(&s_safe_off_led_lock);
    if (!s_safe_off_led_enabled) {
        portEXIT_CRITICAL(&s_safe_off_led_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = safe_off_led_drive_locked(1, true, &log_error, &log_level);
    portEXIT_CRITICAL(&s_safe_off_led_lock);

    safe_off_led_report_drive(1, err, log_error, log_level);
    return err;
}

static void safe_off_led_task(void *arg)
{
    (void)arg;

    while (true) {
        if (safe_off_led_drive_high_if_enabled() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SAFE_OFF_LED_OFF_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SAFE_OFF_LED_ON_MS));
        (void)safe_off_led_drive(0, true);
        vTaskDelay(pdMS_TO_TICKS(SAFE_OFF_LED_OFF_MS));
    }
}

static esp_err_t safe_off_led_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SAFE_OFF_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    s_safe_off_led_enabled = false;
    const esp_err_t initial_level_err = safe_off_led_drive(0, false);
    if (initial_level_err != ESP_OK) {
        return initial_level_err;
    }
    if (xTaskCreate(safe_off_led_task, "safe_off_led", SAFE_OFF_LED_TASK_STACK,
                    NULL, SAFE_OFF_LED_TASK_PRIORITY, &s_safe_off_led_task_handle) != pdPASS ||
        s_safe_off_led_task_handle == NULL) {
        (void)safe_off_led_drive(0, false);
        return ESP_ERR_NO_MEM;
    }

    s_safe_off_led_ready = true;
    return ESP_OK;
}

static bool safe_off_led_set(bool enabled)
{
    if (!s_safe_off_led_ready) {
        return false;
    }

    bool log_error = false;
    bool log_level = false;
    portENTER_CRITICAL(&s_safe_off_led_lock);
    s_safe_off_led_enabled = enabled;
    const esp_err_t err = safe_off_led_drive_locked(0, false,
                                                     &log_error, &log_level);
    portEXIT_CRITICAL(&s_safe_off_led_lock);
    safe_off_led_report_drive(0, err, log_error, log_level);
    if (enabled) {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "P4-NANO SAFE-OFF LED ENABLED");
        }
    } else {
        ESP_LOGI(TAG, "P4-NANO SAFE-OFF LED DISABLED");
    }
    return err == ESP_OK;
}

static void report_esp_marker(const char *marker, esp_err_t err)
{
    ESP_LOGI(TAG, "%s: %s", marker, err == ESP_OK ? "PASS" : "FAIL");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s (0x%x)", marker, esp_err_to_name(err), err);
    }
}

static void report_write_failure(const char *marker, const char *reason)
{
    ESP_LOGE(TAG, "%s: FAIL (%s)", marker, reason);
}

static const char *fatfs_result_name(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return "FR_OK";
    case FR_DISK_ERR:
        return "FR_DISK_ERR";
    case FR_INT_ERR:
        return "FR_INT_ERR";
    case FR_NOT_READY:
        return "FR_NOT_READY";
    case FR_NO_FILE:
        return "FR_NO_FILE";
    case FR_NO_PATH:
        return "FR_NO_PATH";
    case FR_INVALID_NAME:
        return "FR_INVALID_NAME";
    case FR_DENIED:
        return "FR_DENIED";
    case FR_EXIST:
        return "FR_EXIST";
    case FR_INVALID_OBJECT:
        return "FR_INVALID_OBJECT";
    case FR_WRITE_PROTECTED:
        return "FR_WRITE_PROTECTED";
    case FR_INVALID_DRIVE:
        return "FR_INVALID_DRIVE";
    case FR_NOT_ENABLED:
        return "FR_NOT_ENABLED";
    case FR_NO_FILESYSTEM:
        return "FR_NO_FILESYSTEM";
    case FR_MKFS_ABORTED:
        return "FR_MKFS_ABORTED";
    case FR_TIMEOUT:
        return "FR_TIMEOUT";
    case FR_LOCKED:
        return "FR_LOCKED";
    case FR_NOT_ENOUGH_CORE:
        return "FR_NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES:
        return "FR_TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER:
        return "FR_INVALID_PARAMETER";
    default:
        return "FR_UNKNOWN";
    }
}

static esp_err_t configure_sd_power(void)
{
    const sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_LDO_CHANNEL,
    };

    /* The NANO schematic routes ESP_LDO_VO4 to the SD power network. */
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "SD power: ESP_LDO_VO4 / on-chip LDO channel %d; GPIO45 left untouched (board default)",
                 SD_LDO_CHANNEL);
    }
    return err;
}

static esp_err_t initialize_sd_host(void)
{
    s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    s_host.slot = SDMMC_HOST_SLOT_1;
    s_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    s_host.pwr_ctrl_handle = s_pwr_ctrl;

    s_slot_config = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    s_slot_config.width = 4;
    /* GPIO45 is part of the board power-control path, not this 4-bit bus. */
    s_slot_config.d4 = GPIO_NUM_NC;
    s_slot_config.d5 = GPIO_NUM_NC;
    s_slot_config.d6 = GPIO_NUM_NC;
    s_slot_config.d7 = GPIO_NUM_NC;
    s_slot_config.cd = SDMMC_SLOT_NO_CD;
    s_slot_config.wp = SDMMC_SLOT_NO_WP;
    s_slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG,
             "SD bus: slot=%d width=%d frequency=%d kHz pins CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             s_host.slot, s_slot_config.width, s_host.max_freq_khz,
             s_slot_config.clk, s_slot_config.cmd, s_slot_config.d0,
             s_slot_config.d1, s_slot_config.d2, s_slot_config.d3);

    esp_err_t err = s_host.init();
    if (err != ESP_OK) {
        return err;
    }
    return sdmmc_host_init_slot(s_host.slot, &s_slot_config);
}

static esp_err_t initialize_sd_card(void)
{
    memset(&s_card, 0, sizeof(s_card));
    return sdmmc_card_init(&s_host, &s_card);
}

static esp_err_t report_sd_card_info(void)
{
    esp_err_t err = sdmmc_get_status(&s_card);
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_card_print_info(stdout, &s_card);
    const uint64_t capacity_bytes =
        (uint64_t)s_card.csd.capacity * (uint64_t)s_card.csd.sector_size;
    const size_t bus_width = s_host.get_bus_width(s_host.slot);
    ESP_LOGI(TAG,
             "SD card: type=%s capacity=%" PRIu64 " bytes sector=%d bus_width=%u real_freq=%d kHz",
             s_card.is_mmc ? "MMC" : (s_card.is_sdio ? "SDIO" : "SD"),
             capacity_bytes, s_card.csd.sector_size, (unsigned)bus_width,
             s_card.real_freq_khz);
    return ESP_OK;
}

static esp_err_t mount_fat_read_only(void)
{
    ff_diskio_register_sdmmc(0, &s_card);

    const esp_vfs_fat_conf_t conf = {
        .base_path = MOUNT_POINT,
        .fat_drive = FAT_DRIVE,
        .max_files = 4,
    };
    esp_err_t err = esp_vfs_fat_register_cfg(&conf, &s_fs);
    if (err != ESP_OK) {
        return err;
    }

    /* Deliberately do not format or repartition if this mount fails. */
    const FRESULT result = f_mount(s_fs, FAT_DRIVE, 1);
    if (result != FR_OK) {
        ESP_LOGE(TAG, "FAT mount failed: %s (%d); no format attempted",
                 fatfs_result_name(result), result);
        (void)f_mount(NULL, FAT_DRIVE, 0);
        (void)esp_vfs_fat_unregister_path(MOUNT_POINT);
        s_fs = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool list_fat_root(void)
{
    DIR *directory = opendir(MOUNT_POINT);
    if (directory == NULL) {
        ESP_LOGE(TAG, "FAT root listing failed: errno=%d (%s)", errno, strerror(errno));
        return false;
    }

    bool ok = true;
    errno = 0;
    while (true) {
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                ESP_LOGE(TAG, "FAT root readdir failed: errno=%d (%s)", errno, strerror(errno));
                ok = false;
            }
            break;
        }
        ESP_LOGI(TAG, "FAT root entry: %s", entry->d_name);
    }
    if (closedir(directory) != 0) {
        ESP_LOGE(TAG, "FAT root closedir failed: errno=%d (%s)", errno, strerror(errno));
        ok = false;
    }
    return ok;
}

static bool read_expected_readme(void)
{
    FILE *file = fopen(README_PATH, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "README open failed: errno=%d (%s)", errno, strerror(errno));
        return false;
    }

    uint8_t actual[sizeof(EXPECTED_README) - 1];
    const size_t expected_size = sizeof(EXPECTED_README) - 1;
    const size_t read_size = fread(actual, 1, expected_size, file);
    const int extra_byte = fgetc(file);
    const bool ok = read_size == expected_size &&
                    extra_byte == EOF &&
                    !ferror(file) &&
                    memcmp(actual, EXPECTED_README, expected_size) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "README verification failed: read=%u expected=%u extra=0x%x",
                 (unsigned)read_size, (unsigned)expected_size, extra_byte);
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "README close failed: errno=%d (%s)", errno, strerror(errno));
        return false;
    }
    return ok;
}

static void make_scratch_payload(void)
{
    for (size_t i = 0; i < SCRATCH_BYTES; ++i) {
        s_scratch_payload[i] = (uint8_t)((i * 37U + (i >> 8) * 11U + 0x5aU) & 0xffU);
    }
}

static bool delete_scratch_file(void)
{
    ESP_LOGI(TAG, "P4-NANO SD FILE DELETE: BEGIN");
    if (unlink(SCRATCH_PATH) != 0) {
        ESP_LOGE(TAG, "scratch delete failed: errno=%d (%s)", errno, strerror(errno));
        report_write_failure("P4-NANO SD FILE DELETE", "unlink failed");
        return false;
    }

    if (access(SCRATCH_PATH, F_OK) == 0) {
        report_write_failure("P4-NANO SD FILE DELETE", "file still present after unlink");
        return false;
    }
    if (errno != ENOENT) {
        ESP_LOGE(TAG, "scratch absence check failed: errno=%d (%s)", errno, strerror(errno));
        report_write_failure("P4-NANO SD FILE DELETE", "absence check failed");
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO SD FILE DELETE: PASS");
    return true;
}

static bool run_write_test(void)
{
    ESP_LOGI(TAG, "P4-NANO SD WRITE TEST BEGIN");
    make_scratch_payload();

    ESP_LOGI(TAG, "P4-NANO SD FILE CREATE: BEGIN");
    const int fd = open(SCRATCH_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            ESP_LOGE(TAG, "SCRATCH_EXISTS: %s was not overwritten or deleted", SCRATCH_PATH);
        } else {
            ESP_LOGE(TAG, "scratch exclusive create failed: errno=%d (%s)", errno, strerror(errno));
        }
        report_write_failure("P4-NANO SD FILE CREATE", "exclusive create failed");
        report_write_failure("P4-NANO SD FILE WRITE", "not attempted");
        report_write_failure("P4-NANO SD FILE READ", "not attempted");
        report_write_failure("P4-NANO SD FILE VERIFY", "not attempted");
        report_write_failure("P4-NANO SD FILE DELETE", "not attempted");
        ESP_LOGE(TAG, "P4-NANO SD WRITE TEST RESULT: FAIL");
        return false;
    }
    ESP_LOGI(TAG, "P4-NANO SD FILE CREATE: PASS path=%s bytes=%u",
             SCRATCH_PATH, SCRATCH_BYTES);

    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "scratch fdopen failed: errno=%d (%s)", errno, strerror(errno));
        close(fd);
        report_write_failure("P4-NANO SD FILE WRITE", "fdopen failed");
        report_write_failure("P4-NANO SD FILE READ", "not attempted");
        report_write_failure("P4-NANO SD FILE VERIFY", "not attempted");
        (void)delete_scratch_file();
        ESP_LOGI(TAG, "P4-NANO SD WRITE TEST RESULT: FAIL");
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO SD FILE WRITE: BEGIN");
    size_t written = 0;
    while (written < SCRATCH_BYTES) {
        const size_t count = fwrite(s_scratch_payload + written, 1,
                                    SCRATCH_BYTES - written, file);
        if (count == 0) {
            break;
        }
        written += count;
    }
    bool write_ok = written == SCRATCH_BYTES;
    if (write_ok && fflush(file) != 0) {
        write_ok = false;
    }
    const int write_close_result = fclose(file);
    if (write_close_result != 0) {
        write_ok = false;
    }
    if (!write_ok) {
        ESP_LOGE(TAG, "scratch write failed: written=%u close=%d errno=%d (%s)",
                 (unsigned)written, write_close_result, errno, strerror(errno));
        report_write_failure("P4-NANO SD FILE WRITE", "write/flush/close failed");
        report_write_failure("P4-NANO SD FILE READ", "not attempted");
        report_write_failure("P4-NANO SD FILE VERIFY", "not attempted");
        (void)delete_scratch_file();
        ESP_LOGI(TAG, "P4-NANO SD WRITE TEST RESULT: FAIL");
        return false;
    }
    ESP_LOGI(TAG, "P4-NANO SD FILE WRITE: PASS bytes=%u", SCRATCH_BYTES);

    ESP_LOGI(TAG, "P4-NANO SD FILE READ: BEGIN");
    file = fopen(SCRATCH_PATH, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "scratch reopen failed: errno=%d (%s)", errno, strerror(errno));
        report_write_failure("P4-NANO SD FILE READ", "reopen failed");
        report_write_failure("P4-NANO SD FILE VERIFY", "not attempted");
        (void)delete_scratch_file();
        ESP_LOGI(TAG, "P4-NANO SD WRITE TEST RESULT: FAIL");
        return false;
    }

    const size_t read_size = fread(s_scratch_readback, 1, SCRATCH_BYTES, file);
    const int extra_byte = fgetc(file);
    const bool read_ok = read_size == SCRATCH_BYTES && extra_byte == EOF && !ferror(file);
    const int close_result = fclose(file);
    if (!read_ok || close_result != 0) {
        ESP_LOGE(TAG, "scratch read failed: read=%u extra=0x%x close=%d",
                 (unsigned)read_size, extra_byte, close_result);
        report_write_failure("P4-NANO SD FILE READ", "read/close failed");
        report_write_failure("P4-NANO SD FILE VERIFY", "not attempted");
        (void)delete_scratch_file();
        ESP_LOGI(TAG, "P4-NANO SD WRITE TEST RESULT: FAIL");
        return false;
    }
    ESP_LOGI(TAG, "P4-NANO SD FILE READ: PASS bytes=%u", SCRATCH_BYTES);

    const bool verify_ok = memcmp(s_scratch_payload, s_scratch_readback, SCRATCH_BYTES) == 0;
    ESP_LOGI(TAG, "P4-NANO SD FILE VERIFY: %s", verify_ok ? "PASS" : "FAIL");
    const bool delete_ok = delete_scratch_file();
    const bool result = verify_ok && delete_ok;
    ESP_LOGI(TAG, "P4-NANO SD WRITE TEST RESULT: %s", result ? "PASS" : "FAIL");
    return result;
}

static bool run_read_only_diagnostic(void)
{
    bool ok = true;

    esp_err_t err = configure_sd_power();
    report_esp_marker("P4-NANO SD POWER CONFIG", err);
    if (err != ESP_OK) {
        ok = false;
    }

    bool host_ok = false;
    if (ok) {
        err = initialize_sd_host();
        host_ok = err == ESP_OK;
    } else {
        err = ESP_FAIL;
    }
    report_esp_marker("P4-NANO SD HOST INIT", err);
    ok = ok && host_ok;

    bool card_ok = false;
    if (host_ok) {
        err = initialize_sd_card();
        card_ok = err == ESP_OK;
    } else {
        err = ESP_FAIL;
    }
    report_esp_marker("P4-NANO SD CARD INIT", err);
    ok = ok && card_ok;

    bool info_ok = false;
    if (card_ok) {
        err = report_sd_card_info();
        info_ok = err == ESP_OK;
    } else {
        err = ESP_FAIL;
    }
    report_esp_marker("P4-NANO SD CARD INFO", err);
    ok = ok && info_ok;

    bool mount_ok = false;
    if (info_ok) {
        err = mount_fat_read_only();
        mount_ok = err == ESP_OK;
    } else {
        err = ESP_FAIL;
    }
    report_esp_marker("P4-NANO FAT MOUNT", err);
    ok = ok && mount_ok;

    bool root_ok = false;
    if (mount_ok) {
        root_ok = list_fat_root();
    }
    ESP_LOGI(TAG, "P4-NANO FAT ROOT LIST: %s", root_ok ? "PASS" : "FAIL");
    ok = ok && root_ok;

    bool readme_ok = false;
    if (root_ok) {
        readme_ok = read_expected_readme();
    }
    ESP_LOGI(TAG, "P4-NANO README READ: %s", readme_ok ? "PASS" : "FAIL");
    ok = ok && readme_ok;

    ESP_LOGI(TAG, "P4-NANO SD READ-ONLY RESULT: %s", ok ? "PASS" : "FAIL");
    return ok;
}

static esp_err_t initialize_command_uart(void)
{
    const uart_port_t uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
    if (!uart_is_driver_installed(uart_num)) {
        esp_err_t err = uart_driver_install(uart_num, COMMAND_RX_BUFFER_SIZE, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UART driver install failed: %s (0x%x)", esp_err_to_name(err), err);
            return err;
        }
        ESP_LOGI(TAG, "UART RX driver installed on console UART %d", uart_num);
    } else {
        ESP_LOGI(TAG, "UART RX driver already installed on console UART %d", uart_num);
    }

    /* Keep console output and the explicit RX reader on the same driver-backed UART. */
    uart_vfs_dev_use_driver(uart_num);
    return ESP_OK;
}

static void command_parser_reset(command_parser_t *parser)
{
    parser->length = 0;
    parser->invalid = false;
    parser->overflow = false;
    parser->pending_cr = false;
    parser->line[0] = '\0';
}

static void command_parser_log_ignored(const command_parser_t *parser, const char *reason)
{
    if (parser->length == 0 && !parser->invalid && !parser->overflow) {
        return;
    }
    if (parser->overflow) {
        ESP_LOGW(TAG, "ignored overlong UART command line");
    } else if (parser->invalid) {
        ESP_LOGW(TAG, "ignored UART command line containing non-ASCII data");
    } else if (reason != NULL) {
        ESP_LOGW(TAG, "ignored UART command line (%s)", reason);
    } else {
        ESP_LOGW(TAG, "ignored ASCII UART command line: \"%s\"", parser->line);
    }
}

static void command_parser_log_ascii(const command_parser_t *parser)
{
    static const char hex_digits[] = "0123456789abcdef";
    char hex[COMMAND_LINE_BUFFER_SIZE * 3] = {0};
    size_t hex_length = 0;

    for (size_t i = 0; i < parser->length; ++i) {
        if (i > 0) {
            hex[hex_length++] = ' ';
        }
        const uint8_t byte = (uint8_t)parser->line[i];
        hex[hex_length++] = hex_digits[byte >> 4];
        hex[hex_length++] = hex_digits[byte & 0x0f];
    }
    hex[hex_length] = '\0';

    ESP_LOGI(TAG, "UART command line length=%u", (unsigned)parser->length);
    ESP_LOGI(TAG, "UART command line hex=%s", hex);
}

static bool command_parser_finish_line(command_parser_t *parser)
{
    const bool accepted = !parser->invalid && !parser->overflow &&
                          parser->length == sizeof(WRITE_COMMAND) - 1 &&
                          memcmp(parser->line, WRITE_COMMAND, sizeof(WRITE_COMMAND) - 1) == 0;
    if (!parser->invalid && !parser->overflow) {
        command_parser_log_ascii(parser);
    }
    if (!accepted) {
        command_parser_log_ignored(parser, NULL);
    }
    command_parser_reset(parser);
    return accepted;
}

static bool command_parser_feed(command_parser_t *parser, uint8_t byte)
{
    if (byte == '\n') {
        return command_parser_finish_line(parser);
    }

    if (byte == '\r') {
        if (parser->pending_cr) {
            command_parser_log_ignored(parser, "bare CR");
            command_parser_reset(parser);
        }
        parser->pending_cr = true;
        return false;
    }

    if (parser->pending_cr) {
        command_parser_log_ignored(parser, "bare CR");
        command_parser_reset(parser);
    }

    /* A non-ASCII byte without an active line is treated as startup garbage. */
    if (byte < 0x20 || byte > 0x7e) {
        if (parser->length == 0 && !parser->invalid && !parser->overflow) {
            ESP_LOGW(TAG, "ignored non-ASCII UART byte: 0x%02x", byte);
        } else {
            parser->invalid = true;
        }
        return false;
    }

    if (parser->invalid || parser->overflow) {
        return false;
    }
    if (parser->length >= sizeof(parser->line) - 1) {
        parser->overflow = true;
        return false;
    }

    parser->line[parser->length++] = (char)byte;
    parser->line[parser->length] = '\0';
    return false;
}

static void wait_for_host_command(void)
{
    command_parser_t parser;
    command_parser_reset(&parser);

    const uart_port_t uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
    if (initialize_command_uart() != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO SD UART RX INIT: FAIL");
        return;
    }

    ESP_LOGI(TAG, "P4-NANO SD UART RX INIT: PASS");
    ESP_LOGI(TAG, "P4-NANO SD WRITE COMMAND READY");
    if (safe_off_led_set(true)) {
        ESP_LOGI(TAG, "P4-NANO SAFE TO POWER OFF: YES");
    } else {
        ESP_LOGE(TAG, "P4-NANO SAFE TO POWER OFF: NO");
    }

    while (true) {
        uint8_t byte = 0;
        const int received = uart_read_bytes(uart_num, &byte, 1,
                                             pdMS_TO_TICKS(COMMAND_RX_TIMEOUT_MS));
        if (received <= 0) {
            continue;
        }
        if (command_parser_feed(&parser, byte)) {
            (void)safe_off_led_set(false);
            ESP_LOGI(TAG, "P4-NANO SAFE TO POWER OFF: NO");
            ESP_LOGI(TAG, "P4-NANO SD WRITE COMMAND ACCEPTED");
            const bool write_ok = run_write_test();
            if (write_ok && safe_off_led_set(true)) {
                ESP_LOGI(TAG, "P4-NANO SAFE TO POWER OFF: YES");
            } else {
                (void)safe_off_led_set(false);
                ESP_LOGE(TAG, "P4-NANO SAFE TO POWER OFF: NO");
            }
        }
    }
}

void app_main(void)
{
    const esp_err_t led_err = safe_off_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO SAFE-OFF LED INIT: FAIL (%s, 0x%x)",
                 esp_err_to_name(led_err), led_err);
    } else {
        ESP_LOGI(TAG, "P4-NANO SAFE-OFF LED INIT: PASS GPIO20");
    }

    ESP_LOGI(TAG, "P4-NANO SDMMC START");
    ESP_LOGI(TAG, "prepared card requirement: /README.TXT exact bytes: ESP32-P4 SD TEST CARD\\n");

    if (!run_read_only_diagnostic()) {
        ESP_LOGE(TAG, "read-only diagnostic failed; no write test will be accepted");
        return;
    }
    wait_for_host_command();
}
