#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "hid_boot_keyboard.h"

#define TAG "p4_nano_usb_host"

#define USB_ENUMERATION_TIMEOUT_MS 20000U
#define USB_SEQUENCE_TIMEOUT_MS 120000U
#define USB_EVENT_QUEUE_LENGTH 24U
#define USB_REPORT_BUFFER_SIZE 64U

typedef enum {
    APP_EVENT_HID_DRIVER,
    APP_EVENT_HID_INTERFACE,
} app_event_kind_t;

typedef struct {
    app_event_kind_t kind;
    hid_host_device_handle_t handle;
    union {
        hid_host_driver_event_t driver_event;
        struct {
            hid_host_interface_event_t event;
            uint8_t data[USB_REPORT_BUFFER_SIZE];
            size_t length;
            esp_err_t read_error;
        } interface_event;
    } value;
} app_event_t;

typedef struct {
    hid_boot_keyboard_event_kind_t kind;
    uint8_t usage;
    uint8_t modifiers;
} expected_event_t;

static const expected_event_t s_expected_sequence[] = {
    {HID_BOOT_EVENT_KEY_PRESS, 0x04, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x1e, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x1e, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x2c, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x2c, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x28, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x28, 0x00},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe1, 0x02},
    {HID_BOOT_EVENT_KEY_PRESS, 0x04, 0x02},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0x02},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe1, 0x00},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe0, 0x01},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe0, 0x00},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe2, 0x04},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe2, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x3a, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x3a, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x52, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x52, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x51, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x51, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x50, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x50, 0x00},
    {HID_BOOT_EVENT_KEY_PRESS, 0x4f, 0x00},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x4f, 0x00},
};

static QueueHandle_t s_app_event_queue;
static usb_host_client_handle_t s_usb_client;
static SemaphoreHandle_t s_usb_client_done;
static SemaphoreHandle_t s_usb_lib_done;
static SemaphoreHandle_t s_hid_disconnect_done;
static SemaphoreHandle_t s_direct_device_gone_done;
static volatile bool s_shutdown_requested;
static bool s_hid_installed;
static volatile bool s_usb_host_installed;
static volatile bool s_keyboard_active;
static volatile hid_host_device_handle_t s_keyboard_handle;
static volatile bool s_keyboard_opened;
static volatile bool s_hid_device_seen;
static volatile bool s_hid_device_gone;
static volatile bool s_hid_disconnect_callback_done;
static volatile bool s_hid_force_disconnect_requested;
static usb_device_handle_t s_direct_device;
static volatile bool s_direct_device_gone;
static bool s_hid_stop_pass;
static bool s_hid_close_pass;
static bool s_hid_uninstall_pass;
static bool s_usb_no_clients_pass;
static bool s_usb_devices_free_pass;
static bool s_usb_uninstall_pass;
static bool s_root_device_seen;
static bool s_root_device_failed;
static bool s_hid_failed;
static bool s_result_printed;
static size_t s_sequence_index;
static bool s_sequence_started;
static bool s_sequence_failed;

typedef enum {
    HID_IFACE_STATE_CLOSED,
    HID_IFACE_STATE_OPEN,
    HID_IFACE_STATE_ACTIVE,
    HID_IFACE_STATE_STOPPED,
} hid_iface_lifecycle_state_t;

static volatile hid_iface_lifecycle_state_t s_keyboard_state;

static const char *usb_speed_name(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return "LS";
    case USB_SPEED_FULL:
        return "FS";
    case USB_SPEED_HIGH:
        return "HS";
    default:
        return "UNKNOWN";
    }
}

static bool queue_app_event(const app_event_t *event)
{
    return s_app_event_queue != NULL &&
           xQueueSend(s_app_event_queue, event, 0) == pdTRUE;
}

static void log_usb_error(const char *operation, esp_err_t error)
{
    ESP_LOGE(TAG, "%s: %s (0x%x)", operation, esp_err_to_name(error), error);
}

static void usb_client_event_callback(const usb_host_client_event_msg_t *event_msg,
                                      void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "USB device gone");
        if (s_direct_device == event_msg->dev_gone.dev_hdl) {
            const esp_err_t close_ret = usb_host_device_close(
                s_usb_client, event_msg->dev_gone.dev_hdl);
            if (close_ret != ESP_OK) {
                log_usb_error("USB direct device close after disconnect failed", close_ret);
                s_root_device_failed = true;
            } else {
                s_direct_device = NULL;
                s_direct_device_gone = true;
                if (s_direct_device_gone_done != NULL) {
                    xSemaphoreGive(s_direct_device_gone_done);
                }
            }
        }
        return;
    }

    if (event_msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV || s_usb_client == NULL) {
        return;
    }

    const uint8_t address = event_msg->new_dev.address;
    usb_device_handle_t device = NULL;
    esp_err_t ret = usb_host_device_open(s_usb_client, address, &device);
    if (ret != ESP_OK) {
        s_root_device_failed = true;
        log_usb_error("USB direct device open failed", ret);
        ESP_LOGE(TAG, "P4-NANO USB DIRECT DEVICE: FAIL");
        return;
    }

    usb_device_info_t info;
    const usb_device_desc_t *descriptor = NULL;
    ret = usb_host_device_info(device, &info);
    if (ret == ESP_OK) {
        ret = usb_host_get_device_descriptor(device, &descriptor);
    }
    if (ret == ESP_OK && descriptor != NULL) {
        s_root_device_seen = true;
        s_direct_device = device;
        ESP_LOGI(TAG, "P4-NANO USB DIRECT DEVICE: PASS address=%u parent_port=%u",
                 info.dev_addr, info.parent.port_num);
        ESP_LOGI(TAG, "P4-NANO USB DIRECT VIDPID: %04x:%04x",
                 descriptor->idVendor, descriptor->idProduct);
        ESP_LOGI(TAG, "P4-NANO USB DIRECT SPEED: %s", usb_speed_name(info.speed));
    } else {
        s_root_device_failed = true;
        log_usb_error("USB direct device information failed", ret);
        ESP_LOGE(TAG, "P4-NANO USB DIRECT DEVICE: FAIL");
    }
    if (s_direct_device != device) {
        const esp_err_t close_ret = usb_host_device_close(s_usb_client, device);
        if (close_ret != ESP_OK) {
            log_usb_error("USB direct device close failed", close_ret);
        }
    }
}

static void usb_client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = usb_client_event_callback,
            .callback_arg = NULL,
        },
    };

    esp_err_t ret = usb_host_client_register(&client_config, &s_usb_client);
    if (ret != ESP_OK) {
        log_usb_error("USB client register failed", ret);
        s_root_device_failed = true;
        xSemaphoreGive(s_usb_client_done);
        vTaskDelete(NULL);
        return;
    }

    while (!s_shutdown_requested) {
        ret = usb_host_client_handle_events(s_usb_client, pdMS_TO_TICKS(250));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            log_usb_error("USB client event handling failed", ret);
            s_root_device_failed = true;
            break;
        }
    }

    if (s_usb_client != NULL) {
        ret = usb_host_client_deregister(s_usb_client);
        if (ret != ESP_OK) {
            log_usb_error("USB client deregister failed", ret);
        }
        s_usb_client = NULL;
    }
    xSemaphoreGive(s_usb_client_done);
    vTaskDelete(NULL);
}

static void usb_lib_task(void *arg)
{
    TaskHandle_t app_task = (TaskHandle_t)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
        .fifo_settings_custom = {},
        .peripheral_map = 0,
    };

    const esp_err_t install_ret = usb_host_install(&host_config);
    if (install_ret == ESP_OK) {
        s_usb_host_installed = true;
        ESP_LOGI(TAG, "P4-NANO USB HOST LIB INIT: PASS");
    } else {
        log_usb_error("USB host install failed", install_ret);
        ESP_LOGE(TAG, "P4-NANO USB HOST LIB INIT: FAIL");
    }
    xTaskNotifyGive(app_task);

    if (install_ret != ESP_OK) {
        xSemaphoreGive(s_usb_lib_done);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "P4-NANO USB ROOT PERIPHERAL: HS");
    esp_err_t ret;
    while (true) {
        uint32_t event_flags = 0;
        ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            log_usb_error("USB host library event handling failed", ret);
            break;
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0U) {
            s_usb_no_clients_pass = true;
            ESP_LOGI(TAG, "P4-NANO USB HOST NO CLIENTS: PASS");
            const esp_err_t free_ret = usb_host_device_free_all();
            if (free_ret == ESP_ERR_NOT_FINISHED) {
                while (true) {
                    uint32_t free_flags = 0;
                    ret = usb_host_lib_handle_events(portMAX_DELAY, &free_flags);
                    if (ret != ESP_OK) {
                        log_usb_error("USB host device free event handling failed", ret);
                        break;
                    }
                    if ((free_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U) {
                        s_usb_devices_free_pass = true;
                        ESP_LOGI(TAG, "P4-NANO USB HOST DEVICES FREE: PASS");
                        break;
                    }
                }
            } else if (free_ret != ESP_OK) {
                log_usb_error("USB host device free failed", free_ret);
            } else {
                s_usb_devices_free_pass = true;
                ESP_LOGI(TAG, "P4-NANO USB HOST DEVICES FREE: PASS");
            }
            break;
        }
    }

    const esp_err_t uninstall_ret = usb_host_uninstall();
    if (uninstall_ret != ESP_OK) {
        log_usb_error("USB host uninstall failed", uninstall_ret);
    } else {
        s_usb_uninstall_pass = true;
        ESP_LOGI(TAG, "P4-NANO USB HOST UNINSTALL: PASS");
    }
    xSemaphoreGive(s_usb_lib_done);
    vTaskDelete(NULL);
}

static void hid_host_interface_callback(hid_host_device_handle_t handle,
                                         const hid_host_interface_event_t event,
                                         void *arg)
{
    (void)arg;
    app_event_t app_event = {
        .kind = APP_EVENT_HID_INTERFACE,
        .handle = handle,
        .value.interface_event = {
            .event = event,
            .length = 0,
            .read_error = ESP_OK,
        },
    };

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        size_t length = 0;
        const esp_err_t ret = hid_host_device_get_raw_input_report_data(
            handle, app_event.value.interface_event.data,
            sizeof(app_event.value.interface_event.data), &length);
        if (ret != ESP_OK) {
            app_event.value.interface_event.event = HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR;
            app_event.value.interface_event.read_error = ret;
        } else {
            app_event.value.interface_event.length = length;
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        const esp_err_t ret = hid_host_device_close(handle);
        if (ret != ESP_OK) {
            log_usb_error("HID device close after disconnect failed", ret);
        } else {
            s_hid_close_pass = true;
            if (s_hid_force_disconnect_requested) {
                s_hid_disconnect_callback_done = true;
            }
            s_keyboard_state = HID_IFACE_STATE_CLOSED;
            ESP_LOGI(TAG, "P4-NANO USB HID CLOSE: PASS");
        }
        if (handle == s_keyboard_handle) {
            s_keyboard_active = false;
            s_keyboard_handle = NULL;
            s_keyboard_opened = false;
        }
    }

    if (!queue_app_event(&app_event)) {
        ESP_LOGE(TAG, "HID event queue full");
    }
}

static void hid_event_task(void *arg)
{
    (void)arg;
    while (hid_host_handle_events(portMAX_DELAY) == ESP_OK) {
        if (s_hid_disconnect_callback_done && !s_hid_device_gone) {
            s_hid_device_gone = true;
            if (s_hid_disconnect_done != NULL) {
                xSemaphoreGive(s_hid_disconnect_done);
            }
        }
    }
    vTaskDelete(NULL);
}

static void hid_host_device_callback(hid_host_device_handle_t handle,
                                      const hid_host_driver_event_t event,
                                      void *arg)
{
    (void)arg;
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        s_hid_device_seen = true;
    }
    const app_event_t app_event = {
        .kind = APP_EVENT_HID_DRIVER,
        .handle = handle,
        .value.driver_event = event,
    };
    if (!queue_app_event(&app_event)) {
        ESP_LOGE(TAG, "HID driver event queue full");
    }
}

static bool expected_event_matches(const hid_boot_keyboard_event_t *event)
{
    if (s_sequence_index >= sizeof(s_expected_sequence) / sizeof(s_expected_sequence[0])) {
        return false;
    }
    const expected_event_t *expected = &s_expected_sequence[s_sequence_index];
    return expected->kind == event->kind && expected->usage == event->usage &&
           expected->modifiers == event->modifiers;
}

static void observe_keyboard_event(const hid_boot_keyboard_event_t *event)
{
    switch (event->kind) {
    case HID_BOOT_EVENT_KEY_PRESS:
        ESP_LOGI(TAG, "P4-NANO USB KEY PRESS: usage=0x%02x modifiers=0x%02x",
                 event->usage, event->modifiers);
        break;
    case HID_BOOT_EVENT_KEY_RELEASE:
        ESP_LOGI(TAG, "P4-NANO USB KEY RELEASE: usage=0x%02x modifiers=0x%02x",
                 event->usage, event->modifiers);
        break;
    case HID_BOOT_EVENT_MOD_PRESS:
        ESP_LOGI(TAG, "P4-NANO USB MOD PRESS: usage=0x%02x modifiers=0x%02x",
                 event->usage, event->modifiers);
        break;
    case HID_BOOT_EVENT_MOD_RELEASE:
        ESP_LOGI(TAG, "P4-NANO USB MOD RELEASE: usage=0x%02x modifiers=0x%02x",
                 event->usage, event->modifiers);
        break;
    case HID_BOOT_EVENT_ERROR_USAGE:
        ESP_LOGW(TAG, "P4-NANO USB KEY ERROR: usage=0x%02x modifiers=0x%02x",
                 event->usage, event->modifiers);
        s_sequence_failed = true;
        return;
    }

    if (expected_event_matches(event)) {
        s_sequence_started = true;
        ++s_sequence_index;
        return;
    }

    if (event->kind == HID_BOOT_EVENT_KEY_RELEASE ||
        event->kind == HID_BOOT_EVENT_MOD_RELEASE) {
        return;
    }

    if (!s_sequence_started) {
        return;
    }

    s_sequence_failed = true;
    ESP_LOGE(TAG, "USB key sequence became ambiguous at step %u/%u",
             (unsigned)s_sequence_index,
             (unsigned)(sizeof(s_expected_sequence) / sizeof(s_expected_sequence[0])));
}

static void process_hid_driver_event(hid_host_device_handle_t handle,
                                     hid_host_driver_event_t event)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params;
    esp_err_t ret = hid_host_device_get_params(handle, &params);
    if (ret != ESP_OK) {
        s_hid_failed = true;
        log_usb_error("HID device parameter query failed", ret);
        ESP_LOGE(TAG, "P4-NANO USB DIRECT HID: FAIL");
        return;
    }

    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
        params.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "Ignoring non-keyboard HID interface addr=%u iface=%u subclass=%u protocol=%u",
                 params.addr, params.iface_num, params.sub_class, params.proto);
        return;
    }

    if (s_keyboard_handle != NULL) {
        ESP_LOGW(TAG, "Ignoring additional Boot keyboard interface");
        return;
    }

    const hid_host_device_config_t device_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    ret = hid_host_device_open(handle, &device_config);
    if (ret == ESP_OK) {
        s_keyboard_handle = handle;
        s_keyboard_opened = true;
        s_keyboard_state = HID_IFACE_STATE_OPEN;
    }
    if (ret == ESP_OK) {
        ret = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
    }
    if (ret == ESP_OK) {
        ret = hid_class_request_set_idle(handle, 0, 0);
    }
    if (ret == ESP_OK) {
        ret = hid_host_device_start(handle);
        if (ret == ESP_OK) {
            s_keyboard_state = HID_IFACE_STATE_ACTIVE;
        }
    }
    if (ret != ESP_OK) {
        s_hid_failed = true;
        log_usb_error("HID keyboard setup failed", ret);
        const esp_err_t close_ret = hid_host_device_close(handle);
        if (close_ret != ESP_OK) {
            log_usb_error("HID keyboard setup cleanup failed", close_ret);
        }
        ESP_LOGE(TAG, "P4-NANO USB DIRECT HID: FAIL");
        return;
    }

    s_keyboard_active = true;
    ESP_LOGI(TAG, "P4-NANO USB DIRECT HID: PASS");
    ESP_LOGI(TAG, "P4-NANO USB DIRECT HID IFACE: %u", params.iface_num);
    ESP_LOGI(TAG, "P4-NANO USB DIRECT HID SUBCLASS: %u", params.sub_class);
    ESP_LOGI(TAG, "P4-NANO USB DIRECT HID PROTOCOL: KEYBOARD");
}

static bool stop_hid_interface(void)
{
    bool pass = true;
    if (s_keyboard_state == HID_IFACE_STATE_ACTIVE && s_keyboard_handle != NULL) {
        const esp_err_t ret = hid_host_device_stop(s_keyboard_handle);
        if (ret != ESP_OK) {
            log_usb_error("HID keyboard stop failed", ret);
            pass = false;
        } else {
            s_keyboard_state = HID_IFACE_STATE_STOPPED;
        }
    } else if (s_keyboard_state == HID_IFACE_STATE_OPEN) {
        s_keyboard_state = HID_IFACE_STATE_STOPPED;
    }
    s_hid_stop_pass = pass;
    if (pass) {
        ESP_LOGI(TAG, "P4-NANO USB HID STOP: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO USB HID STOP: FAIL");
    }
    return pass;
}

static bool force_host_disconnect(void)
{
    if (!s_hid_device_seen && s_direct_device == NULL) {
        return true;
    }

    s_hid_force_disconnect_requested = true;
    const esp_err_t ret = usb_host_lib_set_root_port_power(false);
    if (ret != ESP_OK) {
        s_hid_force_disconnect_requested = false;
        log_usb_error("USB host forced disconnect failed", ret);
        return false;
    }

    if (s_direct_device != NULL && !s_direct_device_gone) {
        if (xSemaphoreTake(s_direct_device_gone_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "USB direct device gone event timed out");
            return false;
        }
    }

    if (s_keyboard_opened && !s_hid_device_gone) {
        if (xSemaphoreTake(s_hid_disconnect_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "HID disconnect callback timed out");
            return false;
        }
    }

    return true;
}

static bool request_host_shutdown(void)
{
    bool cleanup_pass = true;
    if (s_hid_installed) {
        cleanup_pass = stop_hid_interface() && cleanup_pass;

        if (s_hid_device_seen || s_direct_device != NULL) {
            cleanup_pass = force_host_disconnect() && cleanup_pass;
        }

        if (!s_keyboard_opened && !s_hid_close_pass) {
            s_hid_close_pass = true;
            ESP_LOGI(TAG, "P4-NANO USB HID CLOSE: PASS state=closed");
        }
        if (!s_hid_close_pass) {
            cleanup_pass = false;
            ESP_LOGE(TAG, "P4-NANO USB HID CLOSE: FAIL");
        }

        const esp_err_t ret = hid_host_uninstall();
        if (ret != ESP_OK) {
            log_usb_error("HID host uninstall failed", ret);
            ESP_LOGE(TAG, "P4-NANO USB HID UNINSTALL: FAIL");
            cleanup_pass = false;
        } else {
            s_hid_uninstall_pass = true;
            ESP_LOGI(TAG, "P4-NANO USB HID UNINSTALL: PASS");
        }
        s_hid_installed = false;
    }

    s_shutdown_requested = true;
    if (s_usb_client != NULL) {
        const esp_err_t ret = usb_host_client_unblock(s_usb_client);
        if (ret != ESP_OK) {
            log_usb_error("USB client unblock failed", ret);
        }
    }
    (void)xSemaphoreTake(s_usb_client_done, pdMS_TO_TICKS(3000));
    (void)xSemaphoreTake(s_usb_lib_done, pdMS_TO_TICKS(5000));

    if (!s_usb_no_clients_pass) {
        ESP_LOGE(TAG, "P4-NANO USB HOST NO CLIENTS: FAIL");
        cleanup_pass = false;
    }
    if (!s_usb_devices_free_pass) {
        ESP_LOGE(TAG, "P4-NANO USB HOST DEVICES FREE: FAIL");
        cleanup_pass = false;
    }
    if (!s_usb_uninstall_pass) {
        ESP_LOGE(TAG, "P4-NANO USB HOST UNINSTALL: FAIL");
        cleanup_pass = false;
    }
    if (cleanup_pass) {
        ESP_LOGI(TAG, "P4-NANO USB CLEANUP RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO USB CLEANUP RESULT: FAIL");
    }
    return cleanup_pass;
}

static void finish_result(bool pass)
{
    if (s_result_printed) {
        return;
    }
    s_result_printed = true;
    if (pass) {
        ESP_LOGI(TAG, "P4-NANO USB DIRECT INPUT: PASS");
        ESP_LOGI(TAG, "P4-NANO USB DIRECT DIGITAL RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT INPUT: FAIL");
    }
    const bool cleanup_pass = request_host_shutdown();
    if (pass && cleanup_pass) {
        ESP_LOGI(TAG, "P4-NANO USB DIRECT RESULT: PASS");
        ESP_LOGI(TAG, "P4-NANO USB HOST RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT RESULT: FAIL");
        ESP_LOGE(TAG, "P4-NANO USB HOST RESULT: FAIL");
    }
}

static void fail_for_timeout(void)
{
    if (s_root_device_failed) {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT DEVICE: FAIL phase=device-info");
    } else if (!s_root_device_seen) {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT DEVICE: FAIL phase=enumeration");
    } else if (s_hid_failed || !s_keyboard_active) {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT HID: FAIL phase=boot-keyboard");
    } else if (s_sequence_failed) {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT INPUT: FAIL phase=ambiguous-or-error-report");
    } else {
        ESP_LOGE(TAG, "P4-NANO USB DIRECT INPUT: FAIL phase=sequence step=%u/%u",
                 (unsigned)s_sequence_index,
                 (unsigned)(sizeof(s_expected_sequence) / sizeof(s_expected_sequence[0])));
    }
    finish_result(false);
}

void app_main(void)
{
    ESP_LOGI(TAG, "P4-NANO USB HOST START");
    s_app_event_queue = xQueueCreate(USB_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    s_usb_client_done = xSemaphoreCreateBinary();
    s_usb_lib_done = xSemaphoreCreateBinary();
    s_hid_disconnect_done = xSemaphoreCreateBinary();
    s_direct_device_gone_done = xSemaphoreCreateBinary();
    if (s_app_event_queue == NULL || s_usb_client_done == NULL || s_usb_lib_done == NULL ||
        s_hid_disconnect_done == NULL || s_direct_device_gone_done == NULL) {
        ESP_LOGE(TAG, "P4-NANO USB HOST LIB INIT: FAIL allocation");
        return;
    }

    const TaskHandle_t app_task = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, (void *)app_task, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "P4-NANO USB HOST LIB INIT: FAIL task-create");
        return;
    }
    (void)ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));
    if (!s_usb_host_installed) {
        ESP_LOGE(TAG, "P4-NANO USB ROOT RESULT: FAIL");
        ESP_LOGE(TAG, "P4-NANO USB HOST RESULT: FAIL");
        return;
    }
    ESP_LOGI(TAG, "P4-NANO USB ROOT RESULT: PASS");

    if (xTaskCreate(usb_client_task, "usb_client", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "P4-NANO USB HOST LIB INIT: FAIL client-task-create");
        finish_result(false);
        return;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = false,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL,
    };
    esp_err_t ret = hid_host_install(&hid_config);
    if (ret != ESP_OK) {
        s_hid_failed = true;
        log_usb_error("HID host install failed", ret);
        ESP_LOGE(TAG, "P4-NANO USB DIRECT HID: FAIL");
        finish_result(false);
        return;
    }
    s_hid_installed = true;
    if (xTaskCreate(hid_event_task, "hid_events", 4096, NULL, 5, NULL) != pdPASS) {
        s_hid_failed = true;
        ESP_LOGE(TAG, "P4-NANO USB DIRECT HID: FAIL task-create");
        finish_result(false);
        return;
    }

    TickType_t enumeration_deadline = xTaskGetTickCount() +
                                       pdMS_TO_TICKS(USB_ENUMERATION_TIMEOUT_MS);
    TickType_t sequence_deadline = 0;
    while (!s_result_printed) {
        app_event_t event;
        if (xQueueReceive(s_app_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.kind == APP_EVENT_HID_DRIVER) {
                process_hid_driver_event(event.handle, event.value.driver_event);
            } else if (event.value.interface_event.event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
                if (event.handle == s_keyboard_handle && event.value.interface_event.length >=
                    HID_BOOT_KEYBOARD_REPORT_SIZE) {
                    static hid_boot_keyboard_state_t parser_state;
                    static bool parser_initialized;
                    if (!parser_initialized) {
                        hid_boot_keyboard_init(&parser_state);
                        parser_initialized = true;
                    }
                    hid_boot_keyboard_event_t transitions[HID_BOOT_KEYBOARD_MAX_EVENTS];
                    const size_t count = hid_boot_keyboard_process(
                        &parser_state, event.value.interface_event.data,
                        event.value.interface_event.length, transitions,
                        HID_BOOT_KEYBOARD_MAX_EVENTS);
                    for (size_t i = 0; i < count && i < HID_BOOT_KEYBOARD_MAX_EVENTS; ++i) {
                        observe_keyboard_event(&transitions[i]);
                    }
                }
            } else if (event.value.interface_event.event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
                s_sequence_failed = true;
                log_usb_error("HID input report transfer failed",
                              event.value.interface_event.read_error);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (s_keyboard_active && sequence_deadline == 0) {
            sequence_deadline = now + pdMS_TO_TICKS(USB_SEQUENCE_TIMEOUT_MS);
            ESP_LOGI(TAG, "P4-NANO USB DIRECT INPUT: READY timeout_ms=%u",
                     USB_SEQUENCE_TIMEOUT_MS);
        }
        if (s_sequence_index == sizeof(s_expected_sequence) / sizeof(s_expected_sequence[0])) {
            finish_result(true);
        } else if (s_sequence_failed) {
            finish_result(false);
        } else if (now >= enumeration_deadline &&
                   (!s_root_device_seen || !s_keyboard_active)) {
            fail_for_timeout();
        } else if (sequence_deadline != 0 && now >= sequence_deadline) {
            fail_for_timeout();
        }
    }

    ESP_LOGI(TAG, "P4-NANO USB HOST SAFE IDLE");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
