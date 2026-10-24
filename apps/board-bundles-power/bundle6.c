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
 * of each domain. It also records the owner adapter's mapping result and the
 * PAR_EL1 translation of the domain-1 block, which the adapter requires to
 * be Device-nGnRnE and outer-shareable before bundle 7 writes it.
 *
 * The summary (TWI divisor and bit time, supply bits, step distribution
 * against the wire time, the provisional bound, re-spins, access cost,
 * mapping, adoption reads) and the outcome line print first; the detail
 * lines (each service step that completed a bus phase, each snapshot, each
 * owner access) follow while they fit in the log. No GIC register is read
 * for the record.
 *
 * The wire time of a step counts two bit times per START, repeated START or
 * STOP, and per byte the byte time of bb_byte_ns(): nine bit times, or the
 * measured byte when that is longer and at most 110% of nine bit times.
 * Success is status 0 with "COMPLETE board bundle 6": both supplies verified,
 * every snapshot 0xff9e (enabled, 800 mV), 34 snapshots counted and the
 * provisional bound of docs/designs/pmic-access.md met (trace.h,
 * bb_steps_evaluate). Re-spins are reported, not bounded. A missed bound, or
 * a measured byte that is missing or above 110%, ends with status 1 after
 * "FAILED board bundle 6: provisional step bound", a controlled outcome. A
 * supply, snapshot or record mismatch is an uncontrolled failure.
 *
 * Before its first snapshot the client arms the wire-time spin: the service
 * times one byte phase from its CNTR write to INT_FLAG set and leaves that
 * step out of the step records. Every S_TWI0 write of the snapshots is timed
 * in place. At the park, the service, which is the provider task, runs the
 * access-cost windows of trace.h while idle: CNTR
 * reads with the DSB SY helper, plain and discarded, and plain with each
 * address depending on the previous value; the adapter's time and read
 * operations; the task check that the adapter no longer makes per access;
 * and the TWI observer. A CNTR read has no side effect. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_power_authority.h"

#define SNAPSHOTS 32u
/* Enable byte 0xff and selector 0x9e: DCDC2 and DCDC4 enabled at 800 mV, as
 * port 3-3 read them on 2026-10-24. */
#define SNAPSHOT_EXPECTED 0xff9eu
static struct a733_pmic_snapshot snapshots[SNAPSHOTS];

static void snapshot(unsigned i)
{
    const enum a733_rail rail = i & 1 ? A733_RAIL_DCDC4 : A733_RAIL_DCDC2;
    struct a733_pmic_request request;
    enum a733_pmic_status r;
    while ((r = a733_pmic_submit(&bb_power.pmic, BB_POWER_CLIENT_OWNER, &bb_power.token, rail, bb_power.deadline,
                                 &request)) == A733_PMIC_BUSY)
        bb_power_pause("board bundle 6: snapshot submit deadline");
    if (r != A733_PMIC_PENDING)
        bb_power_fail("board bundle 6: snapshot submit");
    while ((r = a733_pmic_result(&bb_power.pmic, &request, &snapshots[i])) == A733_PMIC_PENDING || r == A733_PMIC_BUSY)
        bb_power_pause("board bundle 6: snapshot result deadline");
    if (r != A733_PMIC_COMPLETE)
        bb_power_fail("board bundle 6: snapshot result");
    while ((r = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        bb_power_pause("board bundle 6: snapshot reap deadline");
    if (r != A733_PMIC_COMPLETE)
        bb_power_fail("board bundle 6: snapshot reap");
}
static uint32_t enable_selector(const struct a733_pmic_snapshot *s)
{ return (uint32_t)s->enable << 8 | s->selector; }

static void client(void *unused)
{
    (void)unused;
    platform_puts(BB6_BEGIN);
    bb_power_wait_ready();
    struct bb6_summary s = {0};
    s.state = a733_power_authority_load(&bb_power.rails.authority);
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC2) | A733_RAIL_BIT(A733_RAIL_DCDC4));
    atomic_store_explicit(&bb_power.wire_armed, 1, memory_order_release);
    for (unsigned i = 0; i < SNAPSHOTS; ++i)
        snapshot(i);
    if (!bb_power_read(0x070101ac, &s.ppu) || !bb_power_read(0x020025c0, &s.ahb) || !bb_power_read(0x020025e0, &s.mbus))
        bb_power_fail("board bundle 6: domain-level read");
    static const unsigned domains[3] = {1, 4, 5};
    for (unsigned i = 0; i < 3; ++i) {
        const uintptr_t base = A733_POWER_PCK_DOMAIN(domains[i]);
        if ((s.ppu & 1) && (!bb_power_read(base, &s.policy[i]) || !bb_power_read(base + 8, &s.status[i])))
            bb_power_fail("board bundle 6: adoption read");
        s.check[i] = a733_power_domain_check(&bb_power.domains, domains[i], NULL);
    }
    struct aarch64_translation t = {0};
    s.mapped = bb_power.domains_native.mapped;
    if (aarch64_device_translate(A733_POWER_PCK_DOMAIN(1), 1, &t)) {
        s.par_attributes = t.attributes;
        s.par_shareability = t.shareability;
        s.par_physical = t.physical;
    }
    bb_power.cost_at_park = 1;
    bb_power_park();
    s.cost = bb_power.cost;
    s.wire_excluded = bb_power.wire_excluded;
    for (unsigned i = 0; i < bb_power.stepped; ++i)
        s.respins += bb_power.steps[i].respins;

    s.divisor = bb_power.pmic.divider;
    s.bit_ns = bb_power.pmic.bit_ns;
    s.dcdc2 = !!(s.state & A733_POWER_DCDC2_VERIFIED);
    s.dcdc4 = !!(s.state & A733_POWER_DCDC4_VERIFIED);
    s.unchanged = 1;
    for (unsigned i = 0; i < SNAPSHOTS; ++i)
        s.unchanged &= enable_selector(&snapshots[i]) == enable_selector(&snapshots[i & 1]) &&
            snapshots[i].enabled == 1 && snapshots[i].decoded == 1 && snapshots[i].programmed_uv == 800000;
    s.dcdc2_first = enable_selector(&snapshots[0]);
    s.dcdc4_first = enable_selector(&snapshots[1]);
    s.steps = bb_power.stepped;
    s.dropped = bb_power.steps_dropped;
    s.twi_reads = bb_power.twi.reads;
    s.enable_seen = bb_power.twi.enable_seen;
    s.stop_respins = bb_power.twi.stop_respins;
    s.snapshot_respins_max = bb_power.twi.snapshot_respins_max;
    s.snapshots = bb_power.twi.snapshots;
    s.byte_ok = bb_byte_ns(s.bit_ns, s.cost.wire_seen, s.cost.wire_clear, &s.byte_ns);
    bb_steps_evaluate(bb_power.steps, bb_power.stepped, s.bit_ns, s.byte_ns, &s.bins, &s.bound);
    s.bound.pass &= s.byte_ok;
    struct bb_line lines[BB6_SUMMARY_LINES];
    const unsigned n = bb6_summary_lines(&s, lines);
    for (unsigned i = 0; i < n; ++i)
        bb_power_print(&lines[i]);
    if (!s.dcdc2 || !s.dcdc4 || !s.unchanged || s.dcdc2_first != SNAPSHOT_EXPECTED ||
        s.dcdc4_first != SNAPSHOT_EXPECTED)
        bb_power_fail("board bundle 6: supply verification or snapshot mismatch");
    /* The two verification snapshots and the 32 client snapshots each end in
     * six STOPs. Another count, or no recorded step, means the record itself
     * is wrong. */
    if (s.snapshots != SNAPSHOTS + 2 || !s.steps)
        bb_power_fail("board bundle 6: snapshot count or step record");
    platform_puts(s.bound.pass ? BB6_COMPLETE : BB6_BOUND_MISSED);
    for (unsigned i = 0; i < bb_power.stepped; ++i) {
        bb_step_line(&bb_power.steps[i], &lines[0]);
        bb_power_detail(&lines[0]);
    }
    for (unsigned i = 0; i < SNAPSHOTS; ++i) {
        bb_snapshot_line(i, enable_selector(&snapshots[i]), &lines[0]);
        bb_power_detail(&lines[0]);
    }
    bb_power_detail_accesses();
    bb_power_details_end();
    platform_exit(s.bound.pass ? 0 : 1);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    if (!bb_power_init(client, "bundle6"))
        platform_panic("board bundle 6: power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
