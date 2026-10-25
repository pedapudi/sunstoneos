/* SPDX-License-Identifier: Apache-2.0 */
/* Board bundle 15: read-only DCDC3 and DCDC5 snapshots through the qualified
 * PMIC provider, for the CPU supply part of the A76 operating-point record
 * (docs/designs/cpu-a76-dvfs.md, B1). DCDC3 supplies CPU-B; DCDC5 supplies
 * CPU-L and the DSU. The consumer has no domain row, so the service makes no
 * DCDC2 or DCDC4 verification and no PPU access. A client takes one shared
 * token of both rails, submits one snapshot of each in turn, reaps it, parks
 * the service and prints the raw and decoded values. No voltage, clock or
 * PMIC register is written. */
#include "power.h"
#include "platform.h"
#include "a733_range.h"

static void snapshot(enum a733_rail rail, const char *label)
{
    struct a733_pmic_request request = {0};
    struct a733_pmic_snapshot s = {0};
    enum a733_pmic_status result;
    while ((result = a733_pmic_submit(&bb_power.pmic, BB_POWER_CLIENT_OWNER, &bb_power.token, rail,
                                     bb_power.deadline, &request)) == A733_PMIC_BUSY)
        bb_power_pause("snapshot submit deadline");
    if (result != A733_PMIC_PENDING)
        bb_power_fail("snapshot submit");
    while ((result = a733_pmic_result(&bb_power.pmic, &request, &s)) == A733_PMIC_PENDING ||
           result == A733_PMIC_BUSY)
        bb_power_pause("snapshot result deadline");
    if (result != A733_PMIC_COMPLETE)
        bb_power_fail("snapshot result");
    while ((result = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        bb_power_pause("snapshot reap deadline");
    if (result != A733_PMIC_COMPLETE)
        bb_power_fail("snapshot reap");
    struct bb_line line;
    bb_line_start(&line, label);
    bb_line_hex(&line, "page_before", s.page_before);
    bb_line_hex(&line, "chip", s.chip);
    bb_line_hex(&line, "revision", s.revision);
    bb_line_hex(&line, "page_after", s.page_after);
    bb_line_end(&line);
    bb_power_print(&line);
    bb_line_start(&line, label);
    bb_line_hex(&line, "enable", s.enable);
    bb_line_hex(&line, "selector", s.selector);
    bb_line_dec(&line, "enabled", s.enabled);
    bb_line_dec(&line, "decoded", s.decoded);
    bb_line_dec(&line, "programmed_uv", s.programmed_uv);
    bb_line_end(&line);
    bb_power_print(&line);
}

static void client(void *unused)
{
    (void)unused;
    platform_puts("BEGIN board bundle 15: DCDC3 and DCDC5 snapshots, read-only\n");
    bb_power_wait_ready();
    uint64_t settle;
    if (!a733_deadline_after(bb_power_now(), A733_POWER_SUPPLY_SETTLE_US, &settle) ||
        settle >= bb_power.deadline)
        bb_power_fail("settle deadline");
    while (bb_power_now() < settle)
        bb_power_pause("settle deadline");
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC3) | A733_RAIL_BIT(A733_RAIL_DCDC5));
    snapshot(A733_RAIL_DCDC3, "DCDC3");
    snapshot(A733_RAIL_DCDC5, "DCDC5");
    bb_power_park();
    platform_puts("COMPLETE board bundle 15: DCDC3 and DCDC5 snapshots, reaped and provider parked\n");
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
    if (!bb_power_snapshot_init(client, "cpu-supply"))
        platform_panic("bundle 15 power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
