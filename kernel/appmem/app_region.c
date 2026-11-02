#include "app_region.h"
#include <string.h>

#define SLOT_FIRST 2u                 /* level-1 slot of APP_REGION_BASE */
#define SLOT_END 13u                  /* slot after APP_REGION_LIMIT */
#define BLOCKS_PER_SLOT 512u

static int aligned(uint64_t v, uint64_t a) { return (v & (a - 1)) == 0; }

int app_region_ledger_init(struct app_region_ledger *l, struct app_region_range *usable_storage, unsigned usable_capacity,
                           struct app_region_entry *entry_storage, unsigned entry_capacity,
                           const struct app_region_range *r, unsigned count)
{
    if (!l || !usable_storage || (entry_capacity && !entry_storage) || count > usable_capacity || (count && !r) ||
        entry_capacity > APP_REGION_MAX_REGIONS) return APP_REGION_INVALID;
    uint64_t previous_end = APP_REGION_BASE;
    for (unsigned i = 0; i < count; ++i) {
        if (!r[i].bytes || !aligned(r[i].base, APP_REGION_BLOCK) || !aligned(r[i].bytes, APP_REGION_BLOCK) ||
            r[i].base < previous_end || r[i].base > APP_REGION_LIMIT ||
            r[i].bytes > APP_REGION_LIMIT - r[i].base) return APP_REGION_INVALID;
        previous_end = r[i].base + r[i].bytes;
    }
    memset(l, 0, sizeof *l);
    l->usable = usable_storage; l->usable_capacity = usable_capacity;
    l->entry = entry_storage; l->capacity = entry_capacity;
    if (entry_capacity) memset(entry_storage, 0, sizeof *entry_storage * entry_capacity);
    for (unsigned i = 0; i < count; ++i) l->usable[i] = r[i];
    l->usable_count = count;
    return APP_REGION_OK;
}

/* The free gaps in address order: usable ranges minus entries, which each lie inside one usable range. The
 * iterator keeps the usable range and the cursor, so no array of gaps is needed. */
struct gap { uint64_t base, end; };
struct gap_walk { unsigned range; uint64_t cursor; int started; };

static int gap_next(const struct app_region_ledger *l, struct gap_walk *w, struct gap *out)
{
    for (;;) {
        if (w->range >= l->usable_count) return 0;
        uint64_t end = l->usable[w->range].base + l->usable[w->range].bytes;
        if (!w->started) { w->cursor = l->usable[w->range].base; w->started = 1; }
        const struct app_region_entry *next = 0;
        for (unsigned i = 0; i < l->capacity; ++i) {
            const struct app_region_entry *e = &l->entry[i];
            if (e->owner && e->base >= w->cursor && e->base < end && (!next || e->base < next->base)) next = e;
        }
        uint64_t stop = next ? next->base : end;
        struct gap g = {w->cursor, stop};
        if (next) w->cursor = next->base + next->bytes;
        else { ++w->range; w->started = 0; }
        if (g.end > g.base) { *out = g; return 1; }
    }
}

uint64_t app_region_free_bytes(const struct app_region_ledger *l)
{
    struct gap_walk w = {0, 0, 0};
    struct gap g;
    uint64_t sum = 0;
    while (gap_next(l, &w, &g)) sum += g.end - g.base;
    return sum;
}

int app_region_acquire(struct app_region_ledger *l, uint16_t owner, uint64_t bytes, unsigned access,
                       uint64_t *base_out, uint64_t *bytes_out)
{
    if (!l || !owner || !bytes || !base_out || !bytes_out ||
        (access != APP_REGION_READ && access != (APP_REGION_READ | APP_REGION_WRITE)) ||
        bytes > APP_REGION_LIMIT - APP_REGION_BASE) return APP_REGION_INVALID;
    bytes = (bytes + APP_REGION_BLOCK - 1) & ~(APP_REGION_BLOCK - 1);
    unsigned slot = l->capacity;
    for (unsigned i = 0; i < l->capacity; ++i) if (!l->entry[i].owner) { slot = i; break; }
    if (slot == l->capacity) return APP_REGION_QUOTA;
    uint64_t best_base = 0, best_gap = UINT64_MAX;
    int found = 0;
    struct gap_walk w = {0, 0, 0};
    struct gap g;
    if (bytes >= APP_REGION_GIB) {                 /* a 1 GiB boundary keeps whole slots as one block */
        while (!found && gap_next(l, &w, &g)) {
            uint64_t start = (g.base + APP_REGION_GIB - 1) & ~(APP_REGION_GIB - 1);
            if (start < g.end && g.end - start >= bytes) { best_base = start; found = 1; }
        }
    }
    w = (struct gap_walk){0, 0, 0};
    while (!found && gap_next(l, &w, &g)) {        /* smallest gap that holds the size */
        uint64_t size = g.end - g.base;
        if (size >= bytes && size < best_gap) { best_gap = size; best_base = g.base; }
    }
    if (!found && best_gap == UINT64_MAX) return APP_REGION_NO_SPACE;
    l->entry[slot] = (struct app_region_entry){best_base, bytes, owner, (uint8_t)access};
    *base_out = best_base; *bytes_out = bytes;
    return APP_REGION_OK;
}

int app_region_release(struct app_region_ledger *l, uint16_t owner, uint64_t base)
{
    if (!l || !owner) return APP_REGION_INVALID;
    for (unsigned i = 0; i < l->capacity; ++i)
        if (l->entry[i].owner == owner && l->entry[i].base == base) {
            memset(&l->entry[i], 0, sizeof l->entry[i]);
            return APP_REGION_OK;
        }
    return APP_REGION_NOT_OWNED;
}

void app_asid_pool_init(struct app_asid_pool *pool, uint64_t id_aa64mmfr0_el1, uint64_t tcr_el1)
{
    pool->next = 0;
    pool->limit = APP_ID_AA64MMFR0_ASIDBITS(id_aa64mmfr0_el1) == 2 && APP_TCR_AS(tcr_el1) ? 65535u : 255u;
}

uint16_t app_asid_alloc(struct app_asid_pool *pool)
{
    if (!pool || !pool->limit || pool->next >= pool->limit) return 0;
    return (uint16_t)++pool->next;
}

uint16_t app_region_owner_of(const struct app_region_ledger *l, uint64_t pa)
{
    for (unsigned i = 0; i < l->capacity; ++i)
        if (l->entry[i].owner && pa >= l->entry[i].base && pa - l->entry[i].base < l->entry[i].bytes)
            return l->entry[i].owner;
    return 0;
}

/* The EL1-only descriptor an upper-memory block has in the kernel's table. */
#define KERNEL_BLOCK(pa) ((pa) | APP_PTE_VALID | APP_PTE_ATTR_WB | APP_PTE_SH_INNER | APP_PTE_AF | \
                          APP_PTE_NG | APP_PTE_PXN | APP_PTE_UXN)

static uint64_t ap_bits(unsigned access) { return access & APP_REGION_WRITE ? APP_PTE_AP_EL0_RW : APP_PTE_AP_RO; }

/* Entries of the owner's region that overlaps [lo, hi): access, or 0. */
static unsigned covered(const struct app_region_ledger *l, uint16_t owner, uint64_t lo, uint64_t hi,
                        int *whole)
{
    unsigned access = 0;
    *whole = 0;
    for (unsigned i = 0; i < l->capacity; ++i) {
        const struct app_region_entry *e = &l->entry[i];
        if (e->owner != owner || e->base >= hi || e->base + e->bytes <= lo) continue;
        access = e->access;
        if (e->base <= lo && e->base + e->bytes >= hi) *whole = 1;
    }
    return access;
}

int app_space_build(struct app_space *s, struct app_table_arena *a, const uint64_t base_l1[512],
                    const struct app_region_ledger *l, uint16_t asid)
{ return app_space_build_view(s, a, base_l1, l, asid, asid, 0); }

int app_space_build_view(struct app_space *s, struct app_table_arena *a, const uint64_t base_l1[512],
                         const struct app_region_ledger *l, uint16_t owner, uint16_t asid, unsigned flags)
{
    if (!s || !a || !base_l1 || !l || !asid || !owner || !a->page || (flags & ~APP_SPACE_NO_GIB_BLOCKS)) return APP_REGION_INVALID;
    /* Each upper-memory slot is either unmapped (the board maps less than 12 GiB) or the kernel's EL1-only
     * block with nG set; a slot that a region covers must be the block. */
    for (unsigned slot = SLOT_FIRST; slot < SLOT_END; ++slot) {
        uint64_t want = KERNEL_BLOCK(UINT64_C(0x40000000) * slot), lo = APP_REGION_GIB * slot;
        int whole;
        if (base_l1[slot] != want && base_l1[slot] != 0) return APP_REGION_INVALID;
        if (base_l1[slot] == 0 && covered(l, owner, lo, lo + APP_REGION_GIB, &whole)) return APP_REGION_INVALID;
    }
    unsigned need = 1;
    for (unsigned slot = SLOT_FIRST; slot < SLOT_END; ++slot) {
        uint64_t lo = APP_REGION_GIB * slot;
        int whole;
        if (covered(l, owner, lo, lo + APP_REGION_GIB, &whole) && (!whole || (flags & APP_SPACE_NO_GIB_BLOCKS))) ++need;
    }
    if (a->capacity - a->used < need) return APP_REGION_NO_TABLES;

    uint64_t (*root)[512] = &a->page[a->used];
    memcpy(*root, base_l1, 512 * sizeof(uint64_t));
    unsigned used = a->used + 1;
    for (unsigned slot = SLOT_FIRST; slot < SLOT_END; ++slot) {
        uint64_t lo = APP_REGION_GIB * slot;
        int whole;
        unsigned access = covered(l, owner, lo, lo + APP_REGION_GIB, &whole);
        if (!access) continue;
        if (whole && !(flags & APP_SPACE_NO_GIB_BLOCKS)) {
            (*root)[slot] = KERNEL_BLOCK(lo) & ~APP_PTE_AP_RO;
            (*root)[slot] |= ap_bits(access);
            continue;
        }
        uint64_t (*l2)[512] = &a->page[used];
        uint64_t table_pa = a->physical + (uint64_t)used * 4096;
        ++used;
        for (unsigned j = 0; j < BLOCKS_PER_SLOT; ++j) {
            uint64_t pa = lo + (uint64_t)j * APP_REGION_BLOCK, e = KERNEL_BLOCK(pa);
            int unused;
            unsigned block_access = covered(l, owner, pa, pa + APP_REGION_BLOCK, &unused);
            if (block_access) e |= ap_bits(block_access);
            (*l2)[j] = e;
        }
        (*root)[slot] = table_pa | APP_PTE_TABLE;
    }
    s->root = *root;
    s->root_physical = a->physical + (uint64_t)a->used * 4096;
    s->asid = asid;
    s->tables = used - a->used;
    a->used = used;
    return APP_REGION_OK;
}
