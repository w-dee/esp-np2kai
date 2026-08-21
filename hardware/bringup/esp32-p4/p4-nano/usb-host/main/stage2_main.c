#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch11.h"
#include "usb/usb_types_ch9.h"

#include "hid_boot_keyboard.h"

#define TAG "p4_nano_usb_host"

#define USB_ENUMERATION_TIMEOUT_MS 20000U
#define USB_SEQUENCE_TIMEOUT_MS 120000U
#define USB_EVENT_QUEUE_LENGTH 32U
#define USB_REPORT_BUFFER_SIZE 64U
#define USB_MAX_TRACKED_DEVICES 4U
#define USB_HUB_DESCRIPTOR_TRANSFER_SIZE \
    (sizeof(usb_setup_packet_t) + sizeof(usb_hub_descriptor_t))

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

typedef struct stage2_device stage2_device_t;

struct stage2_device {
    bool used;
    bool gone;
    bool retained;
    bool is_hub;
    bool has_hid_interface;
    bool descriptor_valid;
    bool config_valid;
    bool hub_descriptor_pending;
    bool hub_descriptor_valid;
    uint8_t address;
    uint16_t vid;
    uint16_t pid;
    uint8_t device_class;
    uint8_t hub_interface_protocol;
    uint8_t hub_ports;
    uint8_t hub_bm_attributes;
    uint8_t hub_max_power_2ma;
    usb_device_handle_t handle;
    usb_device_info_t info;
    usb_transfer_t *hub_descriptor_transfer;
};

typedef enum {
    STAGE2_RESULT_FAIL,
    STAGE2_RESULT_PARTIAL_BLOCKED_TT,
    STAGE2_RESULT_PASS,
} stage2_result_t;

static QueueHandle_t s_app_event_queue;
static usb_host_client_handle_t s_usb_client;
static SemaphoreHandle_t s_usb_client_done;
static SemaphoreHandle_t s_usb_lib_done;
static SemaphoreHandle_t s_hid_disconnect_done;
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
static bool s_hid_stop_pass;
static bool s_hid_close_pass;
static bool s_hid_uninstall_pass;
static bool s_usb_no_clients_pass;
static bool s_usb_devices_free_pass;
static bool s_usb_uninstall_pass;
static bool s_root_device_failed;
static bool s_hid_failed;
static bool s_result_printed;
static bool s_sequence_started;
static bool s_sequence_failed;
static size_t s_sequence_index;
static int s_hub_index = -1;
static int s_child_index = -1;
static bool s_hub_seen;
static bool s_hub_gone;
static bool s_child_seen;
static bool s_nested_hub_seen;
static bool s_input_ready_announced;
static bool s_tt_marker_printed;
static volatile bool s_tt_log_seen;
static stage2_device_t s_devices[USB_MAX_TRACKED_DEVICES];
static vprintf_like_t s_previous_vprintf;

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

static int stage2_log_vprintf(const char *format, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (written >= 0 && strstr(line, "transaction translator (TT) is not supported") != NULL) {
        s_tt_log_seen = true;
    }
    return s_previous_vprintf != NULL ? s_previous_vprintf(format, args) : 0;
}

static stage2_device_t *find_device_by_handle(usb_device_handle_t handle)
{
    for (size_t i = 0; i < USB_MAX_TRACKED_DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].handle == handle) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static stage2_device_t *find_device_by_address(uint8_t address)
{
    for (size_t i = 0; i < USB_MAX_TRACKED_DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].address == address) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static stage2_device_t *allocate_device(void)
{
    for (size_t i = 0; i < USB_MAX_TRACKED_DEVICES; ++i) {
        if (!s_devices[i].used) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            s_devices[i].used = true;
            return &s_devices[i];
        }
    }
    return NULL;
}

static bool inspect_config_descriptor(const usb_config_desc_t *config_desc,
                                      bool *is_hub,
                                      bool *has_hid,
                                      uint8_t *hub_protocol)
{
    if (config_desc == NULL || is_hub == NULL || has_hid == NULL || hub_protocol == NULL) {
        return false;
    }

    *is_hub = false;
    *has_hid = false;
    *hub_protocol = 0;
    int offset = 0;
    const usb_intf_desc_t *interface_desc = (const usb_intf_desc_t *)
        usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)config_desc,
                                           config_desc->wTotalLength,
                                           USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                           &offset);
    while (interface_desc != NULL) {
        if (interface_desc->bInterfaceClass == USB_CLASS_HUB) {
            *is_hub = true;
            *hub_protocol = interface_desc->bInterfaceProtocol;
        }
        if (interface_desc->bInterfaceClass == USB_CLASS_HID) {
            *has_hid = true;
        }
        interface_desc = (const usb_intf_desc_t *)
            usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)config_desc,
                                               config_desc->wTotalLength,
                                               USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                               &offset);
    }
    return true;
}

static bool device_is_expected_keyboard(const stage2_device_t *device)
{
    return device != NULL && device->vid == 0x0853 && device->pid == 0x0103;
}

static void log_parent_topology(const stage2_device_t *device)
{
    if (device->info.parent.dev_hdl == NULL) {
        ESP_LOGI(TAG, "P4-NANO USB HUB DEVICE TOPOLOGY: address=%u parent=root port=0",
                 device->address);
        return;
    }

    stage2_device_t *parent = find_device_by_handle(device->info.parent.dev_hdl);
    if (parent != NULL) {
        ESP_LOGI(TAG,
                 "P4-NANO USB HUB CHILD TOPOLOGY: address=%u parent_addr=%u parent_port=%u",
                 device->address, parent->address, device->info.parent.port_num);
    } else {
        ESP_LOGI(TAG,
                 "P4-NANO USB HUB CHILD TOPOLOGY: address=%u parent_addr=UNKNOWN parent_port=%u",
                 device->address, device->info.parent.port_num);
    }
}

static void hub_descriptor_transfer_callback(usb_transfer_t *transfer)
{
    stage2_device_t *device = (stage2_device_t *)transfer->context;
    if (device != NULL && device->used &&
        transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= (int)USB_HUB_DESCRIPTOR_TRANSFER_SIZE) {
        const usb_hub_descriptor_t *descriptor =
            (const usb_hub_descriptor_t *)(transfer->data_buffer + sizeof(usb_setup_packet_t));
        device->hub_ports = descriptor->bNbrPorts;
        device->hub_descriptor_valid = true;
        ESP_LOGI(TAG, "P4-NANO USB HUB DESCRIPTOR: PASS ports=%u", device->hub_ports);
    } else if (device != NULL && device->used) {
        ESP_LOGW(TAG, "P4-NANO USB HUB DESCRIPTOR: UNKNOWN status=%d actual=%d",
                 transfer->status, transfer->actual_num_bytes);
    }
    if (device != NULL) {
        device->hub_descriptor_pending = false;
        device->hub_descriptor_transfer = NULL;
    }
    const esp_err_t ret = usb_host_transfer_free(transfer);
    if (ret != ESP_OK) {
        log_usb_error("USB hub descriptor transfer free failed", ret);
    }
}

static void request_hub_descriptor(stage2_device_t *device)
{
    usb_transfer_t *transfer = NULL;
    esp_err_t ret = usb_host_transfer_alloc(USB_HUB_DESCRIPTOR_TRANSFER_SIZE, 0, &transfer);
    if (ret != ESP_OK) {
        log_usb_error("USB hub descriptor transfer allocation failed", ret);
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                           USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    setup->bRequest = USB_B_REQUEST_HUB_GET_DESCRIPTOR;
    setup->wValue = (USB_CLASS_DESCRIPTOR_TYPE_HUB << 8);
    setup->wIndex = 0;
    setup->wLength = sizeof(usb_hub_descriptor_t);
    transfer->device_handle = device->handle;
    transfer->num_bytes = USB_HUB_DESCRIPTOR_TRANSFER_SIZE;
    transfer->bEndpointAddress = 0;
    transfer->callback = hub_descriptor_transfer_callback;
    transfer->context = device;
    device->hub_descriptor_transfer = transfer;
    device->hub_descriptor_pending = true;

    ret = usb_host_transfer_submit_control(s_usb_client, transfer);
    if (ret != ESP_OK) {
        device->hub_descriptor_pending = false;
        device->hub_descriptor_transfer = NULL;
        log_usb_error("USB hub descriptor transfer submit failed", ret);
        (void)usb_host_transfer_free(transfer);
    }
}

static void close_tracked_device(stage2_device_t *device, const char *reason)
{
    if (device == NULL || device->handle == NULL || s_usb_client == NULL) {
        return;
    }
    const esp_err_t ret = usb_host_device_close(s_usb_client, device->handle);
    if (ret != ESP_OK) {
        log_usb_error(reason, ret);
        s_root_device_failed = true;
    } else {
        device->handle = NULL;
    }
}

static void usb_client_event_callback(const usb_host_client_event_msg_t *event_msg,
                                      void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        stage2_device_t *device = find_device_by_handle(event_msg->dev_gone.dev_hdl);
        if (device != NULL) {
            device->gone = true;
            if (device->is_hub) {
                s_hub_gone = true;
            }
            ESP_LOGI(TAG, "P4-NANO USB HUB DEVICE GONE: address=%u", device->address);
            close_tracked_device(device, "USB Stage 2 device close after disconnect failed");
        }
        return;
    }

    if (event_msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV || s_usb_client == NULL) {
        return;
    }

    const uint8_t address = event_msg->new_dev.address;
    usb_device_handle_t handle = NULL;
    esp_err_t ret = usb_host_device_open(s_usb_client, address, &handle);
    if (ret != ESP_OK) {
        s_root_device_failed = true;
        log_usb_error("USB Stage 2 device open failed", ret);
        return;
    }

    usb_device_info_t info;
    const usb_device_desc_t *descriptor = NULL;
    const usb_config_desc_t *config_desc = NULL;
    ret = usb_host_device_info(handle, &info);
    if (ret == ESP_OK) {
        ret = usb_host_get_device_descriptor(handle, &descriptor);
    }
    if (ret == ESP_OK) {
        ret = usb_host_get_active_config_descriptor(handle, &config_desc);
    }
    if (ret != ESP_OK || descriptor == NULL || config_desc == NULL) {
        log_usb_error("USB Stage 2 device descriptor query failed", ret);
        s_root_device_failed = true;
        (void)usb_host_device_close(s_usb_client, handle);
        return;
    }

    stage2_device_t *device = allocate_device();
    if (device == NULL) {
        ESP_LOGE(TAG, "P4-NANO USB HUB DEVICE: FAIL tracked-device-capacity");
        (void)usb_host_device_close(s_usb_client, handle);
        s_root_device_failed = true;
        return;
    }

    device->address = address;
    device->handle = handle;
    device->info = info;
    device->vid = descriptor->idVendor;
    device->pid = descriptor->idProduct;
    device->device_class = descriptor->bDeviceClass;
    device->descriptor_valid = true;
    device->config_valid = true;
    device->hub_bm_attributes = config_desc->bmAttributes;
    device->hub_max_power_2ma = config_desc->bMaxPower;
    inspect_config_descriptor(config_desc, &device->is_hub,
                              &device->has_hid_interface,
                              &device->hub_interface_protocol);

    log_parent_topology(device);
    ESP_LOGI(TAG, "P4-NANO USB HUB OBSERVED DEVICE: address=%u vidpid=%04x:%04x speed=%s class=0x%02x",
             device->address, device->vid, device->pid,
             usb_speed_name(device->info.speed), device->device_class);

    if (device->is_hub) {
        if (device->info.parent.dev_hdl != NULL) {
            s_nested_hub_seen = true;
            ESP_LOGE(TAG, "P4-NANO USB HUB NESTED: STOP_FOR_HUMAN_REVIEW address=%u",
                     device->address);
        }
        if (s_hub_index < 0 && device->info.parent.dev_hdl == NULL) {
            s_hub_index = (int)(device - s_devices);
            s_hub_seen = true;
            device->retained = true;
            ESP_LOGI(TAG, "P4-NANO USB HUB VIDPID: %04x:%04x", device->vid, device->pid);
            ESP_LOGI(TAG, "P4-NANO USB HUB SPEED: %s", usb_speed_name(device->info.speed));
            ESP_LOGI(TAG, "P4-NANO USB HUB CLASS: interface=0x09 protocol=0x%02x device_class=0x%02x",
                     device->hub_interface_protocol, device->device_class);
            ESP_LOGI(TAG, "P4-NANO USB HUB CONFIG POWER: %s max_power_mA=%u",
                     (config_desc->bmAttributes & USB_BM_ATTRIBUTES_SELFPOWER) != 0U
                         ? "SELF_POWERED" : "BUS_POWERED",
                     (unsigned)config_desc->bMaxPower * 2U);
            request_hub_descriptor(device);
        } else {
            device->retained = true;
        }
    } else if (device->info.parent.dev_hdl != NULL && device_is_expected_keyboard(device)) {
        if (s_child_index < 0) {
            s_child_index = (int)(device - s_devices);
            s_child_seen = true;
            device->retained = true;
            ESP_LOGI(TAG, "P4-NANO USB HUB CHILD VIDPID: %04x:%04x", device->vid, device->pid);
            ESP_LOGI(TAG, "P4-NANO USB HUB CHILD SPEED: %s", usb_speed_name(device->info.speed));
            ESP_LOGI(TAG, "P4-NANO USB HUB CHILD CLASS: device_class=0x%02x hid_interface=%s",
                     device->device_class, device->has_hid_interface ? "YES" : "NO");
        } else {
            ESP_LOGW(TAG, "Ignoring additional expected keyboard address=%u", device->address);
        }
    }

    if (!device->retained) {
        close_tracked_device(device, "Unexpected Stage 2 USB device close failed");
        device->used = false;
    }
}

static void usb_client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 12,
        .async = {
            .client_event_callback = usb_client_event_callback,
            .callback_arg = NULL,
        },
    };

    esp_err_t ret = usb_host_client_register(&client_config, &s_usb_client);
    if (ret != ESP_OK) {
        log_usb_error("USB Stage 2 client register failed", ret);
        s_root_device_failed = true;
        xSemaphoreGive(s_usb_client_done);
        vTaskDelete(NULL);
        return;
    }

    while (!s_shutdown_requested) {
        ret = usb_host_client_handle_events(s_usb_client, pdMS_TO_TICKS(250));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            log_usb_error("USB Stage 2 client event handling failed", ret);
            s_root_device_failed = true;
            break;
        }
    }

    if (s_usb_client != NULL) {
        ret = usb_host_client_deregister(s_usb_client);
        if (ret != ESP_OK) {
            log_usb_error("USB Stage 2 client deregister failed", ret);
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
        log_usb_error("USB Stage 2 host install failed", install_ret);
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
            log_usb_error("USB Stage 2 host library event handling failed", ret);
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
                        log_usb_error("USB Stage 2 host device free handling failed", ret);
                        break;
                    }
                    if ((free_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U) {
                        s_usb_devices_free_pass = true;
                        ESP_LOGI(TAG, "P4-NANO USB HOST DEVICES FREE: PASS");
                        break;
                    }
                }
            } else if (free_ret != ESP_OK) {
                log_usb_error("USB Stage 2 host device free failed", free_ret);
            } else {
                s_usb_devices_free_pass = true;
                ESP_LOGI(TAG, "P4-NANO USB HOST DEVICES FREE: PASS");
            }
            break;
        }
    }

    const esp_err_t uninstall_ret = usb_host_uninstall();
    if (uninstall_ret != ESP_OK) {
        log_usb_error("USB Stage 2 host uninstall failed", uninstall_ret);
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
            log_usb_error("Stage 2 HID device close after disconnect failed", ret);
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
        ESP_LOGE(TAG, "Stage 2 HID event queue full");
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
        ESP_LOGE(TAG, "Stage 2 HID driver event queue full");
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
    ESP_LOGE(TAG, "Stage 2 USB key sequence became ambiguous at step %u/%u",
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
        log_usb_error("Stage 2 HID device parameter query failed", ret);
        return;
    }

    stage2_device_t *device = find_device_by_address(params.addr);
    if (device == NULL || !device_is_expected_keyboard(device)) {
        ESP_LOGI(TAG, "Ignoring non-target HID interface addr=%u iface=%u subclass=%u protocol=%u",
                 params.addr, params.iface_num, params.sub_class, params.proto);
        return;
    }

    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
        params.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "Target child HID is not Boot Protocol keyboard addr=%u iface=%u subclass=%u protocol=%u",
                 params.addr, params.iface_num, params.sub_class, params.proto);
        return;
    }

    if (s_keyboard_handle != NULL) {
        ESP_LOGW(TAG, "Ignoring additional target Boot keyboard interface");
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
        log_usb_error("Stage 2 HID keyboard setup failed", ret);
        const esp_err_t close_ret = hid_host_device_close(handle);
        if (close_ret != ESP_OK) {
            log_usb_error("Stage 2 HID keyboard setup cleanup failed", close_ret);
        }
        s_keyboard_handle = NULL;
        s_keyboard_opened = false;
        s_keyboard_state = HID_IFACE_STATE_CLOSED;
        return;
    }

    s_keyboard_active = true;
    ESP_LOGI(TAG, "P4-NANO USB HUB HID BOOT: PASS iface=%u", params.iface_num);
    ESP_LOGI(TAG, "P4-NANO USB HUB HID SUBCLASS: %u", params.sub_class);
    ESP_LOGI(TAG, "P4-NANO USB HUB HID PROTOCOL: KEYBOARD");
    if (!s_input_ready_announced) {
        s_input_ready_announced = true;
        ESP_LOGI(TAG, "HUB + HID READY FOR KEY TEST");
        ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: WAITING_FOR_HUMAN_READY");
    }
}

static bool stop_hid_interface(void)
{
    bool pass = true;
    if (s_keyboard_state == HID_IFACE_STATE_ACTIVE && s_keyboard_handle != NULL) {
        const esp_err_t ret = hid_host_device_stop(s_keyboard_handle);
        if (ret != ESP_OK) {
            log_usb_error("Stage 2 HID keyboard stop failed", ret);
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

static bool any_hub_descriptor_pending(void)
{
    for (size_t i = 0; i < USB_MAX_TRACKED_DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].hub_descriptor_pending) {
            return true;
        }
    }
    return false;
}

static bool force_host_disconnect(void)
{
    if (!s_hub_seen && !s_hid_device_seen && !s_child_seen) {
        return true;
    }

    bool pass = true;
    s_hid_force_disconnect_requested = true;
    const esp_err_t ret = usb_host_lib_set_root_port_power(false);
    if (ret != ESP_OK) {
        s_hid_force_disconnect_requested = false;
        log_usb_error("USB Stage 2 forced disconnect failed", ret);
        return false;
    }

    if (s_keyboard_opened && !s_hid_device_gone) {
        if (xSemaphoreTake(s_hid_disconnect_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "Stage 2 HID disconnect callback timed out");
            pass = false;
        }
    }

    for (unsigned i = 0; i < 20U && any_hub_descriptor_pending(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (any_hub_descriptor_pending()) {
        ESP_LOGE(TAG, "Stage 2 hub descriptor transfer did not finish");
        pass = false;
    }

    /* Hub device gone is consumed by the stock Hub Driver. Close any retained
     * public client handles after root power-off, without a duplicate close. */
    vTaskDelay(pdMS_TO_TICKS(250));
    for (size_t i = 0; i < USB_MAX_TRACKED_DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].handle != NULL) {
            close_tracked_device(&s_devices[i],
                                 "USB Stage 2 retained device close failed");
            if (s_devices[i].handle != NULL) {
                pass = false;
            }
        }
    }
    return pass;
}

static bool request_host_shutdown(void)
{
    bool cleanup_pass = true;
    if (s_hid_installed) {
        cleanup_pass = stop_hid_interface() && cleanup_pass;
    }

    if (s_hid_device_seen || s_hub_seen || s_child_seen) {
        cleanup_pass = force_host_disconnect() && cleanup_pass;
    }

    if (s_hid_installed) {
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
            log_usb_error("Stage 2 HID host uninstall failed", ret);
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
            log_usb_error("Stage 2 USB client unblock failed", ret);
            cleanup_pass = false;
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

static void log_hub_summary(void)
{
    if (s_hub_index < 0) {
        ESP_LOGE(TAG, "P4-NANO USB HUB PORTS: UNKNOWN");
        return;
    }
    const stage2_device_t *hub = &s_devices[s_hub_index];
    if (hub->hub_descriptor_valid) {
        ESP_LOGI(TAG, "P4-NANO USB HUB PORTS: %u", hub->hub_ports);
    } else {
        ESP_LOGI(TAG, "P4-NANO USB HUB PORTS: UNKNOWN");
    }
}

static void finish_stage2(stage2_result_t result)
{
    if (s_result_printed) {
        return;
    }
    s_result_printed = true;

    const bool hub_pass = s_hub_seen && !s_hub_gone && !s_nested_hub_seen;
    ESP_LOGI(TAG, "P4-NANO USB HUB DEVICE: %s", hub_pass ? "PASS" : "FAIL");
    log_hub_summary();
    ESP_LOGI(TAG, "P4-NANO USB HUB RESULT: %s", hub_pass ? "PASS" : "FAIL");

    if (result == STAGE2_RESULT_PARTIAL_BLOCKED_TT) {
        ESP_LOGI(TAG, "P4-NANO USB HUB CHILD DETECT: BLOCKED_TT");
        ESP_LOGI(TAG, "P4-NANO USB HUB HID: BLOCKED_TT");
        ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: NOT_RUN");
    } else if (s_child_seen) {
        ESP_LOGI(TAG, "P4-NANO USB HUB CHILD DETECT: PASS");
        if (s_keyboard_active || s_sequence_index == sizeof(s_expected_sequence) /
                                        sizeof(s_expected_sequence[0])) {
            ESP_LOGI(TAG, "P4-NANO USB HUB HID: PASS");
        } else {
            ESP_LOGE(TAG, "P4-NANO USB HUB HID: FAIL");
        }
        if (s_sequence_index == sizeof(s_expected_sequence) /
                              sizeof(s_expected_sequence[0])) {
            ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: PASS");
        } else {
            ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: NOT_RUN");
        }
    } else {
        ESP_LOGE(TAG, "P4-NANO USB HUB CHILD DETECT: FAIL");
        ESP_LOGI(TAG, "P4-NANO USB HUB HID: NOT_RUN");
        ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: NOT_RUN");
    }

    const char *result_name = result == STAGE2_RESULT_PASS
                                  ? "PASS"
                                  : result == STAGE2_RESULT_PARTIAL_BLOCKED_TT
                                      ? "PARTIAL_BLOCKED_TT" : "FAIL";
    ESP_LOGI(TAG, "P4-NANO USB HUB DIGITAL RESULT: %s", result_name);

    const bool cleanup_pass = request_host_shutdown();
    if (!cleanup_pass) {
        ESP_LOGE(TAG, "P4-NANO USB HUB DIGITAL RESULT: FAIL_CLEANUP");
    }
    esp_log_set_vprintf(s_previous_vprintf);
}

static void fail_for_timeout(void)
{
    if (s_nested_hub_seen) {
        finish_stage2(STAGE2_RESULT_FAIL);
    } else if (s_tt_log_seen && s_hub_seen && !s_child_seen) {
        if (!s_tt_marker_printed) {
            s_tt_marker_printed = true;
            ESP_LOGI(TAG, "P4-NANO USB HUB TT EVIDENCE: STOCK_IDF_LOG_SEEN");
        }
        finish_stage2(STAGE2_RESULT_PARTIAL_BLOCKED_TT);
    } else {
        finish_stage2(STAGE2_RESULT_FAIL);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "P4-NANO USB HUB STAGE2 START");
    s_previous_vprintf = esp_log_set_vprintf(stage2_log_vprintf);

    s_app_event_queue = xQueueCreate(USB_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    s_usb_client_done = xSemaphoreCreateBinary();
    s_usb_lib_done = xSemaphoreCreateBinary();
    s_hid_disconnect_done = xSemaphoreCreateBinary();
    if (s_app_event_queue == NULL || s_usb_client_done == NULL || s_usb_lib_done == NULL ||
        s_hid_disconnect_done == NULL) {
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
        ESP_LOGE(TAG, "P4-NANO USB HUB RESULT: FAIL");
        return;
    }
    ESP_LOGI(TAG, "P4-NANO USB ROOT RESULT: PASS");

    if (xTaskCreate(usb_client_task, "usb_client", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "P4-NANO USB HOST LIB INIT: FAIL client-task-create");
        finish_stage2(STAGE2_RESULT_FAIL);
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
        log_usb_error("Stage 2 HID host install failed", ret);
        finish_stage2(STAGE2_RESULT_FAIL);
        return;
    }
    s_hid_installed = true;
    if (xTaskCreate(hid_event_task, "hid_events", 4096, NULL, 5, NULL) != pdPASS) {
        s_hid_failed = true;
        ESP_LOGE(TAG, "P4-NANO USB HUB HID: FAIL task-create");
        finish_stage2(STAGE2_RESULT_FAIL);
        return;
    }

    const TickType_t enumeration_deadline = xTaskGetTickCount() +
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
                log_usb_error("Stage 2 HID input report transfer failed",
                              event.value.interface_event.read_error);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (s_sequence_started && sequence_deadline == 0) {
            sequence_deadline = now + pdMS_TO_TICKS(USB_SEQUENCE_TIMEOUT_MS);
            ESP_LOGI(TAG, "P4-NANO USB HUB INPUT: CAPTURE_STARTED timeout_ms=%u",
                     USB_SEQUENCE_TIMEOUT_MS);
        }
        if (s_sequence_index == sizeof(s_expected_sequence) / sizeof(s_expected_sequence[0])) {
            finish_stage2(STAGE2_RESULT_PASS);
        } else if (s_sequence_failed) {
            finish_stage2(STAGE2_RESULT_FAIL);
        } else if (s_tt_log_seen && s_hub_seen && !s_child_seen) {
            fail_for_timeout();
        } else if (now >= enumeration_deadline) {
            fail_for_timeout();
        } else if (sequence_deadline != 0 && now >= sequence_deadline) {
            finish_stage2(STAGE2_RESULT_FAIL);
        }
    }

    ESP_LOGI(TAG, "P4-NANO USB HOST SAFE IDLE");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
