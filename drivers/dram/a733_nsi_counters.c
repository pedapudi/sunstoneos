/* SPDX-License-Identifier: Apache-2.0
 * Counter sequence after sunxi-nsi.c __nsi_pmu_timer_store_v2 (linux-orangepi 2ac08e8c7); see a733_nsi_counters.h. */
#include "a733_nsi_counters.h"
#include <string.h>

static const uint32_t port[A733_NSI_COUNTER_BANKS] = {6, 18};

uintptr_t a733_nsi_counter_address(unsigned bank, unsigned word)
{
    return A733_NSI_COUNTER_BASE + UINT32_C(0x200) * port[bank] + UINT32_C(0xc0) + 4u * word;
}
static int bank_word(uintptr_t address, unsigned *word)
{
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        const uintptr_t first = a733_nsi_counter_address(b, 0);
        if (address >= first && address < first + 4u * A733_NSI_COUNTER_WORDS && !((address - first) & 3u)) {
            *word = (unsigned)((address - first) / 4u);
            return 1;
        }
    }
    return 0;
}
int a733_nsi_counter_readable(uintptr_t address) { unsigned w; return bank_word(address, &w); }
int a733_nsi_counter_writable(uintptr_t address) { unsigned w; return bank_word(address, &w) && w <= A733_NSI_W_CYCLE; }

static int rd(struct a733_nsi_counters *c, uintptr_t a, uint32_t *v)
{
    if (a733_nsi_counter_readable(a) && c->ops.read(c->context, a, v)) return 1;
    c->fail_address = (uint32_t)a; c->fail_value = 0;
    return 0;
}
/* One write and its readback; check selects whether the readback must equal the value. */
static int wr(struct a733_nsi_counters *c, uintptr_t a, uint32_t v, int check, uint32_t *readback)
{
    uint32_t r = 0;
    if (!a733_nsi_counter_writable(a) || !c->ops.write(c->context, a, v) || !c->ops.read(c->context, a, &r) ||
        (check && r != v)) {
        c->fail_address = (uint32_t)a; c->fail_value = r;
        return 0;
    }
    if (readback) *readback = r;
    return 1;
}
/* The vendor's 10 us after enable (udelay(10)); bounded so a stopped counter cannot hold the caller. */
static int settle(struct a733_nsi_counters *c)
{
    const uint64_t begin = c->ops.ticks(c->context);
    for (unsigned i = 0; i < 100000u; ++i) {
        const uint64_t t = c->ops.ticks(c->context);
        if (t < begin) return 0;
        if (t - begin >= 240u) return 1;
    }
    return 0;
}
/* CYCLE, CLR and ENABLE back to their reset values, each read back. Every word is attempted even after a miss. */
static int restore_bank(struct a733_nsi_counters *c, unsigned b)
{
    int ok = 1;
    static const unsigned order[] = {A733_NSI_W_ENABLE, A733_NSI_W_CYCLE, A733_NSI_W_CLR};
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned w = order[i];
        /* The enable word first loses only bit 0, so the bank stops before its period changes. */
        const uint32_t v = w == A733_NSI_W_ENABLE ? c->reset[b][w] & ~1u : c->reset[b][w];
        if (!wr(c, a733_nsi_counter_address(b, w), v, 1, NULL)) ok = 0;
    }
    if (!wr(c, a733_nsi_counter_address(b, A733_NSI_W_ENABLE), c->reset[b][A733_NSI_W_ENABLE], 1, NULL)) ok = 0;
    if (ok) c->programmed &= ~(1u << b);   /* a bank whose restore missed stays programmed, so the failure path retries it */
    return ok;
}
static int restore_all(struct a733_nsi_counters *c)
{
    int ok = 1;
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b)
        if (c->programmed >> b & 1u) ok &= restore_bank(c, b);
    return ok;
}
static int fail(struct a733_nsi_counters *c)
{
    const uint32_t address = c->fail_address, value = c->fail_value;
    (void)restore_all(c);
    c->failed = 1;
    c->fail_address = address; c->fail_value = value;   /* the first miss, not a restore's */
    return A733_NSI_C_IO;
}

int a733_nsi_counters_open(struct a733_nsi_counters *c, const struct a733_nsi_counter_ops *ops, void *context,
                           uint32_t reset[A733_NSI_COUNTER_BANKS][A733_NSI_COUNTER_WORDS])
{
    if (!c || !ops || !ops->read || !ops->write || !ops->ticks || c->opened) return A733_NSI_C_REFUSED;
    memset(c, 0, sizeof(*c));
    c->ops = *ops; c->context = context;
    /* Reads only: every word of both banks, before any write. */
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b)
        for (unsigned w = 0; w < A733_NSI_COUNTER_WORDS; ++w)
            if (!rd(c, a733_nsi_counter_address(b, w), &c->reset[b][w])) {
                c->failed = 1;
                return A733_NSI_C_IO;
            }
    if (reset) memcpy(reset, c->reset, sizeof(c->reset));
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b)
        if (c->reset[b][A733_NSI_W_ENABLE] & 1u) return A733_NSI_C_BUSY;
    c->opened = 1;
    return A733_NSI_C_OK;
}
int a733_nsi_counters_begin(struct a733_nsi_counters *c, unsigned bank, uint32_t cycle, uint32_t *clear_readback)
{
    if (!c || !c->opened || c->failed || bank >= A733_NSI_COUNTER_BANKS || (c->programmed >> bank & 1u) || !cycle)
        return A733_NSI_C_REFUSED;
    const uint32_t enable = c->reset[bank][A733_NSI_W_ENABLE];
    uint32_t clear;
    c->programmed |= 1u << bank;   /* before the first write, so a miss restores this bank too */
    if (!wr(c, a733_nsi_counter_address(bank, A733_NSI_W_CYCLE), cycle, 1, NULL) ||
        !wr(c, a733_nsi_counter_address(bank, A733_NSI_W_ENABLE), enable & ~1u, 1, NULL) ||
        !rd(c, a733_nsi_counter_address(bank, A733_NSI_W_CLR), &clear) ||
        !wr(c, a733_nsi_counter_address(bank, A733_NSI_W_CLR), clear | 1u, 0, clear_readback) ||
        !wr(c, a733_nsi_counter_address(bank, A733_NSI_W_ENABLE), enable | 1u, 1, NULL)) return fail(c);
    if (!settle(c)) { c->fail_address = (uint32_t)a733_nsi_counter_address(bank, A733_NSI_W_ENABLE); c->fail_value = 0; return fail(c); }
    return A733_NSI_C_OK;
}
static int values(struct a733_nsi_counters *c, unsigned bank, uint32_t v[A733_NSI_COUNTER_VALUES])
{
    for (unsigned i = 0; i < A733_NSI_COUNTER_VALUES; ++i)
        if (!rd(c, a733_nsi_counter_address(bank, A733_NSI_W_RQ_RD + i), &v[i])) return 0;
    return 1;
}
int a733_nsi_counters_sample(struct a733_nsi_counters *c, unsigned bank, uint32_t value[A733_NSI_COUNTER_VALUES])
{
    if (!c || !c->opened || c->failed || bank >= A733_NSI_COUNTER_BANKS || !(c->programmed >> bank & 1u) || !value)
        return A733_NSI_C_REFUSED;
    return values(c, bank, value) ? A733_NSI_C_OK : fail(c);
}
int a733_nsi_counters_end(struct a733_nsi_counters *c, unsigned bank, uint32_t value[A733_NSI_COUNTER_VALUES],
                          uint32_t held[A733_NSI_COUNTER_VALUES])
{
    if (!c || !c->opened || c->failed || bank >= A733_NSI_COUNTER_BANKS || !(c->programmed >> bank & 1u) ||
        !value || !held) return A733_NSI_C_REFUSED;
    if (!wr(c, a733_nsi_counter_address(bank, A733_NSI_W_ENABLE), c->reset[bank][A733_NSI_W_ENABLE] & ~1u, 1, NULL) ||
        !values(c, bank, value) || !settle(c) || !values(c, bank, held)) return fail(c);
    return restore_bank(c, bank) ? A733_NSI_C_OK : fail(c);
}
int a733_nsi_counters_restore(struct a733_nsi_counters *c, unsigned bank)
{
    if (!c || bank >= A733_NSI_COUNTER_BANKS) return A733_NSI_C_REFUSED;
    /* A closed owner may still hold a programmed bank whose restore missed; OPEN's reset values remain valid. */
    if (!(c->programmed >> bank & 1u)) return A733_NSI_C_OK;
    if (restore_bank(c, bank)) return A733_NSI_C_OK;
    c->failed = 1;
    return A733_NSI_C_IO;
}
int a733_nsi_counters_close(struct a733_nsi_counters *c)
{
    if (!c || !c->opened) return A733_NSI_C_REFUSED;
    const int ok = restore_all(c);
    c->opened = 0;
    if (!ok) { c->failed = 1; return A733_NSI_C_IO; }
    return c->failed ? A733_NSI_C_IO : A733_NSI_C_OK;
}
int a733_nsi_counters_call(struct a733_nsi_counters *c, const struct a733_nsi_counter_ops *ops, void *context,
                           struct a733_nsi_counter_call *a)
{
    if (!c || !a) return A733_NSI_C_REFUSED;
    int r;
    switch (a->op) {
    case A733_NSI_OPEN: r = a733_nsi_counters_open(c, ops, context, a->reset); break;
    case A733_NSI_BEGIN: r = a733_nsi_counters_begin(c, a->bank, a->cycle, &a->clear); break;
    case A733_NSI_SAMPLE: r = a733_nsi_counters_sample(c, a->bank, a->value); break;
    case A733_NSI_END: r = a733_nsi_counters_end(c, a->bank, a->value, a->held); break;
    case A733_NSI_CLOSE: r = a733_nsi_counters_close(c); break;
    default: r = A733_NSI_C_REFUSED; break;
    }
    a->status = (uint32_t)r; a->fail_address = c->fail_address; a->fail_value = c->fail_value;
    return r;
}
