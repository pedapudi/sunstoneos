/* SPDX-License-Identifier: Apache-2.0
 * Vendor-state controller for the vendor-state diagnostic only. It is
 * apps/gpu-power-id/vendor-clk-power.c, itself drivers/gpu/a733_gpu_power.c
 * with the vendor clock and reset order, with these changes:
 * - ON enables PLL_GPU0 and runs the GPU core from it at 1008 MHz, in the
 *   order of the vendor's sunxi_platform_init (sunxi_platform.c:899-922 at
 *   kernel 2ac08e8c7): the PLL enable of ccu_pll_gate_helper_enable
 *   (ccu_gate.c:157-203) with its lock wait (ccu_common.c:47-61), then the
 *   GPU bus reset, bus gate and core clock gate on its reset source 0 (the
 *   PLL) with the update bit (sunxi_platform.c:202-220), then the set-rate
 *   sequence of ccu_nm_set_rate (ccu_nm.c:250-345) to 1008 MHz;
 * - every PLL_GPU0 write is read back, and a word that did not take fails
 *   with VENDOR_STATE_ERROR_PLL; a lock wait that ends unlocked fails with
 *   VENDOR_STATE_ERROR_LOCK, and a 0xb20 update bit that does not clear with
 *   VENDOR_STATE_ERROR_UPDATE (vendor-state.h);
 * - admission requires PLL_GPU0 disabled at the inherited word and the GPU
 *   clock word at its reset source and divider, and the resources to report
 *   DCDC4 at 960 mV, the vendor's operating-point voltage.
 * Nothing else differs, and no default image compiles this file.
 * apps/gpu-power-id/prepare.py pins the exact table.
 */
#include "a733_gpu_power.h"
#include "vendor-state.h"
#include "a733_range.h"
#include <stddef.h>

#define BIT(n) (UINT32_C(1) << (n))
/* apps/gpu-power-id/prepare.py reads the step tables of a linked image and
 * relies on the values of DOMAIN and of the CORE bank. NOTE and SETTLE carry
 * their enum a733_gpu_core_note slot in value. */
enum kind { CHECK, UPDATE, DELAY, DOMAIN, CORE_POLICY, CORE_STATUS, CORE_CONFIG, POLL,
            NOTE, SETTLE, PLL, LOCK, UPDATED };
/* VENDOR-STATE VARIANT: PLL is a CHECK of a PLL_GPU0 write that fails with
 * VENDOR_STATE_ERROR_PLL. LOCK reads the word until mask reads set three
 * times in a row, 3 us apart and at most value reads, as
 * ccu_helper_wait_for_lock does. UPDATED reads the word until mask reads
 * clear, at most value reads; value is the read bound. */
struct step { enum kind kind; enum a733_gpu_power_bank bank;
    uint32_t offset, mask, value; };
/* The GPU_TOP domain is PCK domain 5, which the power-domain owner powers
 * through the GPU's row. The MBUS gates, PPU clock, closed clock gate and
 * asserted reset are checked here before the request, and again by the owner
 * at admission. The selected BXM rgx_sunxi adapter directly controls CORE6;
 * its device-tree node is commented out. CORE configuration/policy writes
 * follow sunxi_platform.c:70-102,911-922 at kernel 2ac08e8c7. DEN0051E5.2.8
 * requires settled static policy before changing PWCR device request enables.
 * After power-up CORE6 reads policy 8, status 0 and the one-Q-Channel reset
 * PWCR: its default ON policy has a transition outstanding that the GPU's
 * Q-Channel cannot accept while the GPU is gated and in reset. The BXM
 * adapter writes PWCR 0 and policy 8 in that state, before any GPU clock,
 * and this controller admits it as the one unsettled exception. NOTE and
 * SETTLE steps record CORE6 words for diagnosis and require no particular
 * value; a SETTLE status other than 0 or 8 still fails.
 * VENDOR-STATE VARIANT: GPU local clock selects PLL_GPU0 / 1 at 1008 MHz.
 */
static const struct step on_steps[] = {
    {CHECK, A733_GPU_CCU, 0x05c0, BIT(7), BIT(7)},
    {CHECK, A733_GPU_CCU, 0x05e0, BIT(16), BIT(16)},
    {CHECK, A733_GPU_R_CCU, 0x01ac, BIT(0), BIT(0)},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8f00000f, 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), 0},
    {CHECK, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY},
    {DOMAIN, A733_GPU_CCU, 0, 0, 1},
    {CORE_POLICY, A733_GPU_CORE, 0, 0, 0},
    {CORE_STATUS, A733_GPU_CORE, 8, 0, 0},
    {NOTE, A733_GPU_CORE, 0xfb0, 0, A733_GPU_NOTE_IDR0},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR},
    {CORE_CONFIG, A733_GPU_CORE, 0x20, 0, 0},
    /* VENDOR-STATE VARIANT. The CORE6 PWCR 0 and policy 8 writes are those
     * of the vendor-clk controller, whose delay-row drop it keeps. The GPU
     * clock then follows the vendor, each PLL_GPU0 write read back with the
     * read-only lock bit 28 excluded:
     * - PLL enable (ccu_gate.c:157-203): output gate bit 27 cleared, LDO bit
     *   30, enable bit 31 and lock enable bit 29 set, the lock wait, 20 us
     *   (ccu_common.c:61), then the output gate set: 840 MHz from the
     *   factors boot0 left (N 70, P0 2);
     * - sunxi_enable_clks_wrap (sunxi_platform.c:202-220): bus reset
     *   deasserted, bus gate opened, core clock gate opened on its reset
     *   source 0 (PLL_GPU0) and divider 1 with the update bit, then the word
     *   written again with the update bit; each update bit must clear;
     * - ccu_nm_set_rate to 1008 MHz (ccu_nm.c:250-345, AW_STANDARD_CCU):
     *   output gate cleared, N 84 and P0 2 written, lock enable cleared and
     *   set, the lock wait, 20 us, output gate set: 0xe9105300. */
    {UPDATE, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {CHECK, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_PWCR},
    {UPDATE, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {CHECK, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_POLICY},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(27), 0},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(30)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(31)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY | BIT(31)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(29)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY | BIT(31) | BIT(29)},
    {LOCK, A733_GPU_CCU, 0x00e0, BIT(28), 100},
    {DELAY, A733_GPU_CCU, 0, 0, 20},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(27)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY | BIT(31) | BIT(29) | BIT(27)},
    {UPDATE, A733_GPU_CCU, 0x0b24, 0, BIT(16)},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), BIT(16)},
    {UPDATE, A733_GPU_CCU, 0x0b24, 0, BIT(0)},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), BIT(16) | BIT(0)},
    {UPDATE, A733_GPU_CCU, 0x0b20, 0, BIT(31) | BIT(27)},
    {UPDATED, A733_GPU_CCU, 0x0b20, BIT(27), 100},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8f00000f, BIT(31)},
    {UPDATE, A733_GPU_CCU, 0x0b20, 0, BIT(27)},
    {UPDATED, A733_GPU_CCU, 0x0b20, BIT(27), 100},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8f00000f, BIT(31)},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(27), 0},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_ENTRY | BIT(31) | BIT(29)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0x0070ff00, 0x00105300},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), (VENDOR_STATE_PLL_1008 & ~BIT(27))},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(29), 0},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), (VENDOR_STATE_PLL_1008 & ~BIT(27)) & ~BIT(29)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(29)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), (VENDOR_STATE_PLL_1008 & ~BIT(27))},
    {LOCK, A733_GPU_CCU, 0x00e0, BIT(28), 100},
    {DELAY, A733_GPU_CCU, 0, 0, 20},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(27)},
    {PLL, A733_GPU_CCU, 0x00e0, ~BIT(28), VENDOR_STATE_PLL_1008},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR_LATE},
    {POLL, A733_GPU_CORE, 8, UINT32_MAX, 8},
};
static const struct step off_steps[] = {
    /* Complete firmware stop is the caller's prerequisite. The selected
     * BXM Post callback commands CORE OFF before TOP and clock teardown.
     * Keep TOP/local clocks available while adding bounded acknowledgement. */
    {UPDATE, A733_GPU_CORE, 0, UINT32_MAX, 0},
    {CHECK, A733_GPU_CORE, 0, UINT32_MAX, 0},
    {POLL, A733_GPU_CORE, 8, UINT32_MAX, 0},
    {UPDATE, A733_GPU_CCU, 0x0b24, BIT(16), 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16), 0},
    {UPDATE, A733_GPU_CCU, 0x0b24, BIT(0), 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), 0},
    {UPDATE, A733_GPU_CCU, 0x0b20, BIT(31), BIT(27)},
    {CHECK, A733_GPU_CCU, 0x0b20, BIT(31), 0},
    {DOMAIN, A733_GPU_CCU, 0, 0, 0},
};
struct vendor_state_clock vendor_state_clock;
static enum a733_gpu_power_result fail(struct a733_gpu_power *s,
                                       enum a733_gpu_power_error error)
{
    s->error = error;
    s->phase = A733_GPU_POWER_FAILED;
    return A733_GPU_POWER_FAULT;
}
/* The power-up state: default ON policy, its transition outstanding, and the
 * reset PWCR, so nothing has written CORE6. */
static int core_power_up(const struct a733_gpu_power *s)
{
    return s->core_policy == 8 && s->core_status == 0 && s->core_config == A733_GPU_CORE_RESET_PWCR;
}
/* Settled static OFF or ON, with whole words, or the power-up state. */
static int core_admitted(const struct a733_gpu_power *s)
{
    return ((s->core_policy == 0 || s->core_policy == 8) && s->core_status == s->core_policy) ||
        core_power_up(s);
}
static int clock_read(struct a733_gpu_power *s)
{
    uint64_t now;
    if (!s->ops.now(s->context, &now)) { fail(s, A733_GPU_POWER_IO); return 0; }
    if (now < s->last_now) { fail(s, A733_GPU_POWER_CLOCK); return 0; }
    s->last_now = now;
    if (now >= s->deadline) { fail(s, A733_GPU_POWER_TIMEOUT); return 0; }
    return 1;
}
int a733_gpu_power_init(struct a733_gpu_power *s,
                       const struct a733_gpu_power_ops *ops, void *context)
{
    if (!s || !ops || s->phase != A733_GPU_POWER_NEW || s->ops.now ||
        !ops->now || !ops->acquire || !ops->release || !ops->read ||
        !ops->update) return 0;
    uintptr_t a = (uintptr_t)s, b = (uintptr_t)ops;
    if (a > UINTPTR_MAX - sizeof(*s) || b > UINTPTR_MAX - sizeof(*ops) ||
        !(a + sizeof(*s) <= b || b + sizeof(*ops) <= a)) return 0;
    *s = (struct a733_gpu_power){.ops = *ops, .context = context};
    return 1;
}
static enum a733_gpu_power_result begin(struct a733_gpu_power *s,
                                        uint64_t deadline, int on)
{
    if (!s || !s->ops.now || !s->row.domains || (on ?
        (s->phase != A733_GPU_POWER_NEW && s->phase != A733_GPU_POWER_OFF) :
        s->phase != A733_GPU_POWER_ON)) return A733_GPU_POWER_INVALID;
    s->deadline = deadline;
    s->index = 0;
    s->requested = 0;
    s->phase = on ? A733_GPU_POWER_STARTING : A733_GPU_POWER_STOPPING;
    if (on) {
        s->lease_attempted = 0;
        s->core_policy = s->core_status = s->core_config = s->core_seen = 0;
        for (unsigned i = 0; i < A733_GPU_CORE_NOTES; ++i) s->core_note[i] = 0;
        s->core_wait_us[0] = s->core_wait_us[1] = s->core_wait_us[2] = s->core_power_up = 0;
    }
    return clock_read(s) ? A733_GPU_POWER_AGAIN : A733_GPU_POWER_FAULT;
}
enum a733_gpu_power_result a733_gpu_power_on(struct a733_gpu_power *s, uint64_t deadline)
{ return begin(s, deadline, 1); }
enum a733_gpu_power_result a733_gpu_power_off(struct a733_gpu_power *s, uint64_t deadline)
{ return begin(s, deadline, 0); }
enum a733_gpu_power_result a733_gpu_power_poll(struct a733_gpu_power *s)
{
    if (!s || !s->ops.now) return A733_GPU_POWER_INVALID;
    if (s->phase == A733_GPU_POWER_FAILED) return A733_GPU_POWER_FAULT;
    if (s->phase == A733_GPU_POWER_ON || s->phase == A733_GPU_POWER_OFF)
        return A733_GPU_POWER_OK;
    if (s->phase != A733_GPU_POWER_STARTING && s->phase != A733_GPU_POWER_STOPPING)
        return A733_GPU_POWER_INVALID;
    if (!clock_read(s)) return A733_GPU_POWER_FAULT;
    int on = s->phase == A733_GPU_POWER_STARTING;
    if (on && !s->lease_attempted) {
        struct a733_gpu_power_resources r = {0};
        s->lease_attempted = 1;
        if (!s->ops.acquire(s->context, &r)) return fail(s, A733_GPU_POWER_IO);
        s->lease_held = 1;
        /* VENDOR-STATE VARIANT: PERI0 400 MHz stays the shared parent the
         * resources hold; the GPU core runs from PLL_GPU0 under the LOCAL
         * lease. DCDC4 must already read 960 mV. */
        if (r.parent_hz != A733_GPU_INITIAL_HZ || r.supply_uv != VENDOR_STATE_SUPPLY_UV)
            return fail(s, A733_GPU_POWER_RESOURCES);
        if (!clock_read(s)) return A733_GPU_POWER_FAULT;
    }
    uint32_t count = on ? sizeof(on_steps) / sizeof(*on_steps) :
                          sizeof(off_steps) / sizeof(*off_steps);
    /* Steps run back to back. A delay waits on the clock inside the poll. The
     * domain step requests the row once and returns AGAIN until the service
     * answers, one acquire load per poll. A delay whose clock stops advancing
     * returns AGAIN without advancing the step. Every wait ends at the
     * deadline. A CORE status poll re-reads for up to A733_GPU_SPIN_US
     * within one call, with the clock advancing, before it returns AGAIN;
     * each step list has one poll. A SETTLE observation reads CORE status
     * once; after the power-up entry state it re-reads the same way while
     * the status is 0. It then continues with the status it saw. */
    struct a733_gpu_spin spin = {0};
    while (s->index < count) {
        const struct step *p = &(on ? on_steps : off_steps)[s->index];
        if (p->kind == DOMAIN) {
            if (!s->requested) {
                if (!a733_power_domain_request(&s->row, p->value)) return fail(s, A733_GPU_POWER_STATE);
                s->requested = 1;
            }
            enum a733_power_row_state state = a733_power_domain_state(&s->row);
            if (state == A733_POWER_ROW_PENDING) return A733_GPU_POWER_AGAIN;
            if (state == A733_POWER_ROW_REFUSED) return fail(s, A733_GPU_POWER_STATE);
            if (state != (p->value ? A733_POWER_ROW_ON : A733_POWER_ROW_OFF))
                return fail(s, A733_GPU_POWER_DOMAIN);
            s->requested = 0;
            ++s->index;
            if (!clock_read(s)) return A733_GPU_POWER_FAULT;
            continue;
        }
        if (p->kind == DELAY) {
            /* The clock may round down to whole microseconds; passing the
             * start sample by more than the delay guarantees the delay. */
            uint64_t until;
            struct a733_gpu_spin delay = {0};
            if (!a733_deadline_after(s->last_now, p->value + 1, &until))
                return fail(s, A733_GPU_POWER_TIMEOUT);
            (void)a733_gpu_spin_advancing(&delay, s->last_now);
            while (s->last_now < until) {
                if (!clock_read(s)) return A733_GPU_POWER_FAULT;
                if (!a733_gpu_spin_advancing(&delay, s->last_now)) return A733_GPU_POWER_AGAIN;
            }
            ++s->index;
            continue;
        }
        if (p->kind == SETTLE) {
            struct a733_gpu_spin wait = {0};
            const uint64_t start = s->last_now;
            uint32_t value;
            do {
                if (!s->ops.read(s->context, p->bank, p->offset, &value)) return fail(s, A733_GPU_POWER_IO);
                s->last_value = s->core_note[p->value] = value;
                s->core_seen |= UINT32_C(8) << p->value;
                if (!clock_read(s)) return A733_GPU_POWER_FAULT;
                s->core_wait_us[p->value - A733_GPU_NOTE_AFTER_DELAYS] = (uint32_t)(s->last_now - start);
                if (value != 0 && value != 8) return fail(s, A733_GPU_POWER_STATE);
            } while (value != 8 && s->core_power_up && a733_gpu_spin_again(&wait, s->last_now));
            ++s->index;
            continue;
        }
        if (p->kind == LOCK || p->kind == UPDATED) {
            /* VENDOR-STATE VARIANT. slot: which LOCK or UPDATED step of the table. */
            unsigned slot = 0;
            for (uint32_t i = 0; i < s->index; ++i) slot += on_steps[i].kind == p->kind;
            struct a733_gpu_spin stall = {0};
            uint32_t value = 0, reads = 0, run = 0;
            while (reads < p->value && (p->kind == LOCK ? run < 3 : !reads || (value & p->mask))) {
                if (!s->ops.read(s->context, p->bank, p->offset, &value)) return fail(s, A733_GPU_POWER_IO);
                ++reads;
                s->last_value = value;
                if (!clock_read(s)) return A733_GPU_POWER_FAULT;
                if (p->kind == UPDATED) continue;
                vendor_state_clock.pll_last = value;
                run = value & p->mask ? run + 1 : 0;
                /* udelay(3) after each read (ccu_common.c:52-59). A clock that
                 * stops advancing returns AGAIN, and the wait starts again. */
                uint64_t until;
                if (!a733_deadline_after(s->last_now, 3 + 1, &until)) return fail(s, A733_GPU_POWER_TIMEOUT);
                (void)a733_gpu_spin_advancing(&stall, s->last_now);
                while (s->last_now < until) {
                    if (!clock_read(s)) return A733_GPU_POWER_FAULT;
                    if (!a733_gpu_spin_advancing(&stall, s->last_now)) return A733_GPU_POWER_AGAIN;
                }
            }
            if (p->kind == LOCK) {
                vendor_state_clock.lock_reads[slot & 1] = reads;
                vendor_state_clock.lock_run[slot & 1] = run;
                if (run < 3) return fail(s, (enum a733_gpu_power_error)VENDOR_STATE_ERROR_LOCK);
            } else {
                vendor_state_clock.update_reads[slot & 1] = reads;
                if (value & p->mask) return fail(s, (enum a733_gpu_power_error)VENDOR_STATE_ERROR_UPDATE);
            }
            ++s->index;
            continue;
        }
        uint32_t value = 0;
        int ok;
        if (p->kind == UPDATE)
            ok = s->ops.update(s->context, p->bank, p->offset, p->mask, p->value);
        else {
            ok = s->ops.read(s->context, p->bank, p->offset, &value);
            if (ok) s->last_value = value;
        }
        if (!ok) return fail(s, A733_GPU_POWER_IO);
        if (p->kind == CORE_POLICY) { s->core_policy = value; s->core_seen |= 1; }
        if (p->kind == CORE_STATUS) { s->core_status = value; s->core_seen |= 2; }
        if (p->kind == CORE_CONFIG) { s->core_config = value; s->core_seen |= 4; }
        if (p->kind == NOTE) { s->core_note[p->value] = value; s->core_seen |= UINT32_C(8) << p->value; }
        if (!clock_read(s)) return A733_GPU_POWER_FAULT;
        if (p->kind == CHECK && (value & p->mask) != p->value) return fail(s, A733_GPU_POWER_STATE);
        if (p->bank == A733_GPU_CCU && p->offset == 0x00e0 && p->kind != UPDATE) vendor_state_clock.pll_last = value;
        if (p->kind == PLL) {
            if ((value & p->mask) != p->value) return fail(s, (enum a733_gpu_power_error)VENDOR_STATE_ERROR_PLL);
            ++vendor_state_clock.pll_steps;
        }
        if (p->kind == CORE_CONFIG) {
            if (!core_admitted(s)) return fail(s, A733_GPU_POWER_STATE);
            s->core_power_up = (uint32_t)core_power_up(s);
        }
        if (p->kind == POLL) {
            if (value != 0 && value != 8) return fail(s, A733_GPU_POWER_STATE);
            if (value != p->value) {
                if (a733_gpu_spin_again(&spin, s->last_now)) continue;
                return A733_GPU_POWER_AGAIN;
            }
        }
        ++s->index;
    }
    if (!on) {
        if (!s->ops.release(s->context)) return fail(s, A733_GPU_POWER_IO);
        s->lease_held = 0;
        if (!clock_read(s)) return A733_GPU_POWER_FAULT;
    }
    s->phase = on ? A733_GPU_POWER_ON : A733_GPU_POWER_OFF;
    return A733_GPU_POWER_OK;
}
enum a733_gpu_power_result a733_gpu_power_cancel(struct a733_gpu_power *s)
{
    if (!s || (s->phase != A733_GPU_POWER_STARTING && s->phase != A733_GPU_POWER_STOPPING))
        return A733_GPU_POWER_INVALID;
    return fail(s, A733_GPU_POWER_CANCELLED);
}
