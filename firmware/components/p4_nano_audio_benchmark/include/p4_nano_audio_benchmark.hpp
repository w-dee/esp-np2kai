#ifndef P4_NANO_AUDIO_BENCHMARK_HPP
#define P4_NANO_AUDIO_BENCHMARK_HPP

#include "esp_err.h"

namespace p4_nano_audio_benchmark {

/* Run the A1 audio-only service-demand benchmark.  This profile owns only
 * the OPNGEN fixture, SPSC producer/worker tasks, and a CRC/SHA sink; it does
 * not start the production runtime, display, I2S, codec, storage, or network
 * paths. */
esp_err_t run();

} // namespace p4_nano_audio_benchmark

#endif
