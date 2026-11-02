/* Host check of the address-test DRAM decode check. The memory model decodes
 * addresses with a selectable fault and aborts on an access outside the
 * ranges the check was given, as a bus error would. */
#include "a733_dram.h"
#include "a733_device_memory_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define GIB (UINT64_C(1) << 30)
enum fault { LINEAR, WRAP_8GIB, IGNORE_BIT33, IGNORE_BIT30, STUCK_BIT21, TRUNCATED_10GIB, ABSENT_TOP, DROP_WRITES };
struct cell { uint64_t address, value; };
static struct cell cells[4096];
static unsigned used;
static enum fault fault;
static const struct a733_dram_range *allowed;
static unsigned allowed_count;
static unsigned reads, writes;

static uint64_t decode(uint64_t pa)
{
    switch (fault) {
    case WRAP_8GIB: return (pa - UINT64_C(0x40000000)) % (8 * GIB);
    case IGNORE_BIT33: return pa & ~(UINT64_C(1) << 33);
    case IGNORE_BIT30: return pa & ~(UINT64_C(1) << 30);
    case STUCK_BIT21: return pa & ~(UINT64_C(1) << 21);
    default: return pa;
    }
}
static void check_allowed(uint64_t pa)
{
    for (unsigned i = 0; i < allowed_count; ++i)
        if (pa >= allowed[i].base && pa + 8 <= allowed[i].base + allowed[i].size) return;
    fprintf(stderr, "access outside the given ranges: %#llx\n", (unsigned long long)pa);
    abort();
}
static uint64_t rd(void *c, uint64_t pa)
{
    (void)c; check_allowed(pa); ++reads;
    assert(!(pa & 7));
    if ((fault == TRUNCATED_10GIB && pa >= UINT64_C(0x40000000) + 10 * GIB) ||
        (fault == ABSENT_TOP && pa >= UINT64_C(0x340000000) - GIB)) return 0;
    uint64_t d = decode(pa);
    for (unsigned i = 0; i < used; ++i) if (cells[i].address == d) return cells[i].value;
    return 0xdeadbeefdeadbeefull;
}
static void wr(void *c, uint64_t pa, uint64_t v)
{
    (void)c; check_allowed(pa); ++writes;
    assert(!(pa & 7));
    if (fault == DROP_WRITES) return;
    if ((fault == TRUNCATED_10GIB && pa >= UINT64_C(0x40000000) + 10 * GIB) ||
        (fault == ABSENT_TOP && pa >= UINT64_C(0x340000000) - GIB)) return;
    uint64_t d = decode(pa);
    for (unsigned i = 0; i < used; ++i) if (cells[i].address == d) { cells[i].value = v; return; }
    assert(used < 4096);
    cells[used++] = (struct cell){d, v};
}
static unsigned barriers;
static void bar(void *c) { (void)c; ++barriers; }

static int run(enum fault f, const struct a733_dram_range *r, unsigned n)
{
    used = reads = writes = barriers = 0; fault = f; allowed = r; allowed_count = n;
    static const struct a733_dram_probe_io io = {0, rd, wr, bar};
    return a733_dram_decode_probe(&io, r, n);
}

int main(void)
{
    static const struct a733_dram_range high[] = {{UINT64_C(0x80000000), A733_DRAM_MAP_END - UINT64_C(0x80000000)}};
    static const struct a733_dram_range both[] = {{UINT64_C(0x48000000), UINT64_C(0x31000000)},
                                                  {UINT64_C(0x80000000), A733_DRAM_MAP_END - UINT64_C(0x80000000)}};
    /* The linear decode passes, in both range sets, with one barrier between the passes and
     * the same number of reads and writes within a bounded probe count. */
    assert(run(LINEAR, high, 1) && barriers == 1 && reads == writes && writes > 500 && writes < 700);
    unsigned high_probes = writes;
    assert(run(LINEAR, both, 2) && barriers == 1 && reads == writes && writes > high_probes);
    /* Each decode fault fails the check. */
    const char *names[] = {"linear", "wrap at 8 GiB", "bit 33 ignored", "bit 30 ignored", "bit 21 stuck",
                           "truncated at 10 GiB", "top gigabyte absent", "writes dropped"};
    for (enum fault f = WRAP_8GIB; f <= DROP_WRITES; ++f)
        if (run(f, high, 1)) { fprintf(stderr, "FAIL: %s passed\n", names[f]); return 1; }
    assert(!run(WRAP_8GIB, both, 2));
    /* Refusals touch nothing. */
    static const struct a733_dram_range below[] = {{UINT64_C(0x40000000), 0x4000000}};
    static const struct a733_dram_range unaligned[] = {{UINT64_C(0x80001000), 0x200000}};
    static const struct a733_dram_range past[] = {{UINT64_C(0x80000000), A733_DRAM_MAP_END}};
    static const struct a733_dram_range unsorted[] = {{UINT64_C(0x90000000), 0x200000}, {UINT64_C(0x80000000), 0x200000}};
    static const struct a733_dram_range empty[] = {{UINT64_C(0x80000000), 0}};
    assert(!run(LINEAR, below, 1) && !writes && !reads);
    assert(!run(LINEAR, unaligned, 1) && !writes);
    assert(!run(LINEAR, past, 1) && !writes);
    assert(!run(LINEAR, unsorted, 2) && !writes);
    assert(!run(LINEAR, empty, 1) && !writes);
    assert(!run(LINEAR, high, 0) && !writes);
    static const struct a733_dram_probe_io none = {0, 0, 0, 0};
    assert(!a733_dram_decode_probe(&none, high, 1));
    /* A small range is probed inside itself only (the model aborts otherwise). */
    static const struct a733_dram_range tiny[] = {{UINT64_C(0x80000000), 0x400000}};
    assert(run(LINEAR, tiny, 1));
    puts("dram-decode-probe host check passed");
    return 0;
}
