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
