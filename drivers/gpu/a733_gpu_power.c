#include "a733_gpu_power.h"
#include "a733_range.h"
#include <stddef.h>

#define BIT(n) (UINT32_C(1) << (n))
/* apps/gpu-power-id/prepare.py reads the step tables of a linked image and
 * relies on the values of DOMAIN and of the CORE bank. NOTE and SETTLE carry
 * their enum a733_gpu_core_note slot in value. PLL is a CHECK of a PLL_GPU0
 * word that fails with A733_GPU_POWER_PLL. LOCK reads the word until mask
 * reads set three times in a row, 3 us apart, at most value reads, as
 * ccu_helper_wait_for_lock does (ccu_common.c:47-61), and fails with
 * A733_GPU_POWER_LOCK. UPDATED reads the word until mask reads clear, at most
 * value reads, and fails with A733_GPU_POWER_UPDATE. */
enum kind { CHECK, UPDATE, DELAY, DOMAIN, CORE_POLICY, CORE_STATUS, CORE_CONFIG, POLL,
            NOTE, SETTLE, PLL, LOCK, UPDATED };
struct step { enum kind kind; enum a733_gpu_power_bank bank;
    uint32_t offset, mask, value; };
/* PLL_GPU0 (CCU 0x0e0, manual 4.1.6.16): enable 31, LDO 30, lock enable 29,
 * lock 28 (read-only), output gate 27, P0 22:20, N 15:8 and M1 1. 1008 MHz is
 * N 84 and P0 2 from 24 MHz. Read-backs leave out the lock bit and boot0's
 * reserved bit 24. */
#define PLL_CONTROL (BIT(31) | BIT(30) | BIT(29) | BIT(27))
#define PLL_FACTORS UINT32_C(0x0070ff02)
#define PLL_1008 (PLL_CONTROL | UINT32_C(0x00105300))
_Static_assert(UINT64_C(24000000) * 84 / 2 == A733_GPU_CORE_HZ, "PLL_GPU0 factors give the operating point");
/* The vendor's order (sunxi_platform.c at kernel 2ac08e8c7;
 * docs/designs/gpu-vendor-default.md). The GPU_TOP domain is PCK domain 5,
 * which the power-domain owner powers through the GPU's row, with the GPU
 * clocked in both directions as the vendor's later power-ons and power-offs
 * do (sunxiPrePowerState, sunxiPostPowerState). The MBUS gates, PPU clock, closed clock gate on its reset source 0 (PLL_GPU0)
 * and divider 1, asserted reset and a disabled PLL_GPU0 with input divider M1
 * 1, which the factor write keeps, are checked first.
 * The selected BXM rgx_sunxi adapter controls CORE6 directly; its
 * device-tree node is commented out, and nothing writes its delay row.
 * DEN0051E 5.2.8 requires settled static policy before changing PWCR device
 * request enables. After power-up CORE6 reads policy 8, status 0 and the
 * one-Q-Channel reset PWCR: its default ON policy has a transition
 * outstanding that the GPU's Q-Channel accepts only once the GPU is clocked.
 * The vendor writes PWCR 0 in that state at its first power-on, and this
 * controller admits it as the one unsettled exception. ON then follows
 * sunxi_enable_clks_wrap and sunxiSetFrequency:
 * - PLL_GPU0 enabled at its present factors as ccu_pll_gate_helper_enable
 *   does (ccu_gate.c:157-203), each write read back;
 * - the GPU bus reset released, the bus gate opened, the core clock gate
 *   opened with the update bit, and the word written again with the update
 *   bit; each update bit must clear;
 * - PLL_GPU0 set to 1008 MHz as ccu_nm_set_rate does with AW_STANDARD_CCU
 *   (ccu_nm.c:250-345).
 * TOP5 ON and CORE6 policy 8 come last, as in the vendor's later power-ons;
 * at power-up TOP5 is ON and the CORE6 policy already reads 8, so the first
 * ON's CORE6 transition completes when the clocks run, as the board showed,
 * and PWCR 0 precedes the clocks as in the vendor's first power-on. NOTE
 * and SETTLE steps record CORE6 words for diagnosis and require no
 * particular value; a SETTLE status other than 0 or 8 still fails. */
static const struct step on_steps[] = {
    /* The GPU AHB master gate read 0 after a TOP5 OFF and took no write
     * while TOP5 was OFF (board-session-20261025-7, s11-gpo-run); the MBUS
     * gate was not read and is treated the same way. ON records both here
     * and requires them after TOP5 ON. The vendor never reads or writes them after boot: they are
     * CLK_IS_CRITICAL (ccu-sun60iw2.c:587-590, 658-661). */
    {NOTE, A733_GPU_CCU, 0x05c0, 0, A733_GPU_NOTE_AHB_ENTRY},
    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_ENTRY},
    {CHECK, A733_GPU_R_CCU, 0x01ac, BIT(0), BIT(0)},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8700000f, 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), 0},
    {CHECK, A733_GPU_CCU, 0x00e0, BIT(31) | BIT(29) | BIT(27) | BIT(1), 0},
    /* CORE6's PPU lies in the always-on PCK block, readable with TOP5 OFF. */
    {CORE_POLICY, A733_GPU_CORE, 0, 0, 0},
    {CORE_STATUS, A733_GPU_CORE, 8, 0, 0},
    {NOTE, A733_GPU_CORE, 0xfb0, 0, A733_GPU_NOTE_IDR0},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR},
    {CORE_CONFIG, A733_GPU_CORE, 0x20, 0, 0},
    {UPDATE, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {CHECK, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_PWCR},
    /* PLL_GPU0 enable: output gate cleared, LDO, enable and lock enable set,
     * the lock wait, 20 us (ccu_common.c:61), the output gate set. */
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(27), 0},
    {PLL, A733_GPU_CCU, 0x00e0, BIT(31) | BIT(29) | BIT(27), 0},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(30)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(30)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(31)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(29)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30) | BIT(29)},
    {LOCK, A733_GPU_CCU, 0x00e0, BIT(28), 100},
    {DELAY, A733_GPU_CCU, 0, 0, 20},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(27)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, PLL_CONTROL},
    /* sunxi_enable_clks_wrap: reset, bus gate, core gate with the update bit,
     * then the update bit again. */
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
    /* ccu_nm_set_rate to 1008 MHz: output gate cleared, N 84 and P0 2, lock
     * enable cleared and set, the lock wait, 20 us, the output gate set. */
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(27), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30) | BIT(29)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0x0070ff00, 0x00105300},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL | PLL_FACTORS, PLL_1008 & ~BIT(27)},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(29), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30)},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(29)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30) | BIT(29)},
    {LOCK, A733_GPU_CCU, 0x00e0, BIT(28), 100},
    {DELAY, A733_GPU_CCU, 0, 0, 20},
    {UPDATE, A733_GPU_CCU, 0x00e0, 0, BIT(27)},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL | PLL_FACTORS, PLL_1008},
    /* sunxiPrePowerState: TOP5 ON (pm_runtime_get_sync) once the GPU is
     * clocked, then CORE6 policy 8. TOP5 is ON from reset at the first ON,
     * which the owner adopts without a command. */
    {DOMAIN, A733_GPU_CCU, 0, 0, 1},
    {NOTE, A733_GPU_CCU, 0x05c0, 0, A733_GPU_NOTE_AHB_TOP5_ON},
    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_TOP5_ON},
    {CHECK, A733_GPU_CCU, 0x05c0, BIT(7), BIT(7)},
    {CHECK, A733_GPU_CCU, 0x05e0, BIT(16), BIT(16)},
    {UPDATE, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {CHECK, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_POLICY},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR_LATE},
    /* The reference never reads CORE6 status. Validate acknowledgement after
     * the clocks run, before any engine access. */
    {POLL, A733_GPU_CORE, 8, UINT32_MAX, 8},
};
/* Complete firmware stop is the caller's prerequisite. The vendor's order
 * (sunxiPostPowerState): CORE6 policy 0, TOP5 OFF (pm_runtime_put_sync),
 * then the clocks (sunxi_disable_clks_wrap): bus gate, core gate, PLL_GPU0
 * as ccu_pll_gate_helper_disable does (ccu_gate.c:40-69), then the reset.
 * This controller adds a wait for CORE6 OFF before TOP5 OFF; both happen
 * with the GPU still clocked. */
static const struct step off_steps[] = {
    {UPDATE, A733_GPU_CORE, 0, UINT32_MAX, 0},
    {CHECK, A733_GPU_CORE, 0, UINT32_MAX, 0},
    {POLL, A733_GPU_CORE, 8, UINT32_MAX, 0},
    {DOMAIN, A733_GPU_CCU, 0, 0, 0},
    {NOTE, A733_GPU_CCU, 0x05c0, 0, A733_GPU_NOTE_AHB_TOP5_OFF},
    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_TOP5_OFF},
    {UPDATE, A733_GPU_CCU, 0x0b24, BIT(0), 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), BIT(16)},
    {UPDATE, A733_GPU_CCU, 0x0b20, BIT(31), BIT(27)},
    {CHECK, A733_GPU_CCU, 0x0b20, BIT(31), 0},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(27), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30) | BIT(29)},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(29), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(31) | BIT(30)},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(31), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, BIT(30)},
    {UPDATE, A733_GPU_CCU, 0x00e0, BIT(30), 0},
    {PLL, A733_GPU_CCU, 0x00e0, PLL_CONTROL, 0},
    {UPDATE, A733_GPU_CCU, 0x0b24, BIT(16), 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), 0},
    {NOTE, A733_GPU_CCU, 0x05c0, 0, A733_GPU_NOTE_AHB_OFF_END},
    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_OFF_END},
};
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
    /* ON clears its own notes and keeps those of the OFF before it; OFF
     * clears its notes. core_seen bit 3 + n follows note n. */
    const unsigned first = on ? 0 : A733_GPU_NOTE_AHB_TOP5_OFF;
    const unsigned end = on ? A733_GPU_NOTE_AHB_TOP5_OFF : A733_GPU_CORE_NOTES;
    for (unsigned i = first; i < end; ++i) {
        s->core_note[i] = 0;
        s->core_seen &= ~(UINT32_C(8) << i);
    }
    if (on) {
        s->lease_attempted = 0;
        s->core_policy = s->core_status = s->core_config = 0;
        s->core_seen &= ~UINT32_C(7);
        s->core_power_up = 0;
    }
    s->pll_lock_reads[0] = s->pll_lock_reads[1] = s->clock_update_reads[0] = s->clock_update_reads[1] = 0;
    s->pll_word = 0;
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
        /* The GPU core runs from PLL_GPU0 under the LOCAL lease; no PERI0
         * parent is held, so only the supply is required. */
        if (r.supply_uv != A733_GPU_SUPPLY_UV)
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
     * once. A LOCK or UPDATED wait runs to its read bound inside one call. */
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
        if (p->kind == LOCK || p->kind == UPDATED) {
            /* slot: the first or second wait of this kind in the table. */
            const struct step *table = on ? on_steps : off_steps;
            unsigned slot = 0;
            for (uint32_t i = 0; i < s->index; ++i) slot += table[i].kind == p->kind;
            struct a733_gpu_spin stall = {0};
            uint32_t value = 0, reads = 0, run = 0;
            while (reads < p->value && (p->kind == LOCK ? run < 3 : !reads || (value & p->mask))) {
                if (!s->ops.read(s->context, p->bank, p->offset, &value)) return fail(s, A733_GPU_POWER_IO);
                ++reads;
                s->last_value = value;
                if (!clock_read(s)) return A733_GPU_POWER_FAULT;
                if (p->kind == UPDATED) continue;
                s->pll_word = value;
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
                s->pll_lock_reads[slot & 1] = reads;
                if (run < 3) return fail(s, A733_GPU_POWER_LOCK);
            } else {
                s->clock_update_reads[slot & 1] = reads;
                if (value & p->mask) return fail(s, A733_GPU_POWER_UPDATE);
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
        if (p->kind == NOTE || p->kind == SETTLE) {
            s->core_note[p->value] = value;
            s->core_seen |= UINT32_C(8) << p->value;
        }
        if (p->kind == PLL) s->pll_word = value;
        if (!clock_read(s)) return A733_GPU_POWER_FAULT;
        if (p->kind == CHECK && (value & p->mask) != p->value) return fail(s, A733_GPU_POWER_STATE);
        if (p->kind == PLL && (value & p->mask) != p->value) return fail(s, A733_GPU_POWER_PLL);
        if (p->kind == SETTLE && value != 0 && value != 8) return fail(s, A733_GPU_POWER_STATE);
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
