#pragma once

#include <stdbool.h>
#include <stdint.h>

/* P4 keeps the complete continuous benchmark pccore lifetime with ample
 * headroom over the historical 151-call fixture.  This storage is supplied
 * by the benchmark owner; normal runner users only carry a pointer. */
#define NP2VIDEO_PCCORE_TRACE_CAPACITY 512U

typedef struct {
    uint64_t start_us;
    uint64_t end_us;
    uint32_t call_index;
} np2video_pccore_interval;

typedef struct {
    np2video_pccore_interval intervals[NP2VIDEO_PCCORE_TRACE_CAPACITY];
    uint32_t stored;
    bool overflow;
} np2video_pccore_trace;
