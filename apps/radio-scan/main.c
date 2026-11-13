/* radio-scan: the application of the scanning radio image. It waits until the radio station has scanned,
 * or failed, and finishes with a short summary. The OS then shuts the module down with a readback at each
 * step, publishes the full scan result (apps/os/radio_scan.c) and returns to the host.
 * It requires the radio device, which grants it the OS radio station. */
#include "radio_scan-app.h"

struct radio_scan_result {
    uint32_t stage, failed, done, reserved;
};
_Static_assert(sizeof(struct radio_scan_result) == 16, "radio-scan result is the manifest's output_limit");

void radio_scan_main(const struct ss_grant *grant)
{
    if(!grant||!grant->finish_by_us)ss_fail(2);
    const uint64_t until=grant->finish_by_us;
    struct ss_radio_status status;
    for (;;) {
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_radio_status(&status);
        const uint64_t now=ss_counter_ticks()/24;
        if(now>=until)ss_fail(2);
        if (status.done) break;
        ss_wait_until(until);
    }
#ifdef SUNSTONE_RADIO_SCAN_FAILURE_TEST
    /* The public radio-status ABI stores the completed scan as stage 7.
     * Inject only after that successful observation, before exporting output. */
    if(!status.failed&&status.stage==7u)ss_fail(UINT32_C(0x524107));
#endif
    const struct radio_scan_result result = {status.stage, status.failed, status.done, 0};
    ss_finish(&result, sizeof(result));
}
