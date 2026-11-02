#ifndef SUNSTONE_APP_REGION_H
#define SUNSTONE_APP_REGION_H
/* Large application regions in upper memory (docs/designs/large-app-regions.md).
 *
 * Two parts, both free of hardware access so the host checks run the code
 * the image links:
 *   - the region ledger, which hands out 2 MiB-aligned extents of upper
 *     memory to one owner each, and
 *   - the application space, a copy of the kernel's level-1 table in which
 *     one application's extents are readable (and optionally writable) at EL0.
 * Stage-1 descriptor fields follow Arm ARM (DDI 0487) D8, 4 KiB granule,
 * 39-bit input addresses, level-1 table. */
#include <stddef.h>
#include <stdint.h>

#define APP_REGION_BASE UINT64_C(0x80000000)       /* first byte of upper memory */
#define APP_REGION_LIMIT UINT64_C(0x340000000)     /* end of the 12 GiB map */
#define APP_REGION_BLOCK UINT64_C(0x200000)        /* 2 MiB: smallest extent */
#define APP_REGION_GIB UINT64_C(0x40000000)
/* Counts. Nothing here is a tuning constant. The upper memory is APP_REGION_SLOTS gigabytes of level-1 slots
 * (0x80000000 to 0x340000000), which bounds the usable ranges (a range is a run of slots) and the level-2
 * pages of one space (a slot has one level-2 table however many regions touch it), so a space needs at most
 * APP_SPACE_PAGES pages: the root and one table per slot. The number of regions is bounded by the memory:
 * each takes at least one 2 MiB block. A ledger holds the regions the manifests declare; the caller gives it
 * storage for that many, which the image sizes at build time from the manifests. */
#define APP_REGION_SLOTS 11u
#define APP_REGION_MAX_REGIONS ((APP_REGION_LIMIT - APP_REGION_BASE) / APP_REGION_BLOCK)
#define APP_SPACE_PAGES (1u + APP_REGION_SLOTS)

enum app_region_status {
    APP_REGION_OK = 0,
    APP_REGION_INVALID = -1,     /* malformed argument; nothing changed */
    APP_REGION_NO_SPACE = -2,    /* no free extent of the size; nothing changed */
    APP_REGION_QUOTA = -3,       /* the ledger has no free entry: its storage holds every declared region */
    APP_REGION_NOT_OWNED = -4,   /* release of a region the owner does not hold */
    APP_REGION_NO_TABLES = -5    /* the table arena cannot hold the space */
};

enum { APP_REGION_READ = 1, APP_REGION_WRITE = 2 };   /* EL0 access declared */

struct app_region_range { uint64_t base, bytes; };

struct app_region_entry {
    uint64_t base, bytes;
    uint8_t owner;               /* ASID of the owning application; 0: slot free */
    uint8_t access;              /* APP_REGION_READ | APP_REGION_WRITE */
};

struct app_region_ledger {
    struct app_region_range *usable;        /* caller storage for usable_capacity ranges */
    unsigned usable_count, usable_capacity;
    struct app_region_entry *entry;         /* caller storage for capacity entries */
    unsigned capacity;
};

/* Admit the upper-memory ranges that the DRAM decode check passed. Every range
 * is 2 MiB-aligned and inside APP_REGION_BASE..APP_REGION_LIMIT; the list is
 * sorted and disjoint. No ranges (the check failed) leaves every later
 * acquire refused with APP_REGION_NO_SPACE. Returns 0 or APP_REGION_INVALID. */
int app_region_ledger_init(struct app_region_ledger *, struct app_region_range *usable_storage, unsigned usable_capacity,
                           struct app_region_entry *entry_storage, unsigned entry_capacity,
                           const struct app_region_range *ranges, unsigned count);

/* Reserve bytes (rounded up to 2 MiB) for owner (ASID 1..255). Regions of
 * 1 GiB or more are placed on a 1 GiB boundary when a free one exists;
 * smaller regions take the smallest free gap. */
int app_region_acquire(struct app_region_ledger *, uint8_t owner, uint64_t bytes, unsigned access,
                       uint64_t *base_out, uint64_t *bytes_out);
int app_region_release(struct app_region_ledger *, uint8_t owner, uint64_t base);
/* Owner (ASID) of the byte at pa, or 0 when no application holds it. A DMA
 * master's grant check asks this before it maps the extent. */
uint8_t app_region_owner_of(const struct app_region_ledger *, uint64_t pa);
uint64_t app_region_free_bytes(const struct app_region_ledger *);

/* ASIDs. TCR_EL1.AS is 0, so an ASID is 8 bits and 0 is the kernel's. The pool hands each of 1..255 out once and
 * never takes one back: nothing tears a space down today (an application runs until the session ends and the board
 * resets), so no ASID is ever reused. A teardown that returned an ASID to the pool would have to clear the space's
 * tables and execute `DSB ISH; TLBI ASIDE1IS, <asid << 48>; DSB ISH` on every core's behalf before the number
 * could name another space; until that exists, exhaustion is the refusal (app_asid_alloc returns 0). */
struct app_asid_pool { unsigned next; };
uint8_t app_asid_alloc(struct app_asid_pool *);

/* Stage-1 descriptor words. */
#define APP_PTE_VALID UINT64_C(1)
#define APP_PTE_TABLE UINT64_C(3)
#define APP_PTE_ATTR_WB (UINT64_C(1) << 2)          /* AttrIndx 1: Normal write-back */
#define APP_PTE_AP_EL0_RW (UINT64_C(1) << 6)        /* AP[2:1] = 0b01 */
#define APP_PTE_AP_RO (UINT64_C(3) << 6)            /* AP[2:1] = 0b11: read-only at both levels */
#define APP_PTE_SH_INNER (UINT64_C(3) << 8)
#define APP_PTE_AF (UINT64_C(1) << 10)
#define APP_PTE_NG (UINT64_C(1) << 11)
#define APP_PTE_PXN (UINT64_C(1) << 53)
#define APP_PTE_UXN (UINT64_C(1) << 54)
#define APP_PTE_ADDR_MASK UINT64_C(0x0000fffffffff000)

/* Tables for application spaces come from one page arena. */
struct app_table_arena {
    uint64_t (*page)[512];       /* 4 KiB-aligned pages */
    uint64_t physical;           /* physical address of page[0] */
    unsigned capacity, used;
};

struct app_space {
    uint64_t *root;              /* a page of the arena */
    uint64_t root_physical;
    uint8_t asid;
    unsigned tables;             /* pages used, root included */
};

/* Build the space of one application: copy base_l1 (the kernel's level-1
 * table, 512 words) and expose the owner's regions. Every upper-memory entry
 * of base_l1 (slots 2..12) must be a block descriptor with nG set, so a TLB
 * entry for upper memory always carries an ASID; the function refuses
 * otherwise. Slots a region covers entirely become 1 GiB blocks; other
 * slots it touches point to a level-2 table of 2 MiB blocks. On any refusal
 * the arena and the space are as before. */
int app_space_build(struct app_space *, struct app_table_arena *, const uint64_t base_l1[512],
                    const struct app_region_ledger *, uint8_t asid);
/* The same with a separate view: the space carries the ASID asid and exposes the regions that
 * owner holds. APP_SPACE_NO_GIB_BLOCKS maps every slot with 2 MiB blocks even when a region covers it
 * whole, to measure what the 1 GiB blocks save in TLB entries. */
#define APP_SPACE_NO_GIB_BLOCKS 1u
int app_space_build_view(struct app_space *, struct app_table_arena *, const uint64_t base_l1[512],
                         const struct app_region_ledger *, uint8_t owner, uint8_t asid, unsigned flags);
/* The TTBR0_EL1 word that installs the space: table base and ASID in bits 63:48. */
static inline uint64_t app_space_ttbr0(const struct app_space *s)
{ return s->root_physical | ((uint64_t)s->asid << 48); }

#endif
