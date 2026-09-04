#ifndef P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE_H
#define P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Focused ESP-EMU harness only; ordinary emulator composition never calls
 * this entry point. */
esp_err_t p4_nano_audio86_live_service_run_profile(void);

#ifdef __cplusplus
}
#endif

#endif /* P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE_H */
