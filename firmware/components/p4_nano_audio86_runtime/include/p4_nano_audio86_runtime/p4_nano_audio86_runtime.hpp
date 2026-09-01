#pragma once

#include "esp_err.h"

namespace p4_nano_audio86_runtime {

/* TEST / ISOLATED PROFILE ONLY.  This does not bind the real guest or output
 * audio; it validates the production transport and FreeRTOS task foundation. */
esp_err_t run();

} // namespace p4_nano_audio86_runtime
