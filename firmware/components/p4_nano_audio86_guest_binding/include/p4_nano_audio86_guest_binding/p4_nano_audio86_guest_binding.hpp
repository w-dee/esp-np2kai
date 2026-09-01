#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace np2runtime {
class Runtime;
}

namespace p4_nano_audio86_guest_binding {

/* Runs only on the existing p4_nano_pc98 owner task.  It never creates a
 * producer task: Core 1 guest ownership remains with that task. */
esp_err_t run_on_pc98_task(TaskHandle_t producer,
                           np2runtime::Runtime *lifecycle_runtime) noexcept;

} // namespace p4_nano_audio86_guest_binding
