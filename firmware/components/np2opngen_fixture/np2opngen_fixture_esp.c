#include "np2opngen_fixture.h"
#include "np2opngen_fixture_esp.h"

#include "esp_timer.h"

static uint64_t np2opngen_fixture_esp_clock(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

esp_err_t np2opngen_fixture_run_esp(void)
{
    return np2opngen_fixture_run(np2opngen_fixture_esp_clock, 0) == 0
               ? ESP_OK
               : ESP_FAIL;
}
