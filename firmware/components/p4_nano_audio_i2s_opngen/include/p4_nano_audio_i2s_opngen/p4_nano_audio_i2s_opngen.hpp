#pragma once

#include "esp_err.h"

namespace p4_nano_audio_i2s_opngen {

/* Run the A3.4 OPNGEN -> finite PCM ring -> real I2S sink profile. */
esp_err_t run();

} // namespace p4_nano_audio_i2s_opngen
