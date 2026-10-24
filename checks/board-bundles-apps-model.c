/* SPDX-License-Identifier: Apache-2.0 */
/* Host model of the board-bundle sequences that run without a QEMU model
 * (checks/board-bundles-apps.py builds and runs it).
 *
 * Bundle 8: apps/board-bundles-dram/sequence.c and drivers/dram/a733_dram.c
 * in table mode against a register file of R_CCU 0x1ac, the domain 1 status,
 * CCU +0x580 to +0x588 and the fields of NSI ports 2, 13, 14 and 15. Each
 * case checks the outcome, the skipped stage, the order of the accesses and
 * that no NSI access happens behind a closed gate or an OFF domain, and that
 * writes reach only ports 2, 14 and 15.
 *
 * Bundles 6 and 7: apps/board-bundles-power/trace.c. The TWI observer is fed
 * the S_TWI0 accesses of one modeled read transaction; the status spin is fed
 * a modeled status sequence; the step distribution is computed for modeled
 * steps. Every summary and detail line is formatted with each field at its
 * widest value: no line is cut, and the begin line, the summary, the outcome
 * line and the detail reserve of each image fit the 16,127 log bytes, so the
 * outcome line always reaches the log. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sequence.h"
#include "trace.h"

#define NSI(p, o) (A733_NSI_PORT(p) + (o))
struct access { uintptr_t address; uint32_t value; unsigned write; };
static struct model {
    uint32_t ppu, domain1, nsi_clock, nsi_config, mbus;
    uint32_t field[16][3];
    unsigned ignore_writes, alias, refuse_after;
    struct access log[128];
    unsigned n;
} m;

static uint32_t *word(uintptr_t a)
{
    static uint32_t none;
    switch (a) {
    case BB8_R_CCU_PPU: return &m.ppu;
    case BB8_DOMAIN1_STATUS: return &m.domain1;
    case A733_NSI_CLOCK: return &m.nsi_clock;
    case A733_NSI_CONFIG: return &m.nsi_config;
    case A733_MBUS_CLOCK: return &m.mbus;
    default: break;
    }
    for (unsigned p = 0; p < 16; ++p)
        for (unsigned o = 0; o < 3; ++o)
            if (a == NSI(p, 0x10 + 4 * o)) return &m.field[p][o];
    assert(!"access outside the model");
    return &none;
}
static int model_read(void *c, uintptr_t a, uint32_t *v)
{
    (void)c;
    if (m.refuse_after && m.n >= m.refuse_after) return 0;
    *v = *word(a);
    m.log[m.n++] = (struct access){a, *v, 0};
    return 1;
}
static int model_write(void *c, uintptr_t a, uint32_t v)
{
    (void)c;
    m.log[m.n++] = (struct access){a, v, 1};
    if (!m.ignore_writes) *word(a) = v;
    /* A decode that also hits port 13. */
    if (m.alias && a == NSI(2, 0x14)) m.field[13][1] = v;
    return 1;
}
static const struct a733_dram_io io = {NULL, model_read, model_write};

static void ready_model(void)
{
    memset(&m, 0, sizeof m);
    m.ppu = 1;
    m.domain1 = 8;
    m.nsi_clock = 0xc3000000;
    m.nsi_config = 0x00010001;
    m.mbus = 0x84000000;
    m.field[13][0] = 0x2;
    m.field[13][1] = 0x5;
    m.field[13][2] = 0x1;
}
static int nsi(uintptr_t a) { return a >= A733_NSI_BASE && a < A733_NSI_BASE + 0x10000; }
static unsigned nsi_accesses(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < m.n; ++i) n += nsi(m.log[i].address);
    return n;
}

static void gate_case(const char *name, void (*change)(void), enum bb8_skip skip, unsigned domain_read)
{
    struct bb8_record r;
    ready_model();
    change();
    assert(bb8_gates(&io, &r) == 0 && r.skipped == skip && r.domain1_read == domain_read);
    assert(nsi_accesses() == 0 && m.log[0].address == BB8_R_CCU_PPU);
    for (unsigned i = 0; i < m.n; ++i)
        assert(!m.log[i].write && (m.log[i].address != BB8_DOMAIN1_STATUS || domain_read));
    printf("\"%s\",", name);
}
static void ppu_closed(void) { m.ppu = 0; }
static void domain_off(void) { m.domain1 = 0; }
static void domain_transition(void) { m.domain1 = 4; }
static void nsi_gate_closed(void) { m.nsi_clock = 0x43000000; }
static void nsi_reset_asserted(void) { m.nsi_config = 0x00000001; }

static enum bb8_result run(struct bb8_record *r)
{
    assert(bb8_gates(&io, r) == 1 && r->skipped == BB8_READY && r->domain1_read);
    assert(m.n == 5 && !nsi_accesses());
    return bb8_nsi(&io, r);
}
static void written_case(void)
{
    struct bb8_record r;
    ready_model();
    assert(run(&r) == BB8_WRITTEN && r.possibly_protected);
    /* The first NSI accesses are the nine field reads, port 2 first, then the
     * three USB_PCIE reads. */
    for (unsigned i = 0; i < BB8_FIELDS; ++i)
        assert(m.log[5 + i].address == bb8_field(i) && !m.log[5 + i].write);
    for (unsigned i = 0; i < 3; ++i)
        assert(m.log[5 + BB8_FIELDS + i].address == NSI(13, 0x10 + 4 * i));
    unsigned writes = 0;
    for (unsigned i = 0; i < m.n; ++i)
        if (m.log[i].write) {
            ++writes;
            unsigned port = (unsigned)((m.log[i].address - A733_NSI_BASE) / 0x200);
            assert(nsi(m.log[i].address) && (port == 2 || port == 14 || port == 15));
        }
    /* Port 2 priority 2, ports 14 and 15 priority 3, mode 0 and select 1:
     * from zero fields, priority and select differ (mode already 0). */
    assert(writes == 6);
    assert(m.field[2][1] == 0xa && m.field[14][1] == 0xf && m.field[15][1] == 0xf);
    assert(m.field[2][2] == 1 && m.field[14][2] == 1 && m.field[15][2] == 1);
    assert(r.block.final[0] == (0xa << 2 | 1 << 6) && r.block.final[1] == (0xf << 2 | 1 << 6));
    assert(r.usb_pcie_after[1] == 0x5 && m.log[m.n - 1].address == NSI(13, 0x18));
    /* 5 gate reads, 12 reads, the hook's 2 CCU reads, 9 reads, 6 writes and
     * 6 readbacks, 3 USB_PCIE reads. */
    assert(m.n == 5 + 12 + 2 + 9 + 12 + 3);
    printf("\"table written from zero fields: 6 writes, ports 2, 14 and 15 only\",");
}
static void at_table_case(void)
{
    struct bb8_record r;
    ready_model();
    for (unsigned p = 0; p < 16; ++p) {
        m.field[p][1] = p == 2 ? 0xa : 0xf;
        m.field[p][2] = 1;
    }
    assert(run(&r) == BB8_WRITTEN && !r.possibly_protected);
    for (unsigned i = 0; i < m.n; ++i)
        assert(!m.log[i].write);
    printf("\"fields already at the table: no write\",");
}
static void mismatch_case(void)
{
    struct bb8_record r;
    ready_model();
    m.ignore_writes = 1;
    assert(run(&r) == BB8_MISMATCH && r.block.failed == 2);
    printf("\"writes ignored: readback mismatch at port 2 priority\",");
}
static void refused_case(void)
{
    struct bb8_record r;
    ready_model();
    /* The register pair refuses the hook's first readback, as when its
     * trace is full: a refused access, not a mismatch. */
    m.refuse_after = 5 + 12 + 2 + 2 + 1;
    assert(bb8_gates(&io, &r) == 1);
    assert(bb8_nsi(&io, &r) == BB8_ACCESS_FAILED && r.block.failed == 2);
    printf("\"a refused hook access is ACCESS_FAILED, not a mismatch\",");
}
static void alias_case(void)
{
    struct bb8_record r;
    ready_model();
    m.alias = 1;
    assert(run(&r) == BB8_USB_PCIE_CHANGED);
    printf("\"a write reaching port 13 is detected\",");
}

static void twi_case(void)
{
    struct bb_twi t = {0};
    const uintptr_t cntr = BB_TWI_CNTR, stat = 0x07083010, data = 0x07083008;
    /* START, address, register, repeated START, read address, receive,
     * data, STOP: one of the six read transactions of a snapshot. */
    bb_twi_observe(&t, 0, cntr, 0x40);           /* idle check: not a phase */
    bb_twi_observe(&t, 1, cntr, 0x68);
    bb_twi_observe(&t, 0, cntr, 0x60);           /* flag clear */
    bb_twi_observe(&t, 0, cntr, 0x48);           /* flag set */
    bb_twi_observe(&t, 0, stat, 0x08);
    for (unsigned i = 0; i < 2; ++i) {
        bb_twi_observe(&t, 1, data, 0x6c);
        bb_twi_observe(&t, 1, cntr, 0x48);
        bb_twi_observe(&t, 0, cntr, 0x48);
    }
    bb_twi_observe(&t, 1, cntr, 0x68);
    bb_twi_observe(&t, 0, cntr, 0x48);
    for (unsigned i = 0; i < 2; ++i) {
        bb_twi_observe(&t, 1, cntr, 0x48);
        bb_twi_observe(&t, 0, cntr, 0x40);
        bb_twi_observe(&t, 0, cntr, 0x40);
        bb_twi_observe(&t, 0, cntr, 0x48);
    }
    bb_twi_observe(&t, 0, data, 0x03);
    bb_twi_observe(&t, 1, cntr, 0x58);
    bb_twi_observe(&t, 0, cntr, 0x50);           /* STOP pending */
    bb_twi_observe(&t, 0, cntr, 0x40);           /* STOP done */
    bb_twi_observe(&t, 0, cntr, 0x40);           /* idle check */
    assert(t.conditions == 2 && t.bytes == 4 && t.stops == 1 && t.mode == 0);
    assert(t.first_set == 3 && t.first_clear == 3 && t.polls_max == 3 && !t.enable_seen);
    assert(bb_twi_wire_bits(t.conditions, t.bytes, t.stops) == 42);
    bb_twi_observe(&t, 0, cntr, 0xc0);
    assert(t.enable_seen == 0x80 && t.conditions == 2);
    printf("\"one read transaction: 2 conditions, 4 bytes, 1 STOP, 42 bit times\",");
}

static const uint32_t *statuses;
static unsigned status_count, status_index;
static uint64_t clock_now;
static int status_read(void *c, uintptr_t a, uint32_t *v)
{
    (void)c;
    assert(a == 0x07061008);
    *v = statuses[status_index < status_count ? status_index : status_count - 1];
    ++status_index;
    return 1;
}
static uint64_t step_now(void) { return clock_now += 10; }
static void spin_case(void)
{
    static const uint32_t to_on[] = {0, 0, 0, 4, 4, 8};
    struct bb_status_trace t;
    statuses = to_on;
    status_count = 6;
    status_index = 0;
    clock_now = 1000;
    bb_status_spin(status_read, NULL, step_now, 0x07061008, 1000, 240000, &t);
    assert(t.on && !t.bounded && t.reads == 6 && t.changes == 3 && t.first == 0);
    assert(t.change[0].value == 0 && t.change[1].value == 4 && t.change[2].value == 8 && t.on_at == 60);
    assert(t.change[1].at == 40 && t.change[1].before == 30 && t.change[2].before == 50);
    static const uint32_t stuck[] = {0};
    statuses = stuck;
    status_count = 1;
    status_index = 0;
    clock_now = 0;
    bb_status_spin(status_read, NULL, step_now, 0x07061008, 0, 100, &t);
    assert(!t.on && t.bounded && t.reads == 10 && t.changes == 1 && t.last_at == 100);
    printf("\"status spin: ON after an intermediate value; bounded when it never changes\"");
}

static void bins_case(void)
{
    /* 400 kHz, 2,500 ns bits: one snapshot is 252 bit times, 630,000 ns. */
    const struct bb_step steps[3] = {{15120, 0, 12, 24, 6, 20, 16, 2},  /* 630,000 ns: 100% */
                                     {16632, 1, 12, 24, 6, 20, 16, 2},  /* 693,000 ns: 110% */
                                     {30000, 1, 12, 24, 6, 20, 16, 2}}; /* 1,250,000 ns */
    struct bb_step_bins b;
    bb_step_bins(steps, 3, 2500, &b);
    assert(b.bins[0] == 1 && b.bins[2] == 1 && b.bins[3] == 1 && b.over_ms == 1 && b.worst == 1984);
    printf("\"step distribution against the wire time\",");
}

static size_t length(const struct bb_line *l)
{
    assert(l->n == strlen(l->text) && l->n + 1 < BB_LINE_MAX && l->text[l->n - 1] == '\n');
    return l->n;
}
static void log_bound_case(void)
{
    const size_t room = 16384 - 256 - 1, reserve = 2 * BB_LINE_MAX;
    struct bb_line lines[16];
    struct bb6_summary s6;
    memset(&s6, 0xff, sizeof s6);
    for (unsigned i = 0; i < 3; ++i) s6.check[i] = 1;
    size_t six = strlen(BB6_BEGIN) + strlen(BB6_COMPLETE) + reserve;
    const unsigned n6 = bb6_summary_lines(&s6, lines);
    assert(n6 == BB6_SUMMARY_LINES);
    for (unsigned i = 0; i < n6; ++i) six += length(&lines[i]);
    struct bb7_summary s7;
    memset(&s7, 0xff, sizeof s7);
    size_t seven = strlen(BB7_BEGIN) + strlen(BB7_REFUSED) + reserve;
    assert(strlen(BB7_REFUSED) >= strlen(BB7_ON) && strlen(BB7_REFUSED) >= strlen(BB7_FAILED));
    assert(strlen(BB7_TRACE_END) < BB_LINE_MAX);
    const unsigned n7 = bb7_summary_lines(&s7, lines);
    assert(n7 == BB7_SUMMARY_LINES);
    for (unsigned i = 0; i < n7; ++i) seven += length(&lines[i]);
    assert(six <= room && seven <= room);
    struct bb_step step;
    memset(&step, 0xff, sizeof step);
    bb_step_line(&step, &lines[0]);
    struct bb_status_trace t;
    memset(&t, 0xff, sizeof t);
    bb_status_line(&t, BB_STATUS_CHANGES - 1, &lines[1]);
    bb_access_line(UINT32_MAX, UINT32_MAX, 1, UINT64_MAX, &lines[2]);
    bb_word_line("BEFORE", UINT32_MAX, UINT32_MAX, &lines[3]);
    bb_snapshot_line(UINT32_MAX, UINT32_MAX, &lines[4]);
    size_t widest = 0;
    for (unsigned i = 0; i < 5; ++i)
        if (length(&lines[i]) > widest) widest = length(&lines[i]);
    printf("\"log bound: bundle 6 needs %zu and bundle 7 %zu of %zu bytes before its details; widest line %zu\",",
           six, seven, room, widest);
}

int main(void)
{
    printf("{\"cases\":[");
    gate_case("PPU clock closed: no PPU or NSI access", ppu_closed, BB8_SKIP_PPU_CLOCK, 0);
    gate_case("domain 1 OFF: no NSI access", domain_off, BB8_SKIP_DOMAIN1, 1);
    gate_case("domain 1 in transition: no NSI access", domain_transition, BB8_SKIP_DOMAIN1, 1);
    gate_case("NSI gate closed: no NSI access", nsi_gate_closed, BB8_SKIP_NSI_CLOCK, 1);
    gate_case("NSI reset asserted: no NSI access", nsi_reset_asserted, BB8_SKIP_NSI_CONFIG, 1);
    written_case();
    at_table_case();
    mismatch_case();
    refused_case();
    alias_case();
    bins_case();
    log_bound_case();
    twi_case();
    spin_case();
    printf("]}\nPASS\n");
    return 0;
}
