/* radio-scan: the application of the scanning radio image. It waits until the radio station has scanned,
 * or failed, and finishes with a short summary. The OS then shuts the module down with a readback at each
 * step, publishes the full scan result (apps/os/radio_scan.c) and returns to the host.
 * It uses no device: the radio station is an OS service. */
#include "radio_scan-app.h"

struct radio_scan_result {
    uint32_t stage, failed, done, reserved;
};
_Static_assert(sizeof(struct radio_scan_result) == 16, "radio-scan result is the manifest's output_limit");

void radio_scan_main(const struct ss_grant *grant)
{
    (void)grant;
    struct ss_radio_status status;
    for (;;) {
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_radio_status(&status);
        if (status.done) break;
        ss_usb_wait(1000);
    }
    const struct radio_scan_result result = {status.stage, status.failed, status.done, 0};
    ss_finish(&result, sizeof(result));
}
