#include <stdbool.h>
#include <stdint.h>

#include "eh_host_core.h"
#include "eh_host_event.h"
#include "eh_host_sys.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4_nano_wireless";
static bool s_cp_init_seen;

static void host_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != EH_HOST_EVENT) {
        return;
    }

    if (event_id == EH_HOST_EVENT_CP_INIT) {
        const eh_host_event_init_t *init = (const eh_host_event_init_t *)event_data;
        s_cp_init_seen = true;
        ESP_LOGI(TAG, "P4-NANO WIRELESS CP INIT EVENT: PASS reset_reason=%d",
                 init != NULL ? (int)init->reason : -1);
    }
}

static void report_result_failure(const char *stage, int rc)
{
    ESP_LOGE(TAG, "P4-NANO WIRELESS %s: FAIL rc=%d (%s)",
             stage, rc, esp_err_to_name((esp_err_t)rc));
    ESP_LOGE(TAG, "P4-NANO WIRELESS TRANSPORT RESULT: FAIL");
}

void app_main(void)
{
    int rc = eh_host_init(NULL);
    if (rc != 0) {
        report_result_failure("HOST INIT", rc);
        return;
    }
    ESP_LOGI(TAG, "P4-NANO WIRELESS HOST INIT: PASS");

    esp_err_t event_rc = esp_event_handler_register(
        EH_HOST_EVENT, ESP_EVENT_ANY_ID, host_event_handler, NULL);
    if (event_rc != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO WIRELESS CP INIT EVENT: FAIL rc=%s",
                 esp_err_to_name(event_rc));
        ESP_LOGE(TAG, "P4-NANO WIRELESS TRANSPORT RESULT: FAIL");
        return;
    }

    rc = eh_host_connect_to_slave();
    if (rc != 0) {
        report_result_failure("CONNECT", rc);
        ESP_LOGE(TAG, "P4-NANO WIRELESS TRANSPORT: FAIL");
        return;
    }
    ESP_LOGI(TAG, "P4-NANO WIRELESS CONNECT: PASS");
    ESP_LOGI(TAG, "P4-NANO WIRELESS TRANSPORT: PASS");

    /* The CP INIT event is part of the Layer-1 acceptance boundary. */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_cp_init_seen) {
        ESP_LOGE(TAG, "P4-NANO WIRELESS CP INIT EVENT: FAIL");
        ESP_LOGE(TAG, "P4-NANO WIRELESS TRANSPORT RESULT: FAIL");
        return;
    }
    ESP_LOGI(TAG, "P4-NANO WIRELESS TRANSPORT RESULT: PASS");

#if CONFIG_P4_NANO_WIRELESS_RUN_FW_VERSION_QUERY
    /* This is the only control-plane RPC issued by this diagnostic. */
    eh_host_coprocessor_fwver_t fw_version = {0};
    esp_err_t fw_rc = eh_host_sys_get_cp_fw_version(&fw_version);
    if (fw_rc == ESP_OK) {
        ESP_LOGI(TAG,
                 "P4-NANO WIRELESS FW QUERY: PASS version=%u.%u.%u revision=%ld prerelease=%ld build=%ld",
                 (unsigned)fw_version.major1, (unsigned)fw_version.minor1,
                 (unsigned)fw_version.patch1, (long)fw_version.revision,
                 (long)fw_version.prerelease, (long)fw_version.build);
    } else if (fw_rc == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "P4-NANO WIRELESS FW QUERY: UNSUPPORTED rc=%s",
                 esp_err_to_name(fw_rc));
    } else {
        ESP_LOGE(TAG, "P4-NANO WIRELESS FW QUERY: FAIL rc=%d (%s)",
                 (int)fw_rc, esp_err_to_name(fw_rc));
        ESP_LOGE(TAG,
                 "P4-NANO WIRELESS FW QUERY: NO RESPONSE - FACTORY CP PROTOCOL HAS NO MSG_ID 350 REQUEST");
    }
#else
    ESP_LOGI(TAG,
             "P4-NANO WIRELESS FW QUERY: NOT RUN (factory CP has no MSG_ID 350 request)");
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
