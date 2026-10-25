/* SPDX-License-Identifier: Apache-2.0 */
/* Records and log lines of bundles 6 and 7; see trace.h. */
#include "trace.h"

/* Re-spins of a completed phase: polls beyond the second. */
static unsigned respun(struct bb_twi *t)
{
    if (t->polls > t->polls_max) t->polls_max = t->polls;
    const unsigned extra = t->polls > 2 ? t->polls - 2 : 0;
    t->respins += extra;
    t->snapshot_respins += extra;
    t->mode = 0;
    return extra;
}

void bb_twi_observe(struct bb_twi *t, unsigned write, uintptr_t address, uint32_t value)
{
    if (address == BB_TWI_STAT) {
        /* The idle status ends a STOP wait whose STOP bit has cleared. */
        if (!write && t->mode == 3 && (value & 0xff) == 0xf8) {
            t->stop_respins += respun(t);
            if (++t->stops % BB_TWI_SNAPSHOT_STOPS == 0) {
                if (t->snapshot_respins > t->snapshot_respins_max) t->snapshot_respins_max = t->snapshot_respins;
                t->snapshot_respins = 0;
                ++t->snapshots;
            }
        }
        return;
    }
    if (address != BB_TWI_CNTR)
        return;
    if (write) {
        /* START and repeated START 0x68, byte 0x48, STOP 0x58. */
        t->mode = value == 0x58 ? 2 : value == 0x68 || value == 0x48 ? 1 : 0;
        t->condition = value == 0x68;
        t->polls = 0;
        return;
    }
    ++t->reads;
    t->enable_seen |= value & 0x80;
    if (t->mode == 1) {
        if (!t->polls++) {
            if (value & 8) ++t->first_set;
            else ++t->first_clear;
        }
        if (!(value & 8))
            return;
        (void)respun(t);
        if (t->condition) ++t->conditions;
        else ++t->bytes;
    } else if (t->mode >= 2) {
        /* Every poll of a STOP wait reads CNTR first; the status read
         * follows once the STOP bit reads clear. */
        ++t->polls;
        if (!(value & 0x10))
            t->mode = 3;
    }
}

void bb_steps_evaluate(const struct bb_step *s, unsigned count, uint32_t bit_ns, uint32_t byte_ns,
                       struct bb_step_bins *b, struct bb_bound *bound)
{
    *b = (struct bb_step_bins){{0}, 0, 0};
    *bound = (struct bb_bound){0, 0, 0, 0};
    if (count > BB_BOUND_STEPS)
        count = BB_BOUND_STEPS;
    uint64_t sorted[BB_BOUND_STEPS];
    uint64_t first_limit_scaled = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t ns = s[i].ticks * 125 / 3; /* 24 MHz counter ticks */
        const uint64_t wire = bb_twi_wire_ns(s[i].conditions, s[i].bytes, s[i].stops, bit_ns, byte_ns);
        /* A counter tick is 125/3 ns. Keep that fraction for admission. */
        const uint64_t scaled = s[i].ticks * 125000;
        const uint64_t divisor = wire * 3;
        const uint64_t ratio = divisor ? scaled / divisor + (scaled % divisor != 0) : UINT64_MAX;
        b->bins[ratio <= 1000 ? 0 : ratio <= 1050 ? 1 : ratio <= 1100 ? 2 : ratio <= BB_BOUND_PERMILLE ? 3 : 4]++;
        b->over_ms += ns > 1100000;
        if (ratio > b->worst)
            b->worst = ratio;
        if (!i) {
            bound->first_ns = ns;
            bound->first_limit_ns = wire * BB_BOUND_PERMILLE / 1000 + BB_BOUND_FIRST_EXTRA_NS;
            first_limit_scaled = wire * BB_BOUND_PERMILLE * 3 + (uint64_t)BB_BOUND_FIRST_EXTRA_NS * 3000;
        }
        unsigned j = i;
        for (; j && sorted[j - 1] > ratio; --j)
            sorted[j] = sorted[j - 1];
        sorted[j] = ratio;
    }
    if (!count)
        return;
    bound->median = sorted[count / 2] < UINT32_MAX ? (uint32_t)sorted[count / 2] : UINT32_MAX;
    bound->pass = bound->median <= BB_BOUND_PERMILLE && s[0].ticks * 125000 <= first_limit_scaled;
}

unsigned bb_byte_ns(uint32_t bit_ns, unsigned seen, uint64_t clear_ticks, uint32_t *byte_ns)
{
    const uint64_t nominal = 9ull * bit_ns, measured = clear_ticks * 125 / 3;
    *byte_ns = (uint32_t)nominal;
    if (!seen || measured * 1000 > nominal * BB_BYTE_LIMIT_PERMILLE)
        return 0;
    if (measured > nominal)
        *byte_ns = (uint32_t)measured;
    return 1;
}

unsigned bb_xaddr_admitted(uint32_t addr, uint32_t cntr, uint32_t stat, uint32_t lcr)
{
    /* ADDR 7:3 = 11110b selects 10-bit addressing, the only use of XADDR;
     * CNTR 0x40 is BUS_EN alone; STAT 0xf8 is idle; LCR: no line forced and
     * both read high. */
    return (addr & 0xf8) != 0xf0 && (cntr & 0xff) == 0x40 && (stat & 0xff) == 0xf8 && (lcr & 0x35) == 0x30;
}

void bb_samples_start(struct bb_samples *s)
{ *s = (struct bb_samples){0, 0, UINT32_MAX, 0, 0, UINT32_MAX, 0}; }

unsigned bb_samples_add(struct bb_samples *s, uint32_t ticks)
{
    if (s->count >= BB_COST_SAMPLES)
        return 0;
    s->ticks += ticks;
    s->group += ticks;
    if (ticks < s->min) s->min = ticks;
    if (ticks > s->max) s->max = ticks;
    if (++s->count % (BB_COST_SAMPLES / BB_COST_SUBWINDOWS) == 0) {
        if (s->group < s->sub_min) s->sub_min = s->group;
        if (s->group > s->sub_max) s->sub_max = s->group;
        s->group = 0;
    }
    return s->count < BB_COST_SAMPLES;
}

void bb_span_add(struct bb_span *s, uint32_t ticks)
{
    if (!s->count || ticks < s->min) s->min = ticks;
    if (ticks > s->max) s->max = ticks;
    ++s->count;
    s->ticks += ticks;
}

void bb_span_merge(struct bb_span *s, const struct bb_span *add)
{
    if (!add->count)
        return;
    if (!s->count || add->min < s->min) s->min = add->min;
    if (add->max > s->max) s->max = add->max;
    s->count += add->count;
    s->ticks += add->ticks;
}

void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint32_t mask, uint32_t done, uint64_t start, uint64_t bound, struct bb_status_trace *t)
{
    *t = (struct bb_status_trace){0};
    uint32_t previous = 0;
    uint64_t previous_at = 0;
    for (;;) {
        uint32_t v;
        /* read() completes its access before it returns (a fenced read), so
         * the stamp below follows the value it returned. */
        if (read(context, status, &v) != 1)
            return;
        const uint64_t at = now() - start;
        /* The first value and every change, with the stamp of the read
         * before it; the count goes on past the stored entries. */
        if (!t->reads++ || v != previous) {
            if (t->changes < BB_STATUS_CHANGES) {
                t->change[t->changes].value = v;
                t->change[t->changes].at = at;
                t->change[t->changes].before = previous_at;
            }
            ++t->changes;
        }
        if (t->reads == 1)
            t->first = v;
        const uint64_t before = previous_at;
        previous = v;
        previous_at = at;
        t->last_at = at;
        if ((v & mask) == done) {
            t->on = 1;
            t->on_at = at;
            t->on_before = before;
            return;
        }
        if (at >= bound) {
            t->bounded = 1;
            return;
        }
    }
}

/* Appends stop at BB_LINE_MAX - 2, leaving room for the newline and NUL. */
static void put(struct bb_line *l, const char *s)
{
    while (*s && l->n < BB_LINE_MAX - 2)
        l->text[l->n++] = *s++;
    l->text[l->n] = 0;
}
static void put_number(struct bb_line *l, uint64_t v, unsigned base)
{
    char digits[21];
    unsigned i = sizeof digits - 1;
    digits[i] = 0;
    do {
        digits[--i] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    put(l, digits + i);
}
void bb_line_start(struct bb_line *l, const char *s)
{
    l->n = 0;
    l->text[0] = 0;
    put(l, s);
}
void bb_line_dec(struct bb_line *l, const char *name, uint64_t v)
{
    put(l, " ");
    put(l, name);
    put(l, "=");
    put_number(l, v, 10);
}
void bb_line_hex(struct bb_line *l, const char *name, uint64_t v)
{
    put(l, " ");
    put(l, name);
    put(l, "=0x");
    put_number(l, v, 16);
}
void bb_line_end(struct bb_line *l)
{
    l->text[l->n++] = '\n';
    l->text[l->n] = 0;
}

unsigned bb6_summary_lines(const struct bb6_summary *s, struct bb_line *out)
{
    struct bb_line *l = out;
    bb_line_start(l, "TWI");
    bb_line_hex(l, "divisor", s->divisor);
    bb_line_dec(l, "bit_ns", s->bit_ns);
    bb_line_dec(l, "rate_hz", s->bit_ns ? 1000000000u / s->bit_ns : 0);
    bb_line_dec(l, "reads", s->twi_reads);
    bb_line_hex(l, "interrupt_enable_seen", s->enable_seen);
    bb_line_end(l++);
    bb_line_start(l, "SUPPLY");
    bb_line_hex(l, "state_word", s->state);
    bb_line_dec(l, "dcdc2_verified", s->dcdc2);
    bb_line_dec(l, "dcdc4_verified", s->dcdc4);
    bb_line_dec(l, "both_800mv_unchanged", s->unchanged);
    bb_line_end(l++);
    bb_line_start(l, "SNAPSHOT first");
    bb_line_hex(l, "dcdc2_enable_selector", s->dcdc2_first);
    bb_line_hex(l, "dcdc4_enable_selector", s->dcdc4_first);
    bb_line_end(l++);
    bb_line_start(l, "STEPS");
    bb_line_dec(l, "recorded", s->steps);
    bb_line_dec(l, "dropped", s->dropped);
    bb_line_dec(l, "permille_max", s->bins.worst);
    bb_line_dec(l, "excluded", s->wire_excluded);
    bb_line_end(l++);
    bb_line_start(l, "BOUND");
    bb_line_dec(l, "permille", BB_BOUND_PERMILLE);
    bb_line_dec(l, "median", s->bound.median);
    bb_line_dec(l, "first_ns", s->bound.first_ns);
    bb_line_dec(l, "first_limit_ns", s->bound.first_limit_ns);
    bb_line_dec(l, "pass", s->bound.pass);
    bb_line_end(l++);
    bb_line_start(l, "BYTE");
    bb_line_dec(l, "ns", s->byte_ns);
    bb_line_dec(l, "ok", s->byte_ok);
    bb_line_dec(l, "limit_permille", BB_BYTE_LIMIT_PERMILLE);
    bb_line_end(l++);
    bb_line_start(l, "WIRE");
    bb_line_dec(l, "clear_cntpct", s->cost.wire_clear);
    bb_line_dec(l, "set_cntpct", s->cost.wire_set);
    bb_line_dec(l, "seen", s->cost.wire_seen);
    bb_line_dec(l, "reads", s->cost.wire_reads);
    bb_line_end(l++);
    bb_line_start(l, "RESPINS");
    bb_line_dec(l, "recorded_steps", s->respins);
    bb_line_dec(l, "stop", s->stop_respins);
    bb_line_dec(l, "snapshot_max", s->snapshot_respins_max);
    bb_line_dec(l, "snapshots", s->snapshots);
    bb_line_end(l++);
    bb_line_start(l, "STEPS permille");
    bb_line_dec(l, "le1000", s->bins.bins[0]);
    bb_line_dec(l, "le1050", s->bins.bins[1]);
    bb_line_dec(l, "le1100", s->bins.bins[2]);
    bb_line_dec(l, "le1268", s->bins.bins[3]);
    bb_line_dec(l, "gt1268", s->bins.bins[4]);
    bb_line_dec(l, "gt1100us", s->bins.over_ms);
    bb_line_end(l++);
    bb_line_start(l, "TWI_COST");
    bb_line_dec(l, "reads", s->cost.reads);
    bb_line_dec(l, "subwindows", BB_COST_SUBWINDOWS);
    bb_line_dec(l, "core", s->cost.core);
    bb_line_dec(l, "task_ticks", s->cost.task_ticks);
    bb_line_end(l++);
    const struct { const char *name; const struct bb_window *w; } windows[] = {
        {"TWI_COST dsb", &s->cost.dsb}, {"TWI_COST plain_issue", &s->cost.plain},
        {"TWI_COST dependent", &s->cost.dependent}, {"TWI_COST time_read", &s->cost.time_read},
        {"TWI_COST adapter_read", &s->cost.adapter_read}, {"TWI_COST task_check", &s->cost.task_check},
        {"TWI_COST observer", &s->cost.observer}};
    for (unsigned i = 0; i < sizeof windows / sizeof windows[0]; ++i) {
        bb_line_start(l, windows[i].name);
        bb_line_dec(l, "cntpct", windows[i].w->ticks);
        bb_line_dec(l, "sub_min", windows[i].w->sub_min);
        bb_line_dec(l, "sub_max", windows[i].w->sub_max);
        bb_line_end(l++);
    }
    bb_line_start(l, "TWI_COST writes");
    bb_line_dec(l, "count", s->cost.writes);
    bb_line_dec(l, "cntpct", s->cost.write_ticks);
    bb_line_dec(l, "min", s->cost.write_min);
    bb_line_dec(l, "max", s->cost.write_max);
    bb_line_end(l++);
    const struct { const char *name; const struct bb_window *w; } next[] = {
        {"TWI_COST empty", &s->next.empty}, {"TWI_COST xaddr_write", &s->next.xaddr_write},
        {"TWI_COST loop", &s->next.loop}};
    for (unsigned i = 0; i < sizeof next / sizeof next[0]; ++i) {
        bb_line_start(l, next[i].name);
        bb_line_dec(l, "cntpct", next[i].w->ticks);
        bb_line_dec(l, "sub_min", next[i].w->sub_min);
        bb_line_dec(l, "sub_max", next[i].w->sub_max);
        bb_line_end(l++);
    }
    bb_line_start(l, "TWI_WRITE before");
    bb_line_dec(l, "run", s->next.write_run);
    bb_line_hex(l, "addr", s->next.addr);
    bb_line_hex(l, "xaddr", s->next.xaddr);
    bb_line_hex(l, "cntr", s->next.cntr);
    bb_line_hex(l, "stat", s->next.stat);
    bb_line_hex(l, "lcr", s->next.lcr);
    bb_line_end(l++);
    bb_line_start(l, "TWI_WRITE after");
    bb_line_dec(l, "changed", s->next.write_changed);
    bb_line_hex(l, "xaddr", s->next.xaddr_after);
    bb_line_hex(l, "cntr", s->next.cntr_after);
    bb_line_hex(l, "stat", s->next.stat_after);
    bb_line_hex(l, "lcr", s->next.lcr_after);
    bb_line_end(l++);
    const struct { const char *name; const struct bb_samples *x; } samples[] = {
        {"TWI_POLL clear", &s->next.poll}, {"TWI_POLL stamp", &s->next.stamp}};
    for (unsigned i = 0; i < 2; ++i) {
        bb_line_start(l, samples[i].name);
        bb_line_dec(l, "count", samples[i].x->count);
        bb_line_dec(l, "cntpct", samples[i].x->ticks);
        bb_line_dec(l, "sub_min", samples[i].x->sub_min);
        bb_line_dec(l, "sub_max", samples[i].x->sub_max);
        bb_line_dec(l, "min", samples[i].x->min);
        bb_line_dec(l, "max", samples[i].x->max);
        bb_line_end(l++);
    }
    static const char *const complete[BB_COMPLETE_KINDS] = {
        "TWI_COMPLETE byte", "TWI_COMPLETE receive", "TWI_COMPLETE restart", "TWI_COMPLETE start",
        "TWI_COMPLETE stop"};
    for (unsigned i = 0; i < BB_COMPLETE_KINDS; ++i) {
        bb_line_start(l, complete[i]);
        bb_line_dec(l, "window", s->next.complete_window);
        bb_line_dec(l, "count", s->next.complete[i].count);
        bb_line_dec(l, "cntpct", s->next.complete[i].ticks);
        bb_line_dec(l, "min", s->next.complete[i].min);
        bb_line_dec(l, "max", s->next.complete[i].max);
        bb_line_end(l++);
    }
    bb_line_start(l, "TWI_POLL window");
    bb_line_dec(l, "polls", s->next.polls);
    bb_line_dec(l, "ticked", s->next.poll_ticked);
    bb_line_dec(l, "snapshots", s->next.poll_snapshots);
    bb_line_hex(l, "loop_work", s->next.loop_work);
    bb_line_end(l++);
    bb_line_start(l, "PPU_MAPPING");
    bb_line_dec(l, "adapter_mapped", s->mapped);
    bb_line_hex(l, "attributes", s->par_attributes);
    bb_line_dec(l, "shareability", s->par_shareability);
    bb_line_hex(l, "physical", s->par_physical);
    bb_line_end(l++);
    bb_line_start(l, "DOMAIN_LEVEL");
    bb_line_hex(l, "r_ccu_1ac", s->ppu);
    bb_line_hex(l, "ccu_5c0", s->ahb);
    bb_line_hex(l, "ccu_5e0", s->mbus);
    bb_line_end(l++);
    static const unsigned domains[3] = {1, 4, 5};
    for (unsigned i = 0; i < 3; ++i) {
        bb_line_start(l, "DOMAIN");
        bb_line_dec(l, "id", domains[i]);
        bb_line_hex(l, "policy", s->policy[i]);
        bb_line_hex(l, "status", s->status[i]);
        bb_line_dec(l, "owner_check", s->check[i] == 1);
        bb_line_end(l++);
    }
    return (unsigned)(l - out);
}

unsigned bb7_summary_lines(const struct bb7_summary *s, struct bb_line *out)
{
    struct bb_line *l = out;
    bb_line_start(l, "ROW");
    bb_line_dec(l, "state", s->row_state);
    bb_line_dec(l, "owner_domain_state", s->domain_state);
    bb_line_hex(l, "state_word", s->state);
    bb_line_dec(l, "before_read", s->before_read);
    bb_line_dec(l, "after_read", s->after_read);
    bb_line_end(l++);
    bb_line_start(l, "OWNER");
    bb_line_dec(l, "accesses", s->accesses);
    bb_line_dec(l, "writes", s->writes);
    bb_line_dec(l, "first", s->owner_first);
    bb_line_end(l++);
    bb_line_start(l, "STATUS");
    bb_line_dec(l, "reads", s->status.reads);
    bb_line_dec(l, "changes", s->status.changes);
    bb_line_dec(l, "on", s->status.on);
    bb_line_end(l++);
    bb_line_start(l, "STATUS");
    bb_line_dec(l, "on_cntpct", s->status.on_at);
    bb_line_dec(l, "bounded", s->status.bounded);
    bb_line_dec(l, "last_cntpct", s->status.last_at);
    bb_line_end(l++);
    return (unsigned)(l - out);
}

void bb_step_line(const struct bb_step *s, struct bb_line *l)
{
    bb_line_start(l, "STEP");
    bb_line_dec(l, "cntpct", s->ticks);
    bb_line_dec(l, "irq", s->interrupts);
    bb_line_dec(l, "c", s->conditions);
    bb_line_dec(l, "b", s->bytes);
    bb_line_dec(l, "s", s->stops);
    bb_line_dec(l, "set", s->first_set);
    bb_line_dec(l, "clear", s->first_clear);
    bb_line_dec(l, "polls", s->polls_max);
    bb_line_dec(l, "respins", s->respins);
    bb_line_end(l);
}
void bb_access_line(uint32_t address, uint32_t value, unsigned write, uint64_t at, struct bb_line *l)
{
    bb_line_start(l, write ? "OWNER w" : "OWNER r");
    bb_line_hex(l, "a", address);
    bb_line_hex(l, "v", value);
    bb_line_dec(l, "cntpct", at);
    bb_line_end(l);
}
void bb_status_line(const struct bb_status_trace *t, unsigned i, struct bb_line *l)
{
    bb_line_start(l, "STATUS change");
    bb_line_hex(l, "value", t->change[i].value);
    bb_line_dec(l, "cntpct", t->change[i].at);
    bb_line_dec(l, "previous_read_cntpct", t->change[i].before);
    bb_line_end(l);
}
void bb_word_line(const char *prefix, uint32_t offset, uint32_t value, struct bb_line *l)
{
    bb_line_start(l, prefix);
    bb_line_hex(l, "offset", offset);
    bb_line_hex(l, "word", value);
    bb_line_end(l);
}
void bb_snapshot_line(unsigned i, uint32_t enable_selector, struct bb_line *l)
{
    bb_line_start(l, i & 1 ? "SNAPSHOT DCDC4" : "SNAPSHOT DCDC2");
    bb_line_dec(l, "index", i);
    bb_line_hex(l, "enable_selector", enable_selector);
    bb_line_end(l);
}
