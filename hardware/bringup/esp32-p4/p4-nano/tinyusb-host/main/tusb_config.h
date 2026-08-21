#pragma once

#include "diagnostic_profile.h"

// TinyUSB 0.21.0 configuration for the ESP32-P4 HS controller used as an
// explicitly full-speed root bus. The HS/UTMI PHY remains selected in the
// DWC2 configuration; the root speed is intentionally full speed.
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 2

#define CFG_TUH_ENABLED 1
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HS_ROOT_DIRECT_FS
#define CFG_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED
#else
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
#endif
#define CFG_TUH_DEVICE_MAX 2
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 1
#define CFG_TUH_HID 1

#define CFG_TUH_HUB_BUFSIZE 12
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
#define CFG_TUH_HID_SET_PROTOCOL_ON_ENUM 1

// Keep the P4 DWC2 path in slave/FIFO mode and avoid PSRAM/D-cache DMA
// requirements in this first diagnostic.
#define CFG_TUH_DWC2_DMA_ENABLE 0
#define CFG_TUH_DWC2_SLAVE_ENABLE 1
#define CFG_TUH_MEM_DCACHE_ENABLE 0
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
