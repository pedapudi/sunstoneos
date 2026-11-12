/* Host check of the address-test DRAM decode check. The memory model decodes addresses with a selectable fault. The probe
 * may write only its scratch word, a word of kernel memory outside every range it is given; every other access must be a read.
 * The check records every write and proves the write set is disjoint from the ranges and from each application region. */
#include "a733_dram.h"
#include "a733_device_memory_layout.h"
#include "app_region.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define GIB (UINT64_C(1) << 30)
#define BASE UINT64_C(0x40000000)
enum fault { LINEAR, WRAP_8GIB, IGNORE_BIT33, IGNORE_BIT30, STUCK_BIT21, WRAP_10GIB, DROP_WRITES, IGNORE_BIT24, ABSENT_TOP, TRUNCATED_10GIB };
struct cell { uint64_t address, value; };
static struct cell cells[64];
static unsigned used, reads, writes, barriers;
static uint64_t written[16];
#define TOP_WORD (A733_DRAM_MAP_END - 8)
static enum fault fault;

static uint64_t initial(uint64_t cell)
{
    uint64_t z = cell * UINT64_C(0x9e3779b97f4a7c15) + UINT64_C(0x1234567);
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
/* The cell an address reaches: the DRAM offset passes through the fault. */
static uint64_t decode(uint64_t pa)
{
    uint64_t off = pa - BASE;
    switch (fault) {
    case WRAP_8GIB: off %= 8 * GIB; break;
    case WRAP_10GIB: off %= 10 * GIB; break;
    case IGNORE_BIT33: off &= ~(UINT64_C(1) << 33); break;
    case IGNORE_BIT30: off &= ~(UINT64_C(1) << 30); break;
    case STUCK_BIT21: off &= ~(UINT64_C(1) << 21); break;
    case IGNORE_BIT24: off &= ~(UINT64_C(1) << 24); break;
    default: break;
    }
    return BASE + off;
}
static uint64_t rd(void *c, uint64_t pa)
{
    (void)c; ++reads; assert(!(pa & 7));
    if ((fault == ABSENT_TOP && pa >= A733_DRAM_MAP_END - GIB) || (fault == TRUNCATED_10GIB && pa >= BASE + 10 * GIB)) return 0;
    uint64_t d = decode(pa);
    for (unsigned i = 0; i < used; ++i) if (cells[i].address == d) return cells[i].value;
    return initial(d);
}
static void wr(void *c, uint64_t pa, uint64_t v)
{
    (void)c; assert(!(pa & 7) && writes < 16);
    written[writes++] = pa;
    if (fault == DROP_WRITES || (fault == ABSENT_TOP && pa >= A733_DRAM_MAP_END - GIB) || (fault == TRUNCATED_10GIB && pa >= BASE + 10 * GIB)) return;
    uint64_t d = decode(pa);
    for (unsigned i = 0; i < used; ++i) if (cells[i].address == d) { cells[i].value = v; return; }
    assert(used < 64);
    cells[used++] = (struct cell){d, v};
}
static void bar(void *c) { (void)c; ++barriers; }

#define SCRATCH UINT64_C(0x41234560)   /* EL3 bss: kernel memory, below the application ranges */
static int run(enum fault f, const struct a733_dram_range *r, unsigned n)
{
    used = reads = writes = barriers = 0; fault = f;
    static const struct a733_dram_probe_io io = {0, rd, wr, bar};
    return a733_dram_decode_probe(&io, r, n, SCRATCH, 1);
}
/* Every application region of the image: the window the loader fills, the slot 1 area and all of upper memory, as the ledger knows them. */
static void disjoint(const struct a733_dram_range *r, unsigned n)
{
    for (unsigned w = 0; w < writes; ++w) {
        assert(written[w] == SCRATCH || written[w] == TOP_WORD);
        for (unsigned i = 0; i < n; ++i) assert(written[w] + 8 <= r[i].base || r[i].base + r[i].size <= written[w]);
        /* Below the window the loader fills (0x44000000) and so below every application region. */
        /* The scratch word lies below the window the loader fills; the top word lies in the reserved last block, past every range. */
        assert(written[w] == TOP_WORD ? written[w] >= A733_DRAM_APP_END : (written[w] >= BASE && written[w] < UINT64_C(0x44000000)));
    }
}

int main(void)
{
    static const struct a733_dram_range high[] = {{UINT64_C(0x80000000), A733_DRAM_APP_END - UINT64_C(0x80000000)}};
    static const struct a733_dram_range both[] = {{UINT64_C(0x48000000), UINT64_C(0x31000000)},
                                                  {UINT64_C(0x80000000), A733_DRAM_APP_END - UINT64_C(0x80000000)}};
    /* The linear decode passes. Exactly one write, to the scratch word, and it lies in no range; the rest are reads. */
    assert(run(LINEAR, high, 1) && writes == 2 && barriers == 2 && reads > 20);
    disjoint(high, 1);
    assert(run(LINEAR, both, 2) && writes == 2);
    disjoint(both, 2);
    printf("probe: 2 writes (scratch word %#llx and the top word), %u reads for the cached image's table\n", (unsigned long long)SCRATCH, reads);
    /* An INPUT region at 0x48000000 (the graph region of the recognition application) keeps its bytes: no write reaches it. */
    for (unsigned i = 0; i < used; ++i) assert(cells[i].address < UINT64_C(0x44000000) || cells[i].address == TOP_WORD);
    /* Each decode fault fails the check. */
    const char *names[] = {"linear", "wrap at 8 GiB", "bit 33 ignored", "bit 30 ignored", "bit 21 stuck", "wrap at 10 GiB", "writes dropped", "bit 24 ignored", "top gigabyte absent", "truncated at 10 GiB"};
    for (enum fault f = WRAP_8GIB; f <= TRUNCATED_10GIB; ++f)
        if (run(f, both, 2)) { fprintf(stderr, "FAIL: %s passed\n", names[f]); return 1; }
    /* Refusals write and read nothing. */
    static const struct a733_dram_range below[] = {{UINT64_C(0x40000000), 0x4000000}};
    static const struct a733_dram_range unaligned[] = {{UINT64_C(0x80001000), 0x200000}};
    static const struct a733_dram_range past[] = {{UINT64_C(0x80000000), A733_DRAM_MAP_END - UINT64_C(0x80000000)}};   /* includes the reserved top block */
    static const struct a733_dram_range unsorted[] = {{UINT64_C(0x90000000), 0x200000}, {UINT64_C(0x80000000), 0x200000}};
    static const struct a733_dram_range empty[] = {{UINT64_C(0x80000000), 0}};
    assert(!run(LINEAR, below, 1) && !writes && !reads);
    assert(!run(LINEAR, unaligned, 1) && !writes);
    assert(!run(LINEAR, past, 1) && !writes);
    assert(!run(LINEAR, unsorted, 2) && !writes);
    assert(!run(LINEAR, empty, 1) && !writes);
    assert(!run(LINEAR, high, 0) && !writes);
    static const struct a733_dram_probe_io none = {0, 0, 0, 0};
    assert(!a733_dram_decode_probe(&none, high, 1, SCRATCH, 1));
    /* A scratch word inside an application range, outside DRAM, or misaligned is refused before any access. */
    static const struct a733_dram_probe_io io = {0, rd, wr, bar};
    used = reads = writes = 0; fault = LINEAR;
    assert(!a733_dram_decode_probe(&io, both, 2, UINT64_C(0x48000008), 1) && !a733_dram_decode_probe(&io, both, 2, UINT64_C(0x90000000), 1) &&
           !a733_dram_decode_probe(&io, both, 2, SCRATCH + 4, 1) && !a733_dram_decode_probe(&io, both, 2, UINT64_C(0x1000), 1) &&
           !a733_dram_decode_probe(&io, both, 2, SCRATCH, 0) && !writes && !reads);
    puts("dram-decode-probe host check passed");
    return 0;
}
