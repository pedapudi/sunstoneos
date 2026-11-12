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
 * image. The test writes one token word, in the caller's own kernel memory, and
 * only reads everywhere else: for each address bit of the DRAM offset and each
 * gigabyte multiple it reads the word at the offset with that bit flipped (or
 * that many gigabytes added) and requires that it is not the token. A decode that
 * ignores an address bit, or wraps at a size, maps that word onto the token's cell.
 * No application memory is written, so nothing the runner or the loader placed in
 * the probed ranges can change.
 *
 * The test runs at EL3 with the MMU off (Device-nGnRnE accesses, so no cache
 * holds the token and every read reaches the DRAM). It does not prove that Non-secure accesses pass the
 * memory firewall, which the qualification image's Non-secure run showed for
 * its tested map, and it does not prove that no other master writes there. */
#include "a733_dram.h"
#include "a733_device_memory_layout.h"

#define TESTED_FIRST UINT64_C(0x44000000)   /* first byte of the qualification image's tested map */
#define DRAM_BASE UINT64_C(0x40000000)
#define TOKEN_KEY UINT64_C(0x5ad3a5c3c5a35ac3)
#define GIB (UINT64_C(1) << 30)
#define BLOCK (UINT64_C(1) << 21)

static uint64_t token(uint64_t pa) { return (pa * UINT64_C(0x9e3779b97f4a7c15)) ^ TOKEN_KEY ^ (pa >> 17); }

/* `scratch` is a word of the caller's kernel memory (EL3 bss): the only word this test writes. Its physical address equals its
 * pointer value at EL3 with the MMU off. It must lie in DRAM and outside every range given, so that the write set and the
 * application ranges are disjoint by construction; the ranges are the memory that the caller is about to map for applications. */
int a733_dram_decode_probe(const struct a733_dram_probe_io *io, const struct a733_dram_range *ranges, unsigned count,
                           uint64_t scratch_pa, uint64_t scratch_words)
{
    if (!io || !io->read || !io->write || !io->barrier || !ranges || !count || !scratch_words ||
        (scratch_pa & 7) || scratch_pa < DRAM_BASE || scratch_pa + 8 > A733_DRAM_MAP_END) return 0;
    uint64_t previous_end = TESTED_FIRST;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t b = ranges[i].base, n = ranges[i].size;
        if (!n || (b | n) & (BLOCK - 1) || b < previous_end || b > A733_DRAM_MAP_END ||
            n > A733_DRAM_MAP_END - b) return 0;
        previous_end = b + n;
        if (scratch_pa < b + n && scratch_pa + 8 > b) return 0;     /* the scratch word is not application memory */
    }
    const uint64_t offset = scratch_pa - DRAM_BASE, span = A733_DRAM_MAP_END - DRAM_BASE;
    io->write(io->context, scratch_pa, token(scratch_pa));
    io->barrier(io->context);
    unsigned failed = io->read(io->context, scratch_pa) != token(scratch_pa);
    /* Partners: the offset with each address bit from 2 MiB to 8 GiB flipped, and the offset plus each whole gigabyte. */
    for (uint64_t bit = BLOCK; bit <= UINT64_C(1) << 33; bit <<= 1) {
        uint64_t partner = offset ^ bit;
        if (partner < span && partner != offset && io->read(io->context, DRAM_BASE + partner) == token(scratch_pa)) failed = 1;
    }
    for (uint64_t add = GIB; offset + add < span; add += GIB)
        if (io->read(io->context, DRAM_BASE + offset + add) == token(scratch_pa)) failed = 1;
    /* The first and last word of each range must not alias the scratch word either. */
    for (unsigned i = 0; i < count; ++i) {
        if (io->read(io->context, ranges[i].base) == token(scratch_pa)) failed = 1;
        if (io->read(io->context, ranges[i].base + ranges[i].size - 8) == token(scratch_pa)) failed = 1;
    }
    return !failed;
}

#ifndef A733_DRAM_DECODE_PROBE_HOST
static uint64_t hw_read(void *c, uint64_t pa) { (void)c; return *(volatile uint64_t *)(uintptr_t)pa; }
static void hw_write(void *c, uint64_t pa, uint64_t v) { (void)c; *(volatile uint64_t *)(uintptr_t)pa = v; }
static void hw_barrier(void *c) { (void)c; __asm__ volatile("dsb sy" ::: "memory"); }

int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned count, uint64_t *scratch, unsigned scratch_words)
{
    static const struct a733_dram_probe_io io = {0, hw_read, hw_write, hw_barrier};
    return a733_dram_decode_probe(&io, ranges, count, (uint64_t)(uintptr_t)scratch, scratch_words);
}
#endif
