#include "a733_gpu_power.h"
#include "a733_range.h"
#include <stddef.h>

#define BIT(n) (UINT32_C(1) << (n))
/* apps/gpu-power-id/prepare.py reads the step tables of a linked image and
 * relies on the values of DOMAIN and of the CORE bank. NOTE and SETTLE carry
 * their enum a733_gpu_core_note slot in value. */
enum kind { CHECK, UPDATE, DELAY, DOMAIN, CORE_POLICY, CORE_STATUS, CORE_CONFIG, POLL,
            NOTE, SETTLE };
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
 * GPU local clock selects pll-peri0-400m / 1.
 */
static const struct step on_steps[] = {
    {CHECK, A733_GPU_CCU, 0x05c0, BIT(7), BIT(7)},
    {CHECK, A733_GPU_CCU, 0x05e0, BIT(16), BIT(16)},
    {CHECK, A733_GPU_R_CCU, 0x01ac, BIT(0), BIT(0)},
    {CHECK, A733_GPU_CCU, 0x0b20, BIT(31), 0},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), 0},
    {DOMAIN, A733_GPU_CCU, 0, 0, 1},
    {CORE_POLICY, A733_GPU_CORE, 0, 0, 0},
    {CORE_STATUS, A733_GPU_CORE, 8, 0, 0},
    {NOTE, A733_GPU_CORE, 0xfb0, 0, A733_GPU_NOTE_IDR0},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR},
    {CORE_CONFIG, A733_GPU_CORE, 0x20, 0, 0},
    {UPDATE, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {CHECK, A733_GPU_CORE, 0x20, UINT32_MAX, 0},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_PWCR},
    {UPDATE, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {CHECK, A733_GPU_CORE, 0, UINT32_MAX, 8},
    {SETTLE, A733_GPU_CORE, 8, 0, A733_GPU_NOTE_AFTER_POLICY},
    {UPDATE, A733_GPU_CCU, 0x0b20, 0x8700000f, 0x0b000000},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8700000f, 0x03000000},
    {UPDATE, A733_GPU_CCU, 0x0b20, 0, BIT(31) | BIT(27)},
    {CHECK, A733_GPU_CCU, 0x0b20, 0x8700000f, 0x83000000},
    {UPDATE, A733_GPU_CCU, 0x0b24, 0, BIT(0)},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), BIT(0)},
    {DELAY, A733_GPU_CCU, 0, 0, 1},
    {UPDATE, A733_GPU_CCU, 0x0b24, 0, BIT(16)},
    {CHECK, A733_GPU_CCU, 0x0b24, BIT(16) | BIT(0), BIT(16) | BIT(0)},
    {NOTE, A733_GPU_CORE, 0x14, 0, A733_GPU_NOTE_MISR_LATE},
    /* The reference does not wait before enabling clocks/reset. Validate
     * acknowledgement only after that progress, before any engine access. */
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
static enum a733_gpu_power_result fail(struct a733_gpu_power *s,
                                       enum a733_gpu_power_error error)
{
    s->error = error;
    s->phase = A733_GPU_POWER_FAILED;
    return A733_GPU_POWER_FAULT;
}
/* Settled static OFF or ON, with whole words, or the power-up state with the
 * reset PWCR; nothing else has written CORE6 in that state. */
static int core_admitted(const struct a733_gpu_power *s)
{
    return ((s->core_policy == 0 || s->core_policy == 8) && s->core_status == s->core_policy) ||
        (s->core_policy == 8 && s->core_status == 0 && s->core_config == A733_GPU_CORE_RESET_PWCR);
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
        s->core_wait_us[0] = s->core_wait_us[1] = 0;
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
        if (r.parent_hz != A733_GPU_INITIAL_HZ || r.supply_uv != A733_GPU_INITIAL_UV)
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
                s->core_wait_us[p->value - A733_GPU_NOTE_AFTER_PWCR] = (uint32_t)(s->last_now - start);
                if (value != 0 && value != 8) return fail(s, A733_GPU_POWER_STATE);
            } while (value != 8 && s->core_status != s->core_policy &&
                     a733_gpu_spin_again(&wait, s->last_now));
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
        if (p->kind == CORE_CONFIG && !core_admitted(s)) return fail(s, A733_GPU_POWER_STATE);
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
