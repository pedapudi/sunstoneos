/* DRAM decode check by address test (docs/designs/memory-map-12g.md,
 * docs/designs/large-app-regions.md). A strong definition of
 * a733_dram_decode_check() for images that opt in; it replaces the weak
 * refusal in the table builder. It is not linked together with
 * a733_dram_decode.c, which holds the definition for a register source that
 * does not exist.
 *
 * Basis. The memory qualification image maps 12,224 MiB from 0x44000000 at
 * EL1 as Normal write-back memory and checks that every 2 MiB page holds only
 * its own signature (docs/evidence/memory-dma.jsonl, scan and signature
 * passed on both training records' 12,288 MiB configuration). That evidence
 * covers the tested map, so a range outside it is refused. The test below
 * repeats the aliasing part of that evidence on every boot, because the DRAM
 * controller configuration (its size and address mapping) comes from the
 * training or restore record that the boot applied, and a different record or
 * a different DRAM population would break linear decode without changing the
 * image. The test writes a distinct token to each probe address and reads
 * every token back; two addresses that decode to the same cells, a wrapped
 * address and an absent cell all return another token.
 *
 * The test runs at EL3 with the MMU off (Device-nGnRnE accesses, so no cache
 * holds the tokens, and the restored words reach DRAM the same way, behind the
 * same kind of barrier). It does not prove that Non-secure accesses pass the
 * memory firewall, which the qualification image's Non-secure run showed for
 * its tested map, and it does not prove that no other master writes there. */
#include "a733_dram.h"
#include "a733_device_memory_layout.h"

#define TESTED_FIRST UINT64_C(0x44000000)   /* first byte of the qualification image's tested map */
#define TOKEN_KEY UINT64_C(0x5ad3a5c3c5a35ac3)
#define GIB (UINT64_C(1) << 30)
#define BLOCK (UINT64_C(1) << 21)

/* The probe leaves every word it touches as it found it: the runner places INPUT regions in DRAM before the launch, in the very
 * ranges this test covers, and the application's admission reads them afterwards. For each probe address the write pass reads
 * the word, saves it and writes the token; after the last compare the restore pass writes the saved words back in the reverse of
 * the order in which they were taken. The order matters when the decode aliases two addresses: the later address saved the earlier
 * one's token, so restoring it first and the earlier one last leaves the true word in the cell.
 *
 * The saved words are the caller's buffer, sized at build time from the same range table (A733_DRAM_PROBE_WORDS in a733_dram.h).
 * The count is computed first and a buffer that is too small refuses before any access. */
static uint64_t token(uint64_t pa) { return (pa * UINT64_C(0x9e3779b97f4a7c15)) ^ TOKEN_KEY ^ (pa >> 17); }

enum pass { WRITE, VERIFY, COUNT, RESTORE };
struct probe_walk { const struct a733_dram_probe_io *io; uint64_t *saved; unsigned failed; uint64_t count, index; };

/* Visit one probe address of a range. */
static void visit(struct probe_walk *w, enum pass pass, uint64_t pa)
{
    switch (pass) {
    case WRITE:
        w->saved[w->index++] = w->io->read(w->io->context, pa);
        w->io->write(w->io->context, pa, token(pa));
        break;
    case VERIFY:
        if (w->io->read(w->io->context, pa) != token(pa)) w->failed = 1;
        break;
    case RESTORE:
        w->io->write(w->io->context, pa, w->saved[--w->index]);
        break;
    case COUNT:
        break;
    }
    ++w->count;
}

/* Probe addresses of one range: its first and last word, each 2 MiB step inside its first gigabyte, each power-of-two offset
 * from 2 MiB to 8 GiB (each address line the 12 GiB map decodes), and each gigabyte boundary. */
static void sweep(struct probe_walk *w, enum pass pass, uint64_t base, uint64_t end)
{
    visit(w, pass, base);
    visit(w, pass, end - 8);
    for (uint64_t pa = (base + BLOCK) & ~(BLOCK - 1); pa < end && pa - base < GIB; pa += BLOCK) visit(w, pass, pa);
    for (uint64_t bit = BLOCK; bit <= UINT64_C(1) << 33; bit <<= 1) {
        uint64_t pa = base + bit;
        if (pa < end) visit(w, pass, pa);
    }
    for (uint64_t pa = (base + GIB) & ~(GIB - 1); pa < end; pa += GIB) visit(w, pass, pa);
}

/* The same addresses in the opposite order. */
static void sweep_reverse(struct probe_walk *w, uint64_t base, uint64_t end)
{
    uint64_t gibs = 0, steps = 0;
    for (uint64_t pa = (base + GIB) & ~(GIB - 1); pa < end; pa += GIB) ++gibs;
    for (uint64_t pa = (base + BLOCK) & ~(BLOCK - 1); pa < end && pa - base < GIB; pa += BLOCK) ++steps;
    const uint64_t first_gib = (base + GIB) & ~(GIB - 1), first_step = (base + BLOCK) & ~(BLOCK - 1);
    while (gibs) visit(w, RESTORE, first_gib + --gibs * GIB);
    for (uint64_t bit = UINT64_C(1) << 33; bit >= BLOCK; bit >>= 1) {
        uint64_t pa = base + bit;
        if (pa < end) visit(w, RESTORE, pa);
    }
    while (steps) visit(w, RESTORE, first_step + --steps * BLOCK);
    visit(w, RESTORE, end - 8);
    visit(w, RESTORE, base);
}

int a733_dram_decode_probe(const struct a733_dram_probe_io *io, const struct a733_dram_range *ranges, unsigned count,
                           uint64_t *saved, uint64_t saved_words)
{
    if (!io || !io->read || !io->write || !io->barrier || !ranges || !count) return 0;
    uint64_t previous_end = TESTED_FIRST;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t b = ranges[i].base, n = ranges[i].size;
        if (!n || (b | n) & (BLOCK - 1) || b < previous_end || b > A733_DRAM_MAP_END ||
            n > A733_DRAM_MAP_END - b) return 0;
        previous_end = b + n;
    }
    struct probe_walk w = {io, saved, 0, 0, 0};
    for (unsigned i = 0; i < count; ++i) sweep(&w, COUNT, ranges[i].base, ranges[i].base + ranges[i].size);
    if (!saved || w.count > saved_words) return 0;      /* before any access */
    const uint64_t total = w.count;
    w.count = 0;
    for (unsigned i = 0; i < count; ++i) sweep(&w, WRITE, ranges[i].base, ranges[i].base + ranges[i].size);
    io->barrier(io->context);
    for (unsigned i = 0; i < count; ++i) sweep(&w, VERIFY, ranges[i].base, ranges[i].base + ranges[i].size);
    w.index = total;
    for (unsigned i = count; i-- > 0;) sweep_reverse(&w, ranges[i].base, ranges[i].base + ranges[i].size);
    io->barrier(io->context);   /* the restores complete before the table builder maps the memory */
    return !w.failed;
}

#ifndef A733_DRAM_DECODE_PROBE_HOST
static uint64_t hw_read(void *c, uint64_t pa) { (void)c; return *(volatile uint64_t *)(uintptr_t)pa; }
static void hw_write(void *c, uint64_t pa, uint64_t v) { (void)c; *(volatile uint64_t *)(uintptr_t)pa = v; }
static void hw_barrier(void *c) { (void)c; __asm__ volatile("dsb sy" ::: "memory"); }

int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned count, uint64_t *saved, unsigned saved_words)
{
    static const struct a733_dram_probe_io io = {0, hw_read, hw_write, hw_barrier};
    return a733_dram_decode_probe(&io, ranges, count, saved, saved_words);
}
#endif
