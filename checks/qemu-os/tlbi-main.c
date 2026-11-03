/* Test application of checks/qemu-os-boot.py for the invalidation task of apps/os/tlbi-issuer.c: after 200 ms it reports what the
 * task did. Result {tag, wakes, invalidations, core, largest ticks}. */
#include "tlbi_test-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>

struct os_tlbi_stats { atomic_uint wakes, issued, core; atomic_uint_fast64_t max_ticks, total_ticks; };
extern struct os_tlbi_stats os_tlbi_stats;

void tlbi_test_main(const struct ss_grant *grant)
{
    (void)grant;
    vTaskDelay(pdMS_TO_TICKS(200));
    const uint64_t out[5] = {UINT64_C(0x54424c49), atomic_load(&os_tlbi_stats.wakes), atomic_load(&os_tlbi_stats.issued),
                             atomic_load(&os_tlbi_stats.core), atomic_load(&os_tlbi_stats.max_ticks)};
    ss_finish(out, sizeof out);
}
