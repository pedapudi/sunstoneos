/* SPDX-License-Identifier: Apache-2.0 */
/* Returning DCDC2 observation through the qualified PMIC provider. The host
 * classifies the raw snapshot after a successful reap and service park. No
 * voltage value is a prerequisite to returning a valid observation. */
#include "power.h"
#include "platform.h"
#include "a733_range.h"

static void client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    uint64_t settle;
    if (!a733_deadline_after(bb_power_now(), A733_POWER_SUPPLY_SETTLE_US, &settle) ||
        settle >= bb_power.deadline)
        bb_power_fail("DCDC2 settle deadline");
    while (bb_power_now() < settle)
        bb_power_pause("DCDC2 settle deadline");
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC2));
    struct a733_pmic_request request = {0};
    struct a733_pmic_snapshot snapshot = {0};
    enum a733_pmic_status result;
    while ((result = a733_pmic_submit(&bb_power.pmic, BB_POWER_CLIENT_OWNER, &bb_power.token,
                                     A733_RAIL_DCDC2, bb_power.deadline, &request)) == A733_PMIC_BUSY)
        bb_power_pause("DCDC2 submit deadline");
    if (result != A733_PMIC_PENDING)
        bb_power_fail("DCDC2 submit");
    while ((result = a733_pmic_result(&bb_power.pmic, &request, &snapshot)) == A733_PMIC_PENDING ||
           result == A733_PMIC_BUSY)
        bb_power_pause("DCDC2 result deadline");
    if (result != A733_PMIC_COMPLETE)
        bb_power_fail("DCDC2 result");
    while ((result = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        bb_power_pause("DCDC2 reap deadline");
    if (result != A733_PMIC_COMPLETE)
        bb_power_fail("DCDC2 reap");
    bb_power_park();

    struct bb_line line;
    bb_line_start(&line, "DCDC2 raw");
    bb_line_hex(&line, "page_before", snapshot.page_before);
    bb_line_hex(&line, "chip", snapshot.chip);
    bb_line_hex(&line, "revision", snapshot.revision);
    bb_line_end(&line);
    bb_power_print(&line);
    bb_line_start(&line, "DCDC2 raw");
    bb_line_hex(&line, "enable", snapshot.enable);
    bb_line_hex(&line, "selector", snapshot.selector);
    bb_line_hex(&line, "page_after", snapshot.page_after);
    bb_line_end(&line);
    bb_power_print(&line);
    bb_line_start(&line, "DCDC2 decoded");
    bb_line_dec(&line, "enabled", snapshot.enabled);
    bb_line_dec(&line, "decoded", snapshot.decoded);
    bb_line_dec(&line, "programmed_uv", snapshot.programmed_uv);
    bb_line_end(&line);
    bb_power_print(&line);
    platform_puts("COMPLETE DCDC2 snapshot: reaped and provider parked\n");
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    if (!bb_power_snapshot_init(client, "dcdc2-read"))
        platform_panic("DCDC2 power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
