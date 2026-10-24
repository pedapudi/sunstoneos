/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 6 of docs/designs/board-bundles-next.md: the PMIC service task and
 * the power-domain owner, read-only.
 *
 * The service task verifies DCDC2 and DCDC4 once, as every image with a row
 * does; the row is never requested, so no PPU write is possible. The client
 * task then takes 32 snapshots, DCDC2 and DCDC4 in turn, each submitted,
 * polled once per tick and reaped, and makes the adoption reads of domains
 * 1, 4 and 5: R_CCU 0x1ac, CCU 0x5c0 and 0x5e0, then the policy and status
 * words only while the PPU clock reads open, and the owner's read-only check
 * of each domain. The record holds the inherited S_TWI0 divisor and its bit
 * time, each snapshot, and for every service step that completed a bus phase
 * its span, the scheduler ticks inside it and the TWI interrupt flag at each
 * byte phase, read from CNTR with the interrupt disabled. No GIC register is
 * read for the record. Success is status 0 with "COMPLETE board bundle 6". */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"

#define SNAPSHOTS 32u
static struct a733_pmic_snapshot snapshots[SNAPSHOTS];

static void wait(const char *what)
{
    if (bb_power_now() >= bb_power.deadline)
        bb_power_fail(what);
    vTaskDelay(1);
}

static void snapshot(unsigned i)
{
    const enum a733_rail rail = i & 1 ? A733_RAIL_DCDC4 : A733_RAIL_DCDC2;
    struct a733_pmic_request request;
    enum a733_pmic_status r;
    while ((r = a733_pmic_submit(&bb_power.pmic, BB_POWER_CLIENT_OWNER, &bb_power.token, rail, bb_power.deadline,
                                 &request)) == A733_PMIC_BUSY)
        wait("board bundle 6: snapshot submit deadline");
    if (r != A733_PMIC_PENDING)
        bb_power_fail("board bundle 6: snapshot submit");
    while ((r = a733_pmic_result(&bb_power.pmic, &request, &snapshots[i])) == A733_PMIC_PENDING || r == A733_PMIC_BUSY)
        wait("board bundle 6: snapshot result deadline");
    if (r != A733_PMIC_COMPLETE)
        bb_power_fail("board bundle 6: snapshot result");
    while ((r = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        wait("board bundle 6: snapshot reap deadline");
    if (r != A733_PMIC_COMPLETE)
        bb_power_fail("board bundle 6: snapshot reap");
}

static void client(void *unused)
{
    (void)unused;
    platform_puts("BEGIN board bundle 6: PMIC service and power-domain owner, read-only\n");
    bb_power_wait_ready();
    const unsigned state = a733_power_authority_load(&bb_power.rails.authority);
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC2) | A733_RAIL_BIT(A733_RAIL_DCDC4));
    for (unsigned i = 0; i < SNAPSHOTS; ++i)
        snapshot(i);
    uint32_t ppu = 0, ahb = 0, mbus = 0;
    if (!bb_power_read(0x070101ac, &ppu) || !bb_power_read(0x020025c0, &ahb) || !bb_power_read(0x020025e0, &mbus))
        bb_power_fail("board bundle 6: domain-level read");
    static const unsigned domains[3] = {1, 4, 5};
    uint32_t policy[3] = {0}, status[3] = {0};
    int check[3];
    for (unsigned i = 0; i < 3; ++i) {
        const uintptr_t base = A733_POWER_PCK_DOMAIN(domains[i]);
        if ((ppu & 1) && (!bb_power_read(base, &policy[i]) || !bb_power_read(base + 8, &status[i])))
            bb_power_fail("board bundle 6: adoption read");
        check[i] = a733_power_domain_check(&bb_power.domains, domains[i]);
    }
    bb_power_park();

    bb_power_put_hex("TWI_DIVISOR=", bb_power.pmic.divider);
    bb_power_put("TWI_BIT_NS=", bb_power.pmic.bit_ns);
    bb_power_put("TWI_RATE_HZ=", bb_power.pmic.bit_ns ? 1000000000u / bb_power.pmic.bit_ns : 0);
    bb_power_put_hex("POWER_STATE_WORD=", state);
    bb_power_put("DCDC2_VERIFIED=", !!(state & A733_POWER_DCDC2_VERIFIED));
    bb_power_put("DCDC4_VERIFIED=", !!(state & A733_POWER_DCDC4_VERIFIED));
    unsigned unchanged = 1;
    for (unsigned i = 0; i < SNAPSHOTS; ++i) {
        const struct a733_pmic_snapshot *s = &snapshots[i];
        platform_puts(i & 1 ? "SNAPSHOT DCDC4" : "SNAPSHOT DCDC2");
        bb_power_put_hex(" enable_selector=", (uint32_t)s->enable << 8 | s->selector);
        unchanged &= s->enable == snapshots[i & 1].enable && s->selector == snapshots[i & 1].selector &&
            s->enabled == 1 && s->decoded == 1 && s->programmed_uv == 800000;
    }
    bb_power_put("DCDC2_DCDC4_800MV_UNCHANGED=", unchanged);
    bb_power_report_steps();
    bb_power_put_hex("R_CCU_PPU_CLOCK=", ppu);
    bb_power_put_hex("CCU_0x5c0=", ahb);
    bb_power_put_hex("CCU_0x5e0=", mbus);
    for (unsigned i = 0; i < 3; ++i) {
        bb_power_put("DOMAIN=", domains[i]);
        bb_power_put_hex("  policy=", policy[i]);
        bb_power_put_hex("  status=", status[i]);
        bb_power_put("  owner_check=", (uint64_t)check[i]);
    }
    bb_power_report_accesses();
    platform_puts("COMPLETE board bundle 6: PMIC service and domain adoption reads\n");
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
    const uint64_t start = counter() / 24;
    /* The service session ends 1.8 s after start, inside the monitor's 2 s. */
    if (!bb_power_init(start + 1800000, client, "bundle6"))
        platform_panic("board bundle 6: power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
