#pragma once

// Diagnostic-only build profiles.  The default preserves the original
// one-shot functional bring-up; robustness profiles add bounded control and
// logging without changing the TinyUSB stack or the board PHY implementation.
#define TINYUSB_PROFILE_LEGACY                  0
#define TINYUSB_PROFILE_COLD_BOOT               1
#define TINYUSB_PROFILE_KEYBOARD_HOTPLUG        2
#define TINYUSB_PROFILE_HUB_HOTPLUG             3
#define TINYUSB_PROFILE_REINIT                 4
#define TINYUSB_PROFILE_HS_ROOT_DIRECT_FS      5
#define TINYUSB_PROFILE_DIRECT_FS_HOTPLUG      6

#ifndef TINYUSB_DIAG_PROFILE
#define TINYUSB_DIAG_PROFILE TINYUSB_PROFILE_LEGACY
#endif
