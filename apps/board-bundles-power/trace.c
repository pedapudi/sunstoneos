/* SPDX-License-Identifier: Apache-2.0 */
/* Records and log lines of bundles 6 and 7; see trace.h. */
#include "trace.h"

void bb_twi_observe(struct bb_twi *t, unsigned write, uintptr_t address, uint32_t value)
{
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
        if (t->polls > t->polls_max) t->polls_max = t->polls;
        if (t->condition) ++t->conditions;
        else ++t->bytes;
        t->mode = 0;
    } else if (t->mode == 2 && !(value & 0x10)) {
        ++t->stops;
        t->mode = 0;
    }
}

void bb_step_bins(const struct bb_step *s, unsigned count, uint32_t bit_ns, struct bb_step_bins *b)
{
    *b = (struct bb_step_bins){{0}, 0, 0};
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t ns = s[i].ticks * 125 / 3; /* 24 MHz counter ticks */
        const uint64_t wire = bb_twi_wire_bits(s[i].conditions, s[i].bytes, s[i].stops) * bit_ns;
        const uint64_t ratio = wire ? ns * 1000 / wire : 0;
        b->bins[ratio <= 1000 ? 0 : ratio <= 1050 ? 1 : ratio <= 1100 ? 2 : 3]++;
        b->over_ms += ns > 1100000;
        if (ratio > b->worst)
            b->worst = ratio;
    }
}

void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint64_t start, uint64_t bound, struct bb_status_trace *t)
{
    *t = (struct bb_status_trace){0};
    uint32_t previous = 0;
    uint64_t previous_at = 0;
    for (;;) {
        uint32_t v;
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
        previous = v;
        previous_at = at;
        t->last_at = at;
        if ((v & 15) == 8) {
            t->on = 1;
            t->on_at = at;
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
    bb_line_end(l++);
    bb_line_start(l, "STEPS of wire time");
    bb_line_dec(l, "le100", s->bins.bins[0]);
    bb_line_dec(l, "le105", s->bins.bins[1]);
    bb_line_dec(l, "le110", s->bins.bins[2]);
    bb_line_dec(l, "gt110", s->bins.bins[3]);
    bb_line_dec(l, "gt1100us", s->bins.over_ms);
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
