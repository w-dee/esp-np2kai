#include "np2_memory_probe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"

/* cpumem.c owns the NP2 CPU memory buffer. It must remain external BSS. */
extern unsigned char mem[];

#define NP2_MEMORY_ALLOC_SIZE (13U * 1024U * 1024U + 16U)

static void np2_memory_probe_report_heap(const char *phase)
{
    printf("NP2MEM_HEAP phase=%s psram_size=%lu free_spiram=%lu "
           "largest_spiram=%lu free_internal=%lu largest_internal=%lu "
           "free_default=%lu largest_default=%lu\n",
           phase,
           (unsigned long)esp_psram_get_size(),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static esp_err_t np2_memory_probe_fail(const char *reason)
{
    printf("NP2MEM_RESULT=FAIL reason=%s\n", reason);
    fflush(stdout);
    return ESP_FAIL;
}

esp_err_t np2_memory_probe_run(void)
{
    if (!esp_psram_is_initialized()) {
        return np2_memory_probe_fail("psram_not_initialized");
    }

    np2_memory_probe_report_heap("before");

    if (!esp_ptr_external_ram(mem)) {
        return np2_memory_probe_fail("np2_mem_not_external");
    }

    const size_t largest_external =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largest_external < NP2_MEMORY_ALLOC_SIZE) {
        return np2_memory_probe_fail("external_block_too_small");
    }

    void *probe = malloc(NP2_MEMORY_ALLOC_SIZE);
    if (probe == NULL) {
        return np2_memory_probe_fail("malloc_null");
    }
    if (!esp_ptr_external_ram(probe)) {
        free(probe);
        return np2_memory_probe_fail("malloc_not_external");
    }

    printf("NP2MEM_MALLOC requested=%lu ptr_external=1\n",
           (unsigned long)NP2_MEMORY_ALLOC_SIZE);
    free(probe);
    np2_memory_probe_report_heap("after_free");
    printf("NP2MEM_RESULT=PASS\n");
    fflush(stdout);
    return ESP_OK;
}
