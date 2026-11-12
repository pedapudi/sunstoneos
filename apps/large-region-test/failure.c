/* Read the declared card file completely before exercising application failure. */
#include "large_region-app.h"
#include "FreeRTOS.h"
#include "task.h"

#define SOURCE_BYTES (UINT64_C(512) << 20)
#define REGION_BYTES (UINT64_C(1) << 30)

void large_region_main(const struct ss_grant *grant)
{
    (void)grant;
    while (!ss_clock_ready()) {
        if (ss_live() < 0) ss_fail(0x781);
        ss_progress();
        vTaskDelay(1);
    }
    int ready;
    do {
        ss_progress();
        ready = ss_large_wait(LARGE_REGION_LARGE_WEIGHTS, 1000000);
    } while (ready == 0 && ss_live() >= 0);
    struct ss_large_status status;
    if (ss_large_status(LARGE_REGION_LARGE_WEIGHTS, &status) != 0) ss_fail(0x782);
    if (ready != 1) ss_fail_value(0x781, status.error);
    if (status.state != SS_LARGE_READY || status.error ||
        status.loaded != SOURCE_BYTES || status.total != SOURCE_BYTES) ss_fail(0x782);
    size_t bytes;
    if (!ss_large_base(LARGE_REGION_LARGE_WEIGHTS, &bytes) || bytes != REGION_BYTES) ss_fail(0x783);
    /* READY requires the loader's SHA-256 to match the immutable manifest. */
    ss_fail_value(0x780, (uint32_t)status.loaded);
}
