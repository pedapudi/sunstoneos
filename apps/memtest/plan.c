/* SPDX-License-Identifier: Apache-2.0 */
/* Contract admission and march planning. Pure functions: the image calls them
 * before any write outside the protected window, and checks/memtest-plan.py
 * calls the same object compiled for the host. */
#include "memtest.h"
#include "a733_range.h"

static uint64_t block_address(uint32_t block)
{ return MEMTEST_DRAM_BASE + ((uint64_t)block << MEMTEST_BLOCK_SHIFT); }

static uint64_t blocks_bytes(uint32_t blocks)
{ return (uint64_t)blocks << MEMTEST_BLOCK_SHIFT; }

/* Members other than any one total at least others_blocks. */
static int group_ok(const struct memtest_geometry *g, const struct memtest_piece *p, unsigned n)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < n; ++i) total += p[i].blocks;
    for (unsigned i = 0; i < n; ++i)
        if (total - p[i].blocks < g->others_blocks) return 0;
    return 1;
}

/* Cut the slice into members of piece_blocks from its start, then group them
 * consecutively. feasible[i] says pieces i.. can be grouped; the plan takes
 * the smaller group whenever the rest stays feasible, so a suffix of the plan
 * that a later run starts from is again feasible. */
static int plan_groups(const struct memtest_geometry *g, uint32_t start, uint32_t end,
                       struct memtest_plan *plan)
{
    unsigned n = 0;
    for (uint32_t block = start; block < end; ++n) {
        if (n == MEMTEST_PIECES_MAX) return 0;
        uint32_t blocks = end - block < g->piece_blocks ? end - block : g->piece_blocks;
        plan->piece[n] = (struct memtest_piece){block, blocks};
        block += blocks;
    }
    unsigned char feasible[MEMTEST_PIECES_MAX + 1] = {0};
    feasible[n] = 1;
    for (unsigned i = n; i-- > 0;)
        for (unsigned k = 2; k <= 3; ++k)
            if (i + k <= n && feasible[i + k] && group_ok(g, &plan->piece[i], k)) feasible[i] = 1;
    if (!feasible[0]) return 0;
    unsigned groups = 0;
    for (unsigned i = 0; i < n;) {
        unsigned k = i + 2 <= n && feasible[i + 2] && group_ok(g, &plan->piece[i], 2) ? 2 : 3;
        plan->group[groups++] = (struct memtest_group){i, k};
        i += k;
    }
    plan->pieces = n;
    plan->groups = groups;
    return 1;
}

static int excluded_hit(const struct memtest_contract *c, uint64_t address, uint64_t bytes)
{
    for (unsigned i = 0; i < c->excluded_count; ++i) {
        uint64_t start = block_address(c->excluded[i].start);
        if (a733_extents_intersect(start, blocks_bytes(c->excluded[i].end - c->excluded[i].start),
                                   address, bytes)) return 1;
    }
    return 0;
}

enum memtest_refusal memtest_admit(const struct memtest_geometry *g, const struct memtest_contract *c,
                                   const struct memtest_facts *f, struct memtest_plan *plan)
{
    if (!g || !c || !f || !plan || c->magic != MEMTEST_CONTRACT_MAGIC) return MEMTEST_REFUSE_CONTRACT;
    if (c->mask & ~MEMTEST_MASK_KNOWN) return MEMTEST_REFUSE_MASK;
    if (c->mask & ~MEMTEST_MASK_BUILT) return MEMTEST_REFUSE_MASK_UNBUILT;
    uint64_t map = block_address(g->first_block), map_bytes = blocks_bytes(g->end_block - g->first_block);
    if (c->excluded_count > MEMTEST_EXCLUDED_MAX) return MEMTEST_REFUSE_EXCLUDED_COUNT;
    for (unsigned i = 0; i < c->excluded_count; ++i) {
        const struct memtest_range *r = &c->excluded[i];
        if (r->start >= r->end) return MEMTEST_REFUSE_EXCLUDED_EMPTY;
        if (i && r->start < c->excluded[i - 1].end) return MEMTEST_REFUSE_EXCLUDED_ORDER;
        if (r->start < g->first_block || r->end > g->end_block) return MEMTEST_REFUSE_EXCLUDED_OUTSIDE;
    }
    if (c->mask & MEMTEST_MARCH) {
        if (c->slice_start >= c->slice_end) return MEMTEST_REFUSE_SLICE_EMPTY;
        uint64_t slice = block_address(c->slice_start), bytes = blocks_bytes(c->slice_end - c->slice_start);
        if (!a733_extent_within(map, map_bytes, slice, bytes)) return MEMTEST_REFUSE_SLICE_OUTSIDE;
        if (excluded_hit(c, slice, bytes)) return MEMTEST_REFUSE_SLICE_EXCLUDED;
    }
    for (unsigned i = 0; i < MEMTEST_FIXED_COUNT; ++i) {
        /* The address-line partners and harness buffers are written only when
         * their tests run, but admission keeps one rule for every extent. */
        const struct memtest_extent *e = &g->fixed[i];
        if (!e->bytes || !a733_extent_within(map, map_bytes, e->address, e->bytes))
            return MEMTEST_REFUSE_FIXED_OUTSIDE;
        if (excluded_hit(c, e->address, e->bytes)) return MEMTEST_REFUSE_FIXED_EXCLUDED;
    }
    plan->pieces = plan->groups = 0;
    if ((c->mask & MEMTEST_MARCH) && !plan_groups(g, c->slice_start, c->slice_end, plan))
        return MEMTEST_REFUSE_GROUPING;
    if (!MEMTEST_CLEAR_OF_K(c->seed)) return MEMTEST_REFUSE_SEED;
    if (!c->budget_ticks) return MEMTEST_REFUSE_BUDGET;
    plan->budget_ticks = c->budget_ticks < MEMTEST_BUDGET_LIMIT ? c->budget_ticks : MEMTEST_BUDGET_LIMIT;
    /* DC ZVA must be permitted (DZP clear) and zero exactly one 64-byte line. */
    if ((f->dczid & 0x1fu) != 4u) return MEMTEST_REFUSE_DCZID;
    /* The march's eviction rule streams 256 MiB through at most 8 MiB. */
    if (!f->dcache_bytes || f->dcache_bytes > (UINT64_C(8) << 20)) return MEMTEST_REFUSE_DCACHE;
    if ((f->writers_sample >> 32) != MEMTEST_WRITERS_MAGIC) return MEMTEST_REFUSE_WRITERS_UNSAMPLED;
    if ((uint32_t)f->writers_sample & ~MEMTEST_WRITERS_ACCEPTED) return MEMTEST_REFUSE_WRITERS;
    return MEMTEST_ADMITTED;
}

uint64_t memtest_dcache_bytes(uint64_t clidr, const uint64_t ccsidr[7], unsigned extended)
{
    uint64_t total = 0;
    for (unsigned level = 0; level < 7; ++level) {
        unsigned type = (unsigned)(clidr >> (3 * level)) & 7u;
        if (!type) break;
        if (type < 2) continue; /* instruction cache only */
        uint64_t c = ccsidr[level];
        uint64_t line = UINT64_C(16) << (c & 7u);
        uint64_t ways = (extended ? (c >> 3) & 0x1fffffu : (c >> 3) & 0x3ffu) + 1u;
        uint64_t sets = (extended ? (c >> 32) & 0xffffffu : (c >> 13) & 0x7fffu) + 1u;
        total += line * ways * sets;
    }
    return total;
}

/* Refuse all table flags, output aliases and noncanonical leaf flags except
 * the two execute-never bits. The low tested blocks share the returning
 * platform's executable mapping; all blocks must still be closed to EL0. */
static int table_page(uint64_t address)
{
    return !(address & 4095u) && address >= MEMTEST_DRAM_BASE &&
           address <= block_address(MEMTEST_WINDOW_BLOCKS) - 4096u;
}

uint64_t memtest_mapping_fault(const struct memtest_geometry *g, const struct memtest_mmu *m,
                               int (*read)(void *, uint64_t, uint64_t *), void *context)
{
    uint64_t first = g ? block_address(g->first_block) : MEMTEST_DRAM_BASE;
    if (!g || !m || !read || g->first_block < MEMTEST_WINDOW_BLOCKS ||
        g->first_block >= g->end_block || g->end_block > MEMTEST_DRAM_BLOCKS ||
        m->tcr != MEMTEST_TCR || m->mair != MEMTEST_MAIR ||
        (m->sctlr & (UINT64_C(1) << 25 | 0x1005u)) != 0x1005u ||
        !table_page(m->ttbr0)) return first;
    const uint64_t attributes = (UINT64_C(1) << 10) | (UINT64_C(3) << 8) | 5u;
    const uint64_t xn = (UINT64_C(1) << 53) | (UINT64_C(1) << 54);
    uint64_t end = block_address(g->end_block);
    for (uint64_t at = first; at < end;) {
        uint64_t entry;
        if (!read(context, m->ttbr0 + (at >> 30) * 8u, &entry)) return at;
        uint64_t limit = ((at >> 30) + 1u) << 30;
        if ((entry & 3u) == 1u) {
            if ((entry & ~xn) != ((at & ~((UINT64_C(1) << 30) - 1u)) | attributes)) return at;
            at = limit < end ? limit : end;
            continue;
        }
        /* Only a plain table pointer is accepted: subtracting the type also
         * leaves any reserved or hierarchical permission bits to be refused. */
        if ((entry & 3u) != 3u || !table_page(entry - 3u)) return at;
        uint64_t table = entry - 3u;
        while (at < end && at < limit) {
            if (!read(context, table + ((at >> 21) & 511u) * 8u, &entry) ||
                (entry & ~xn) != (at | attributes)) return at;
            at += MEMTEST_BLOCK_BYTES;
        }
    }
    return 0;
}
