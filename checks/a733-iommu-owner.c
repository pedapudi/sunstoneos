/* Host test of the OS-image IOMMU owner (drivers/iommu/a733_iommu_owner.c) against a register and page-walk model.
 * The model keeps every register word of both banks and walks a master's IOVA from its own bank's TTB through table
 * memory, as the hardware does. One session grants G2D (master 10) and the decoder (masters 6 and 7); all three sit
 * on bank 1, so they must share the one directory. Cases:
 *  1 G2D attaches first: the register sequence is the vendor hw_init order, touching only the micro sets of master
 *    10 (translated) and USB master 0 (bypassed); both banks' TTB name the one directory and both banks are enabled;
 *  2 the decoder attaches later: no TTB, enable or global flush write; the TLB and page-walk caches are invalidated
 *    over each new span; masters 6 and 7 stop bypassing; one directory allocation in total;
 *  3 every page of every span walks to its physical address from bank 1's TTB, with its access bits; G2D's pages
 *    still walk after the decoder's attach;
 *  4 an overlapping span, an absent master and an attached master are refused with no register access;
 *  5 a bank already enabled at entry is refused with no write, and the clock lease is returned;
 *  6 a fault word recorded by scan sets the master's fault record;
 *  7 unmap removes the walk; map across a 4 MiB boundary adds one table per window;
 *  8 detaching G2D keeps the banks enabled; detaching the decoder last disables both, releases every table and the
 *    clock lease.
 * Built with -DMUTANT_SECOND_DIRECTORY (checks/a733-iommu-owner.py), the decoder's attach installs a second
 * directory; case 2 or 3 must fail. */
#include "a733_iommu_owner.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_PHYSICAL UINT64_C(0x48000000)
#define ARENA_BYTES (1u << 20)
#define REGS 0x20000u
static _Alignas(16384) uint8_t arena[ARENA_BYTES];
static size_t arena_used;
static unsigned allocations, releases, directories;
static uint32_t regs[REGS / 4];
static uint64_t now;
static int clock_held, clock_takes;
struct access { char op; uint32_t offset, value; };
static struct access trace[4096];
static unsigned traced;
static int failures;

#define CHECK(c) do { if(!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while(0)

static int now_us(void *c, uint64_t *t) { (void)c; *t = now; now += 3; return 1; }
static int rd(void *c, uint32_t off, uint32_t *v)
{
    (void)c;
    if(off >= REGS || (off & 3)) return -1;
    *v = regs[off / 4];
    if(traced < 4096) trace[traced++] = (struct access){'R', off, *v};
    return 1;
}
static int wr(void *c, uint32_t off, uint32_t v)
{
    (void)c;
    if(off >= REGS || (off & 3)) return -1;
    regs[off / 4] = v;
    if(traced < 4096) trace[traced++] = (struct access){'W', off, v};
    return 1;
}
static int table(void *c, size_t bytes, size_t align, uint32_t **cpu, uint64_t *pa)
{
    (void)c;
    size_t at = (arena_used + align - 1) & ~(align - 1);
    if(at + bytes > ARENA_BYTES) return -1;
    arena_used = at + bytes;
    memset(arena + at, 0xa5, bytes); /* the owner must zero what it uses */
    *cpu = (uint32_t *)(void *)(arena + at); *pa = ARENA_PHYSICAL + at;
    ++allocations;
    if(bytes > A733_IOMMU_OWNER_L2_BYTES) ++directories;
    return 1;
}
static void release(void *c, uint32_t *cpu, uint64_t pa, size_t bytes)
{
    (void)c; (void)bytes;
    if((uint8_t *)cpu - arena != (ptrdiff_t)(pa - ARENA_PHYSICAL)) { printf("FAIL release alias\n"); ++failures; }
    ++releases;
}
static int publish(void *c) { (void)c; return 1; }
static int clock_op(void *c, int take)
{
    (void)c;
    if(take) { if(clock_held) return -1; clock_held = 1; ++clock_takes; }
    else { if(!clock_held) return -1; clock_held = 0; }
    return 1;
}
static const struct a733_iommu_owner_ops ops = {now_us, rd, wr, table, release, publish, clock_op};

/* The hardware's walk for master id: its bank's ENABLE and TTB, the master's bypass, the directory entry, the leaf. */
static uint32_t word_at(uint64_t pa) { return *(uint32_t *)(void *)(arena + (pa - ARENA_PHYSICAL)); }
static uint64_t entry_pa(uint32_t e) { return (uint64_t)(e & 0xfffff000u) | (uint64_t)(e & 0x300u) << 24; }
static int walk(unsigned id, uint64_t iova, unsigned access, uint64_t *pa)
{
    const struct a733_iommu_master *m = a733_iommu_master(id);
    const uint32_t bank = m->bank * 0x10000u;
    if(regs[(bank + 0x14) / 4] != 1 || regs[(m->micro_offset + 4) / 4] != 0) return -1;
    const uint64_t dir = (uint64_t)regs[(bank + 0x20) / 4] << 14;
    if(dir < ARENA_PHYSICAL || dir >= ARENA_PHYSICAL + ARENA_BYTES) return -1;
    const uint32_t d = word_at(dir + (iova >> 22) * 4);
    if((d & 3) != 1) return -1;
    const uint32_t e = word_at(entry_pa(d) + ((iova >> 12) & 1023) * 4);
    if((e & 3) != 2) return -1;
    const unsigned granted = ((e & 4) ? A733_IOMMU_READ : 0) | ((e & 8) ? A733_IOMMU_WRITE : 0);
    if((granted & access) != access) return -1;
    *pa = entry_pa(e) | (iova & 4095);
    return 1;
}
static int walks(unsigned id, const struct a733_iommu_span *s, unsigned n)
{
    for(unsigned i = 0; i < n; ++i)
        for(uint64_t off = 0; off < s[i].bytes; off += 4096) {
            uint64_t pa;
            if(walk(id, s[i].iova + off, s[i].access, &pa) != 1 || pa != s[i].physical + off) return 0;
        }
    return 1;
}
static int wrote(uint32_t off, unsigned from)
{
    for(unsigned i = from; i < traced; ++i) if(trace[i].op == 'W' && trace[i].offset == off) return 1;
    return 0;
}
static unsigned writes_since(unsigned from)
{
    unsigned n = 0;
    for(unsigned i = from; i < traced; ++i) n += trace[i].op == 'W';
    return n;
}

int main(void)
{
    static struct a733_iommu_owner o;
    CHECK(a733_iommu_owner_init(&o, &ops, NULL) == 1);
    /* The owner's micro offsets against drivers/iommu/a733_iommu.c's table: each real master's interrupt enable is
     * the only micro word a first attach of that master alone writes before bypass. */
    for(unsigned id = 0; id < 12; ++id) {
        if(!(A733_IOMMU_REAL_MASTERS >> id & 1)) continue;
        static struct a733_iommu_owner p;
        const struct a733_iommu_span one[] = {{0x1000, 0x50000000, 0x1000, A733_IOMMU_READ}};
        memset(&p, 0, sizeof(p));
        CHECK(a733_iommu_owner_init(&p, &ops, NULL) == 1);
        const unsigned at = traced;
        CHECK(a733_iommu_owner_attach(&p, (uint16_t)(1u << id), one, 1) == 1);
        CHECK(wrote(a733_iommu_master(id)->micro_offset + 0x30, at) && wrote(a733_iommu_master(id)->micro_offset + 4, at));
        CHECK(a733_iommu_owner_detach(&p, (uint16_t)(1u << id)) == 1);
    }
    traced = 0; arena_used = 0; allocations = releases = directories = 0; clock_takes = 0; memset(regs, 0, sizeof(regs));
    /* G2D's source, output and command spans (drivers/g2d/a733_g2d.c IOVAs), at physical pages the arena does not use. */
    const struct a733_iommu_span g2d[] = {
        {0x10000000, 0x50000000, 0x100000, A733_IOMMU_READ},
        {0x10400000, 0x50200000, 0x100000, A733_IOMMU_WRITE},
        {0x10800000, 0x50400000, 0x2000, A733_IOMMU_READ},
    };
    const struct a733_iommu_span decoder[] = {
        {0x20000000, 0x51000000, 0x80000, A733_IOMMU_READ},
        {0x20400000, 0x51200000, 0x300000, A733_IOMMU_READ | A733_IOMMU_WRITE},
    };

    /* 1 */
    unsigned mark = traced;
    CHECK(a733_iommu_owner_attach(&o, 1u << 10, g2d, 3) == 1);
    CHECK(o.state == A733_IOMMU_OWNER_ACTIVE && clock_held && directories == 1);
    {
        const uint32_t mic = 0x10c00, dirw = (uint32_t)(o.directory_physical >> 14);
        static const struct access want[] = {
            {'R', 0x14, 0}, {'R', 0x88, 0}, {'R', 0xb0, 0}, {'R', 0xb4, 0},
            {'R', 0x10014, 0}, {'R', 0x10088, 0}, {'R', 0x100b0, 0}, {'R', 0x100b4, 0},
            {'R', 0x438, 0}, {'R', 0x404, 0}, {'R', 0x10c38, 0}, {'R', 0x10c04, 0},
        };
        unsigned i = mark, k;
        for(k = 0; k < sizeof(want) / sizeof(want[0]); ++k, ++i)
            CHECK(trace[i].op == want[k].op && trace[i].offset == want[k].offset);
        /* Each write then its readback; the flush requests have none. USB (master 0) is set to bypass. */
        const struct { uint32_t off, value; int back; } seq[] = {
            {0x20, dirw, 1}, {0x10020, dirw, 1}, {0x2c, 0x3007f, 1}, {0x1002c, 0x3007f, 1},
            {0x40, 1, 1}, {0x10040, 1, 1}, {0x60, 1, 1}, {0x10060, 1, 1}, {0x80, 0, 1}, {0x10080, 0, 1},
            {0x430, 0, 1}, {mic + 0x30, 0, 1}, {0x404, 1, 1}, {mic + 4, 0, 1},
            {0x30, 3, 0}, {0x418, 1, 0}, {0x10030, 3, 0}, {mic + 0x18, 1, 0},
            {0x30, 0, 1}, {0x10030, 0, 1}, {0x1c, 0x7fff0003, 1}, {0x1001c, 0x7fff0003, 1}, {0x14, 1, 1}, {0x10014, 1, 1},
        };
        for(k = 0; k < sizeof(seq) / sizeof(seq[0]); ++k) {
            CHECK(i < traced && trace[i].op == 'W' && trace[i].offset == seq[k].off && trace[i].value == seq[k].value);
            ++i;
            if(seq[k].back) { CHECK(i < traced && trace[i].op == 'R' && trace[i].offset == seq[k].off); ++i; }
        }
        CHECK(i == traced);
        CHECK(o.entry.recorded == 1 && o.entry.bypass[0] == 0 && o.entry.bypass[10] == 0);
        for(unsigned id = 1; id < 12; ++id)
            if(id != 10 && a733_iommu_master(id)) {
                const uint32_t m = a733_iommu_master(id)->micro_offset;
                for(unsigned j = mark; j < traced; ++j) CHECK(trace[j].offset < m || trace[j].offset >= m + 0x200);
            }
        CHECK(regs[0x20 / 4] == dirw && regs[0x10020 / 4] == dirw);
    }
    CHECK(walks(10, g2d, 3));

    /* 2 */
    const uint32_t ttb = regs[0x10020 / 4];
    mark = traced;
    CHECK(a733_iommu_owner_attach(&o, (1u << 6) | (1u << 7), decoder, 2) == 1);
    CHECK(!wrote(0x20, mark) && !wrote(0x10020, mark) && !wrote(0x14, mark) && !wrote(0x10014, mark) &&
          !wrote(0x30, mark) && !wrote(0x10030, mark));
    CHECK(regs[0x10020 / 4] == ttb && regs[0x20 / 4] == ttb);
    CHECK(directories == 1);
    CHECK(regs[(0x10400 + 4) / 4] == 0 && regs[(0x10600 + 4) / 4] == 0);
    {
        /* The second span's invalidation: TLB pages 0x20400..0x20700, page-walk windows 0x81..0x81 (end exclusive 0x81). */
        int tlb = 0, pc = 0;
        for(unsigned j = mark; j + 1 < traced; ++j) {
            if(trace[j].op == 'W' && trace[j].offset == 0x10044 && trace[j].value == 0x20400) tlb = 1;
            if(trace[j].op == 'W' && trace[j].offset == 0x10068 && trace[j].value == 0x81) pc = 1;
        }
        CHECK(tlb && pc);
    }

    /* 3 */
    CHECK(walks(6, decoder, 2) && walks(7, decoder, 2));
    CHECK(walks(10, g2d, 3));
    {
        uint64_t pa;
        CHECK(walk(10, 0x10000000, A733_IOMMU_WRITE, &pa) == -1);
        CHECK(a733_iommu_owner_translate(&o, 0x20400000 + 0x1234, A733_IOMMU_WRITE, &pa) == 1 && pa == 0x51201234);
    }

    /* 4 */
    mark = traced;
    const struct a733_iommu_span overlap[] = {{0x10001000, 0x52000000, 0x1000, A733_IOMMU_READ}};
    const struct a733_iommu_span fresh[] = {{0x30000000, 0x52000000, 0x1000, A733_IOMMU_READ}};
    CHECK(a733_iommu_owner_attach(&o, 1u << 9, overlap, 1) == -1);
    CHECK(a733_iommu_owner_attach(&o, 1u << 4, fresh, 1) == -1);
    CHECK(a733_iommu_owner_attach(&o, 1u << 10, fresh, 1) == -1);
    CHECK(a733_iommu_owner_map(&o, overlap, 1) == -1);
    CHECK(traced == mark && o.state == A733_IOMMU_OWNER_ACTIVE);

    /* 6 */
    regs[(0x10c00 + 0x38) / 4] = 0x1; regs[(0x10c00 + 0x3c) / 4] = 0x10000123;
    CHECK(a733_iommu_owner_scan(&o, 10) == 0 && (o.faulted & (1u << 10)) && o.faults[10].micro_low == 0x10000123);
    CHECK(a733_iommu_owner_scan(&o, 6) == 1 && !(o.faulted & (1u << 6)));
    regs[(0x10c00 + 0x38) / 4] = 0; regs[(0x10c00 + 0x3c) / 4] = 0;

    /* 7 */
    {
        const unsigned tables = o.tables;
        const struct a733_iommu_span cross[] = {{0x303fe000, 0x52000000, 0x4000, A733_IOMMU_READ | A733_IOMMU_WRITE}};
        CHECK(a733_iommu_owner_map(&o, cross, 1) == 1 && o.tables == tables + 2);
        CHECK(walks(7, cross, 1));
        CHECK(a733_iommu_owner_unmap(&o, &decoder[0], 1) == 1);
        uint64_t pa;
        CHECK(walk(6, 0x20000000, A733_IOMMU_READ, &pa) == -1 && walks(6, &decoder[1], 1));
        const struct a733_iommu_span wrong[] = {{0x20400000, 0x51300000, 0x1000, A733_IOMMU_READ}};
        CHECK(a733_iommu_owner_unmap(&o, wrong, 1) == -1);
    }

    /* 8 */
    CHECK(a733_iommu_owner_detach(&o, 1u << 10) == 1);
    CHECK(regs[0x14 / 4] == 1 && regs[0x10014 / 4] == 1 && regs[(0x10c00 + 4) / 4] == 1 && clock_held);
    CHECK(walks(6, &decoder[1], 1));
    CHECK(a733_iommu_owner_detach(&o, 1u << 10) == -1);
    CHECK(a733_iommu_owner_detach(&o, (1u << 6) | (1u << 7)) == 1);
    CHECK(regs[0x14 / 4] == 0 && regs[0x10014 / 4] == 0 && !clock_held && o.state == A733_IOMMU_OWNER_IDLE);
    CHECK(regs[0x404 / 4] == 1 && regs[(0x10400 + 4) / 4] == 1 && regs[(0x10600 + 4) / 4] == 1);
    CHECK(releases == allocations && o.directory == NULL);

    /* 5: a new session finds bank 0 enabled. */
    mark = traced;
    regs[0x14 / 4] = 1;
    CHECK(a733_iommu_owner_attach(&o, 1u << 10, g2d, 3) == -1);
    CHECK(writes_since(mark) == 0 && !clock_held && o.state == A733_IOMMU_OWNER_IDLE && o.attached == 0);
    CHECK(o.refused_word == 2 && o.fault_step == 0 && o.entry.enable[0] == 1);
    regs[0x14 / 4] = 0;
    CHECK(a733_iommu_owner_attach(&o, 1u << 10, g2d, 3) == 1 && walks(10, g2d, 3) && clock_takes == 3);
    CHECK(a733_iommu_owner_detach(&o, 1u << 10) == 1 && releases == allocations);

    printf("%s: %u register accesses, %u table allocations, %u directories\n", failures ? "FAIL" : "PASS", traced,
           allocations, directories);
    return failures ? 1 : 0;
}
