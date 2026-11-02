/* Host check of the large application region ledger and application spaces.
 * It links kernel/appmem/app_region.c and walks the tables it builds with an
 * independent model of AArch64 stage-1 translation (Arm ARM DDI 0487 D8: 4 KiB
 * granule, 39-bit input address, translation starts at level 1, PSTATE.PAN
 * clear, no hierarchical table attributes). The model refuses any descriptor
 * bit it does not model. */
#include "app_region.h"
#include "app_large.h"
#include "a733_device_memory_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIB UINT64_C(0x40000000)
#define MIB UINT64_C(0x100000)
#define CAP 64
static struct app_region_range storage_u[4][APP_REGION_SLOTS];
static struct app_region_entry storage_e[4][CAP];
/* Ledger number n with the given entry capacity. */
static int linit(struct app_region_ledger *l, unsigned n, unsigned capacity, const struct app_region_range *r, unsigned count)
{ return app_region_ledger_init(l, storage_u[n], APP_REGION_SLOTS, storage_e[n], capacity, r, count); }
/* The entries of ledger n, saved and compared, because the ledger struct holds only pointers. */
static struct app_region_entry saved_e[CAP];
static void save(unsigned n) { memcpy(saved_e, storage_e[n], sizeof saved_e); }
static int unchanged(unsigned n) { return !memcmp(saved_e, storage_e[n], sizeof saved_e); }

enum { FAULT_NONE, FAULT_TRANSLATION, FAULT_PERMISSION, FAULT_ACCESS_FLAG };
struct result { int fault; uint64_t pa, leaf; unsigned level; uint64_t desc; };

static const uint64_t root_pa = UINT64_C(0x41100000), kernel_l2_pa = UINT64_C(0x41101000);
static const uint64_t arena_pa = UINT64_C(0x41200000);
static uint64_t base_l1[512] __attribute__((aligned(4096)));
static uint64_t kernel_l2[512] __attribute__((aligned(4096)));
static uint64_t arena_pages[16][512] __attribute__((aligned(4096)));
static struct app_table_arena arena = {arena_pages, UINT64_C(0x41200000), 16, 0};

/* The model's physical memory: only the kernel table pages and the arena exist. */
static uint64_t read_word(uint64_t pa)
{
    if (pa >= root_pa && pa < root_pa + 4096) return base_l1[(pa - root_pa) / 8];
    if (pa >= kernel_l2_pa && pa < kernel_l2_pa + 4096) return kernel_l2[(pa - kernel_l2_pa) / 8];
    if (pa >= arena_pa && pa < arena_pa + sizeof arena_pages) {
        uint64_t offset = pa - arena_pa;
        return arena_pages[offset / 4096][(offset % 4096) / 8];
    }
    fprintf(stderr, "walk read outside modeled tables: %#llx\n", (unsigned long long)pa);
    abort();
}

/* ttbr0: table base | ASID << 48. kind: 0 EL0 read, 1 EL0 write, 2 EL1 read, 3 EL1 write. */
static struct result walk(uint64_t ttbr0, uint64_t va, int kind)
{
    struct result r = {0};
    assert(va < (UINT64_C(1) << 39));
    uint64_t table = ttbr0 & APP_PTE_ADDR_MASK;
    for (unsigned level = 1; level <= 2; ++level) {
        unsigned shift = level == 1 ? 30 : 21;
        uint64_t d = read_word(table + ((va >> shift) & 511) * 8);
        r.level = level; r.desc = d;
        if (!(d & 1)) { r.fault = FAULT_TRANSLATION; return r; }
        if ((d & 3) == 3) {                       /* table */
            assert(!(d >> 59));                   /* hierarchical attributes are not modeled */
            table = d & APP_PTE_ADDR_MASK;
            continue;
        }
        assert(!(d & (UINT64_C(1) << 52)));       /* contiguous hint is not modeled */
        assert(!(d & (UINT64_C(1) << 5)));        /* NS bit: single security state */
        assert(!(d & UINT64_C(0x7ff0000000000)) || 1);
        r.leaf = UINT64_C(1) << shift;
        r.pa = (d & APP_PTE_ADDR_MASK & ~(r.leaf - 1)) | (va & (r.leaf - 1));
        if (!(d & APP_PTE_AF)) { r.fault = FAULT_ACCESS_FLAG; return r; }
        unsigned ap = (d >> 6) & 3;               /* AP[2:1] */
        int el0 = kind < 2, write = kind & 1;
        if (el0 && !(ap & 1)) r.fault = FAULT_PERMISSION;
        else if (write && (ap & 2)) r.fault = FAULT_PERMISSION;
        return r;
    }
    r.fault = FAULT_TRANSLATION;                  /* the kernel L2 has only blocks here */
    return r;
}

/* A TLB with ASID-tagged and global entries; a lookup that matches two
 * entries is a TLB conflict (Arm ARM D8.17 "TLB conflict abort"). */
struct tlb_entry { int valid; uint64_t base, size; unsigned asid; int global; uint64_t desc; };
static struct tlb_entry tlb[32];
static int tlb_conflict;
static struct result tlb_access(uint64_t ttbr0, uint64_t va, int kind)
{
    unsigned asid = (unsigned)(ttbr0 >> 48);
    struct tlb_entry *hit = 0;
    unsigned matches = 0;
    for (unsigned i = 0; i < 32; ++i) {
        struct tlb_entry *e = &tlb[i];
        if (e->valid && va >= e->base && va - e->base < e->size && (e->global || e->asid == asid)) {
            ++matches; hit = e;
        }
    }
    if (matches > 1) tlb_conflict = 1;
    struct result w = walk(ttbr0, va, kind);
    if (!hit && w.fault != FAULT_TRANSLATION) {
        for (unsigned i = 0; i < 32; ++i)
            if (!tlb[i].valid) {
                tlb[i] = (struct tlb_entry){1, va & ~(w.leaf - 1), w.leaf, asid, !(w.desc & APP_PTE_NG), w.desc};
                break;
            }
        return w;
    }
    if (hit) {                                    /* the cached descriptor decides */
        unsigned ap = (hit->desc >> 6) & 3;
        struct result r = {0};
        r.leaf = hit->size;
        if (kind < 2 && !(ap & 1)) r.fault = FAULT_PERMISSION;
        else if ((kind & 1) && (ap & 2)) r.fault = FAULT_PERMISSION;
        r.pa = (hit->desc & APP_PTE_ADDR_MASK & ~(hit->size - 1)) | (va & (hit->size - 1));
        return r;
    }
    return w;
}

static void build_kernel(int upper_global)
{
    memset(base_l1, 0, sizeof base_l1); memset(kernel_l2, 0, sizeof kernel_l2);
    base_l1[0] = 0 | 1 | APP_PTE_AF | APP_PTE_PXN | APP_PTE_UXN;            /* MMIO, Device (attr 0), no EL0 */
    base_l1[1] = kernel_l2_pa | APP_PTE_TABLE;
    for (unsigned i = 0; i < 512; ++i) {                                      /* DRAM below 0x8000_0000 */
        uint64_t pa = GIB + (uint64_t)i * 0x200000;
        uint64_t d = pa | 1 | APP_PTE_ATTR_WB | APP_PTE_SH_INNER | APP_PTE_AF | APP_PTE_PXN | APP_PTE_UXN;
        if (pa < 0x41000000 || (pa >= 0x41200000 && pa < 0x42000000) || (pa >= 0x42200000 && pa < 0x48000000))
            d |= APP_PTE_AP_EL0_RW;                                           /* EL0 read-write data */
        else if (pa == 0x41000000) d = (d & ~(APP_PTE_PXN | APP_PTE_UXN)) | APP_PTE_AP_RO;  /* text */
        else d = 0;                                                           /* monitor, tables: invalid */
        kernel_l2[i] = d;
    }
    for (unsigned slot = 2; slot < 13; ++slot) {
        base_l1[slot] = (GIB * slot) | 1 | APP_PTE_ATTR_WB | APP_PTE_SH_INNER | APP_PTE_AF |
                        APP_PTE_PXN | APP_PTE_UXN | (upper_global ? 0 : APP_PTE_NG);
        /* The kernel builder's descriptor is the one the space builder expects. */
        if (!upper_global) assert(base_l1[slot] == A733_EL1_DRAM_BLOCK(GIB * slot));
    }
}

static void expect(struct result r, int fault, const char *what)
{
    if (r.fault != fault) { fprintf(stderr, "FAIL %s: fault %d, expected %d\n", what, r.fault, fault); exit(1); }
}
#define EXPECT(r, f) expect((r), (f), #r)

static unsigned count_leaves(uint64_t ttbr0, uint64_t base, uint64_t bytes, uint64_t size)
{
    unsigned n = 0;
    for (uint64_t va = base; va < base + bytes;) {
        struct result r = walk(ttbr0, va, 2);
        assert(r.fault == FAULT_NONE);
        if (r.leaf == size) ++n;
        va = (va & ~(r.leaf - 1)) + r.leaf;
    }
    return n;
}

static struct ss_large_region record(unsigned index, uint64_t bytes, const char *path)
{
    struct ss_large_region r = {.index = index, .bytes = bytes, .source_bytes = bytes};
    memset(r.sha256, 0x5a, 32);
    strncpy(r.path, path, sizeof r.path);
    return r;
}

/* Large-region records: validation and all-or-nothing admission through the ledger. */
static void large_cases(void)
{
    struct app_region_range all[] = {{APP_REGION_BASE, APP_REGION_LIMIT - APP_REGION_BASE}};
    struct app_region_ledger l;
    struct large_slot slots[8];
    struct ss_large_region ok[2] = {record(0, GIB, "/sunstone-test/sunstone-region-test.bin"), record(1, 4 * MIB, "")};
    ok[0].source_bytes = 512 * MIB;
    ok[1].flags = SS_LARGE_WRITE | SS_LARGE_EXTENT; ok[1].first_sector = 40960;
    assert(ss_large_records_valid(ok, 2) && ss_large_records_valid(0, 0));
    assert(linit(&l, 0, 8, all, 1) == 0);
    assert(large_admit(&l, 3, ok, 2, slots, 8) == APP_REGION_OK);
    assert(slots[0].base == 0x80000000 && slots[0].bytes == GIB && slots[0].state == SS_LARGE_PENDING && slots[0].index == 0);
    assert(slots[1].bytes == 4 * MIB && slots[1].flags == (SS_LARGE_WRITE | SS_LARGE_EXTENT) && slots[1].base == 0xc0000000);
    assert(app_region_owner_of(&l, slots[0].base) == 3 && app_region_owner_of(&l, slots[1].base + 4 * MIB - 1) == 3);
    assert(app_region_free_bytes(&l) == 11 * GIB - GIB - 4 * MIB);
    /* A region with no source is valid with every source field zero, and refused with any of them set. */
    struct ss_large_region unfilled[1] = {{.index = 0, .flags = SS_LARGE_WRITE | SS_LARGE_UNFILLED, .bytes = 6 * GIB}};
    assert(ss_large_records_valid(unfilled, 1));
    assert(linit(&l, 0, 8, all, 1) == 0 && large_admit(&l, 6, unfilled, 1, slots, 8) == APP_REGION_OK);
    assert(slots[0].base == 0x80000000 && slots[0].bytes == 6 * GIB && slots[0].base + slots[0].bytes > UINT64_C(0x100000000));
    struct ss_large_region bad = unfilled[0];
    bad.source_bytes = MIB; assert(!ss_large_records_valid(&bad, 1));
    bad = unfilled[0]; bad.sha256[3] = 1; assert(!ss_large_records_valid(&bad, 1));
    bad = unfilled[0]; strcpy(bad.path, "/a"); assert(!ss_large_records_valid(&bad, 1));
    bad = unfilled[0]; bad.flags |= SS_LARGE_EXTENT; assert(!ss_large_records_valid(&bad, 1));
    bad = unfilled[0]; bad.first_sector = 5; assert(!ss_large_records_valid(&bad, 1));
    /* Records out of index order are refused: the section is in index order by construction. */
    struct ss_large_region swapped[2] = {ok[1], ok[0]};
    assert(linit(&l, 0, 8, all, 1) == 0 && large_admit(&l, 4, swapped, 2, slots, 8) == APP_REGION_INVALID);

    /* All or nothing: a second region that does not fit releases the first and changes nothing. */
    struct ss_large_region big[2] = {record(0, 6 * GIB, "/a.bin"), record(1, 5 * GIB, "/b.bin")};
    assert(linit(&l, 0, 8, all, 1) == 0);
    uint64_t taken_base, taken_bytes;
    assert(app_region_acquire(&l, 9, 2 * GIB, APP_REGION_READ, &taken_base, &taken_bytes) == 0);   /* another owner holds 2 GiB */
    save(0);
    assert(large_admit(&l, 5, big, 2, slots, 8) == APP_REGION_NO_SPACE);
    assert(unchanged(0) && !slots[0].bytes && !slots[1].bytes && app_region_free_bytes(&l) == 9 * GIB);
    struct app_region_range none[1];
    struct app_region_ledger empty;
    assert(linit(&empty, 1, 8, none, 0) == 0 && large_admit(&empty, 5, ok, 2, slots, 8) == APP_REGION_NO_SPACE);
    /* The number of records is the manifest's: five records are valid, and a ledger sized for four refuses the
     * fifth entry and holds nothing. */
    struct ss_large_region five[5] = {record(0, 2 * MIB, "/a"), record(1, 2 * MIB, "/b"), record(2, 2 * MIB, "/c"),
                                      record(3, 2 * MIB, "/d"), record(4, 2 * MIB, "/e")};
    assert(ss_large_records_valid(five, 5));
    assert(linit(&l, 0, 5, all, 1) == 0 && large_admit(&l, 6, five, 5, slots, 8) == APP_REGION_OK && slots[4].bytes == 2 * MIB);
    assert(linit(&l, 0, 4, all, 1) == 0 && large_admit(&l, 6, five, 5, slots, 8) == APP_REGION_QUOTA);
    assert(app_region_free_bytes(&l) == 11 * GIB && !slots[0].bytes);
    /* The total must fit the upper memory, not a constant. */
    struct ss_large_region many[2] = {record(0, 6 * GIB, "/a"), record(1, 6 * GIB, "/b")};
    assert(!ss_large_records_valid(many, 2));

    /* Malformed records are refused whatever the ledger holds. */
    struct ss_large_region r[2];
    int refusals = 0;
#define REFUSE(edit) do { memcpy(r, ok, sizeof r); edit; assert(!ss_large_records_valid(r, 2)); \
        assert(large_admit(&l, 6, r, 2, slots, 8) == APP_REGION_INVALID); ++refusals; } while (0)
    assert(linit(&l, 0, 8, all, 1) == 0);
    REFUSE(r[1].index = 0);                                  /* duplicate index */
    REFUSE(r[1].index = 2);                                  /* index beyond the count */
    REFUSE(r[0].flags = 4);                                  /* unknown flag */
    REFUSE(r[0].bytes = GIB + 4096);                         /* not a multiple of 2 MiB */
    REFUSE(r[0].bytes = 0);
    REFUSE(r[0].bytes = 12 * GIB);                           /* beyond upper memory */
    REFUSE(r[0].source_bytes = GIB + 1);
    REFUSE(r[0].source_bytes = 0);
    /* A zero digest is valid: the region loads without a check. */
    { struct ss_large_region nodigest = ok[0]; memset(nodigest.sha256, 0, 32); assert(ss_large_records_valid(&nodigest, 1)); }
    REFUSE(strcpy(r[0].path, "relative.bin"));
    REFUSE(strcpy(r[0].path, "/bad name.bin"));
    REFUSE(strcpy(r[0].path, ""));
    REFUSE(memset(r[0].path, 'a', 48); r[0].path[0] = '/');   /* no terminator */
    REFUSE(r[0].path[45] = 'x');                              /* data after the terminator */
    REFUSE(r[0].first_sector = 7);                            /* a file with a sector */
    REFUSE(strcpy(r[1].path, "/x"));                          /* an extent with a path */
    REFUSE(r[1].first_sector = 0);                            /* an extent at the partition table */
    assert(refusals == 16 && app_region_free_bytes(&l) == 11 * GIB);
}

int main(void)
{
    build_kernel(0);
    const uint64_t base_ttbr = root_pa;                       /* ASID 0 */
    struct app_region_range all[] = {{APP_REGION_BASE, APP_REGION_LIMIT - APP_REGION_BASE}};

    /* Ledger initialization refuses malformed ranges and leaves an empty ledger refusing requests. */
    struct app_region_ledger l;
    struct app_region_range bad1[] = {{APP_REGION_BASE + 4096, 2 * MIB}};
    struct app_region_range bad2[] = {{0x40000000, 2 * MIB}};
    struct app_region_range bad3[] = {{APP_REGION_LIMIT - 2 * MIB, 4 * MIB}};
    struct app_region_range bad4[] = {{APP_REGION_BASE + 8 * MIB, 4 * MIB}, {APP_REGION_BASE, 2 * MIB}};
    struct app_region_range bad5[] = {{APP_REGION_BASE, 4 * MIB}, {APP_REGION_BASE + 2 * MIB, 4 * MIB}};
    assert(linit(&l, 2, CAP, bad1, 1) == APP_REGION_INVALID);
    assert(linit(&l, 2, CAP, bad2, 1) == APP_REGION_INVALID);
    assert(linit(&l, 2, CAP, bad3, 1) == APP_REGION_INVALID);
    assert(linit(&l, 2, CAP, bad4, 2) == APP_REGION_INVALID);
    assert(linit(&l, 2, CAP, bad5, 2) == APP_REGION_INVALID);
    uint64_t b, n;
    assert(linit(&l, 2, CAP, 0, 0) == 0);            /* decode check failed: nothing usable */
    assert(app_region_acquire(&l, 1, 2 * MIB, APP_REGION_READ, &b, &n) == APP_REGION_NO_SPACE);
    assert(app_region_free_bytes(&l) == 0);

    assert(linit(&l, 2, CAP, all, 1) == 0);
    assert(app_region_free_bytes(&l) == 11 * GIB);

    /* Malformed requests change nothing. */
    save(2);
    assert(app_region_acquire(&l, 0, 2 * MIB, APP_REGION_READ, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, 0, APP_REGION_READ, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, 2 * MIB, APP_REGION_WRITE, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, 2 * MIB, 0, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, UINT64_MAX, APP_REGION_READ, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, UINT64_MAX - 100, APP_REGION_READ, &b, &n) == APP_REGION_INVALID);
    assert(app_region_acquire(&l, 1, 2 * MIB, APP_REGION_READ, 0, &n) == APP_REGION_INVALID);
    assert(unchanged(2));

    /* A: 3 GiB read-write; B: 1.5 GiB read-only; C: 100 MiB read-write. */
    uint64_t abase, abytes, bbase, bbytes, cbase, cbytes;
    assert(app_region_acquire(&l, 1, 3 * GIB, APP_REGION_READ | APP_REGION_WRITE, &abase, &abytes) == 0);
    assert(abase == 0x80000000 && abytes == 3 * GIB);
    assert(app_region_acquire(&l, 2, 1536 * MIB, APP_REGION_READ, &bbase, &bbytes) == 0);
    assert(bbase == 0x140000000 && bbytes == 1536 * MIB);
    assert(app_region_acquire(&l, 3, 100 * MIB + 1, APP_REGION_READ | APP_REGION_WRITE, &cbase, &cbytes) == 0);
    assert(cbase == 0x1a0000000 && cbytes == 102 * MIB);      /* rounded up to 2 MiB */
    assert(app_region_free_bytes(&l) == 11 * GIB - 3 * GIB - 1536 * MIB - 102 * MIB);

    struct app_space sa, sb, sc;
    assert(app_space_build(&sa, &arena, base_l1, &l, 1) == 0);
    assert(app_space_build(&sb, &arena, base_l1, &l, 2) == 0);
    assert(app_space_build(&sc, &arena, base_l1, &l, 3) == 0);
    uint64_t ta = app_space_ttbr0(&sa), tb = app_space_ttbr0(&sb), tc = app_space_ttbr0(&sc);
    assert((ta >> 48) == 1 && (tb >> 48) == 2 && (tc >> 48) == 3);

    /* Table cost: A's region is whole slots, B and C each need one level-2 table. */
    assert(sa.tables == 1 && sb.tables == 2 && sc.tables == 2 && arena.used == 5);
    /* Block sizes as designed: A is three 1 GiB blocks; B one 1 GiB block and 256 2 MiB blocks;
     * C fifty-one 2 MiB blocks. */
    assert(count_leaves(ta, abase, abytes, GIB) == 3 && count_leaves(ta, abase, abytes, 2 * MIB) == 0);
    assert(count_leaves(tb, bbase, bbytes, GIB) == 1 && count_leaves(tb, bbase, bbytes, 2 * MIB) == 256);
    assert(count_leaves(tc, cbase, cbytes, 2 * MIB) == 51 && count_leaves(tc, cbase, cbytes, GIB) == 0);

    /* Isolation. The owner reaches every byte of its region at EL0. */
    uint64_t probes[] = {abase, abase + 8, abase + GIB, abase + abytes / 2, abase + abytes - 8};
    for (unsigned i = 0; i < 5; ++i) {
        struct result r = walk(ta, probes[i], 0); EXPECT(r, FAULT_NONE); assert(r.pa == probes[i]);
        r = walk(ta, probes[i], 1); EXPECT(r, FAULT_NONE);
    }
    EXPECT(walk(ta, abase + abytes, 0), FAULT_PERMISSION);      /* first byte after A's region */
    EXPECT(walk(ta, abase - 8, 0), FAULT_TRANSLATION);          /* 0x7fff_fff8: the invalid hole */
    EXPECT(walk(ta, bbase, 0), FAULT_PERMISSION);               /* another application's region */
    EXPECT(walk(ta, cbase, 1), FAULT_PERMISSION);
    EXPECT(walk(tb, abase, 0), FAULT_PERMISSION);               /* B reads A's region: faults */
    EXPECT(walk(tb, abase + abytes - 8, 1), FAULT_PERMISSION);
    EXPECT(walk(tc, abase + GIB, 0), FAULT_PERMISSION);
    EXPECT(walk(tc, bbase + bbytes - 8, 0), FAULT_PERMISSION);  /* B's last byte, in a slot C also maps */
    EXPECT(walk(tb, cbase, 0), FAULT_PERMISSION);               /* C's first byte, in a slot B also maps */
    EXPECT(walk(tc, cbase - 8, 0), FAULT_PERMISSION);           /* below C's region */
    EXPECT(walk(tc, cbase + cbytes, 0), FAULT_PERMISSION);      /* above it */
    EXPECT(walk(tc, cbase, 0), FAULT_NONE);
    EXPECT(walk(tc, cbase + cbytes - 8, 1), FAULT_NONE);
    /* Read-only declaration: B reads, never writes at EL0. */
    EXPECT(walk(tb, bbase, 0), FAULT_NONE);
    EXPECT(walk(tb, bbase + bbytes - 8, 0), FAULT_NONE);
    EXPECT(walk(tb, bbase + GIB + 8, 1), FAULT_PERMISSION);
    EXPECT(walk(tb, bbase, 1), FAULT_PERMISSION);
    /* The kernel table gives EL0 none of upper memory, and EL1 all of it. */
    EXPECT(walk(base_ttbr, abase, 0), FAULT_PERMISSION);
    EXPECT(walk(base_ttbr, bbase, 1), FAULT_PERMISSION);
    EXPECT(walk(base_ttbr, abase, 2), FAULT_NONE);
    EXPECT(walk(base_ttbr, bbase, 3), FAULT_NONE);              /* the loader writes B's region here */
    EXPECT(walk(ta, bbase, 2), FAULT_NONE);                     /* EL1 drivers keep reading it */
    EXPECT(walk(ta, bbase, 3), FAULT_NONE);
    /* Under a read-only space EL1 cannot write the region either (stage 1 has no EL1-write, EL0-read-only
     * encoding); the loader therefore runs under the kernel table. */
    EXPECT(walk(tb, bbase, 3), FAULT_PERMISSION);
    EXPECT(walk(tb, bbase, 2), FAULT_NONE);

    /* Everything below upper memory is the kernel's, byte for byte, in every space. */
    uint64_t kernel_va[] = {0x02000000, 0x40000000, 0x41000000, 0x41200000, 0x42100000, 0x42200000, 0x47ff0000};
    for (unsigned i = 0; i < 7; ++i)
        for (int kind = 0; kind < 4; ++kind) {
            struct result k = walk(base_ttbr, kernel_va[i], kind);
            struct result x = walk(ta, kernel_va[i], kind), y = walk(tb, kernel_va[i], kind),
                          z = walk(tc, kernel_va[i], kind);
            assert(k.fault == x.fault && k.fault == y.fault && k.fault == z.fault);
            assert(k.pa == x.pa && k.pa == y.pa && k.pa == z.pa);
        }
    assert(!memcmp(&sa.root[0], &base_l1[0], 16) && !memcmp(&sb.root[0], &base_l1[0], 16));

    /* A view of A's region with 2 MiB blocks only: the same bytes, 1536 leaves for 3 GiB, one level-2
     * page per slot. Another ASID, so its TLB entries never meet A's. */
    struct app_space sa2;
    assert(app_space_build_view(&sa2, &arena, base_l1, &l, 1, 4, APP_SPACE_NO_GIB_BLOCKS) == 0 && sa2.tables == 4);
    uint64_t ta2 = app_space_ttbr0(&sa2);
    assert((ta2 >> 48) == 4 && count_leaves(ta2, abase, abytes, 2 * MIB) == 1536 && count_leaves(ta2, abase, abytes, GIB) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        struct result r = walk(ta2, probes[i], 1); EXPECT(r, FAULT_NONE); assert(r.pa == probes[i] && r.leaf == 2 * MIB);
    }
    EXPECT(walk(ta2, abase + abytes, 0), FAULT_PERMISSION);
    EXPECT(walk(ta2, bbase, 0), FAULT_PERMISSION);
    assert(app_space_build_view(&sa2, &arena, base_l1, &l, 1, 4, 2) == APP_REGION_INVALID);
    assert(app_space_build_view(&sa2, &arena, base_l1, &l, 0, 4, 0) == APP_REGION_INVALID);
    arena.used -= 4;   /* the view pages are scratch; later cases count arena pages from the five spaces */

    /* Ownership queries for DMA grants. */
    assert(app_region_owner_of(&l, abase) == 1 && app_region_owner_of(&l, abase + abytes - 1) == 1);
    assert(app_region_owner_of(&l, abase + abytes) == 2 && app_region_owner_of(&l, bbase) == 2);
    assert(app_region_owner_of(&l, cbase + cbytes - 1) == 3 && app_region_owner_of(&l, cbase + cbytes) == 0);
    assert(app_region_owner_of(&l, 0x70000000) == 0);

    /* A TLB keeps no stale or conflicting entry across switches because every upper-memory entry carries
     * an ASID. The kernel (ASID 0) reads the region, then A, then B, with no invalidation. */
    memset(tlb, 0, sizeof tlb); tlb_conflict = 0;
    EXPECT(tlb_access(base_ttbr, abase, 2), FAULT_NONE);
    EXPECT(tlb_access(ta, abase, 0), FAULT_NONE);
    EXPECT(tlb_access(ta, abase, 1), FAULT_NONE);
    EXPECT(tlb_access(tb, abase, 0), FAULT_PERMISSION);
    EXPECT(tlb_access(base_ttbr, abase, 0), FAULT_PERMISSION);
    EXPECT(tlb_access(ta, abase, 0), FAULT_NONE);
    EXPECT(tlb_access(tb, bbase, 0), FAULT_NONE);
    EXPECT(tlb_access(tc, bbase, 0), FAULT_PERMISSION);
    assert(!tlb_conflict);
    /* The same sequence with a kernel table whose upper entries are global (the table the builder
     * emits today) conflicts, which is why app_space_build refuses it. */
    uint64_t saved[512]; memcpy(saved, base_l1, sizeof saved);
    build_kernel(1);
    struct app_space sbad;
    uint64_t arena_used = arena.used;
    assert(app_space_build(&sbad, &arena, base_l1, &l, 1) == APP_REGION_INVALID && arena.used == arena_used);
    uint64_t bad_ttbr = (UINT64_C(1) << 48) | sa.root_physical;       /* A's good root, global kernel entry */
    memset(tlb, 0, sizeof tlb); tlb_conflict = 0;
    EXPECT(tlb_access(base_ttbr, abase, 2), FAULT_NONE);              /* kernel fill: a global entry */
    EXPECT(walk(bad_ttbr, abase, 0), FAULT_NONE);                     /* what A's table says */
    EXPECT(tlb_access(bad_ttbr, abase, 0), FAULT_PERMISSION);         /* but the stale global entry answers */
    memset(tlb, 0, sizeof tlb); tlb_conflict = 0;
    EXPECT(tlb_access(bad_ttbr, abase, 0), FAULT_NONE);               /* A fills an ASID-tagged entry */
    EXPECT(tlb_access(base_ttbr, abase, 2), FAULT_NONE);              /* kernel fill: global entry as well */
    (void)tlb_access(bad_ttbr, abase, 0);                             /* both entries now match A */
    assert(tlb_conflict);
    memcpy(base_l1, saved, sizeof saved);

    /* A board that maps less upper memory leaves later slots invalid; a region cannot cover an invalid slot. */
    {
        uint64_t saved_slot[13];
        memcpy(saved_slot, base_l1, sizeof saved_slot);
        for (unsigned slot = 6; slot < 13; ++slot) base_l1[slot] = 0;
        struct app_space partial;
        struct app_table_arena scratch_arena = {arena_pages, arena_pa, 16, 0};
        assert(app_space_build(&partial, &scratch_arena, base_l1, &l, 1) == 0);          /* A's 3 GiB lies in slots 2-4 */
        scratch_arena.used = 0;
        assert(app_space_build(&partial, &scratch_arena, base_l1, &l, 3) == APP_REGION_INVALID && scratch_arena.used == 0);   /* C reaches slot 6 */
        base_l1[3] = 0;
        scratch_arena.used = 0;
        assert(app_space_build(&partial, &scratch_arena, base_l1, &l, 1) == APP_REGION_INVALID);   /* a covered slot unmapped */
        base_l1[3] = base_l1[3] | 0x4000000000000000ull;                                          /* neither zero nor the block */
        assert(app_space_build(&partial, &scratch_arena, base_l1, &l, 9) == APP_REGION_INVALID);
        memcpy(base_l1, saved_slot, sizeof saved_slot);
    }

    /* Exhaustion: the largest gap is granted whole, the next request is refused, release restores it. */
    uint64_t free_before = app_region_free_bytes(&l), dbase, dbytes;
    save(2);
    assert(app_region_acquire(&l, 4, free_before + 2 * MIB, APP_REGION_READ, &dbase, &dbytes) == APP_REGION_NO_SPACE);
    assert(app_region_acquire(&l, 4, 7 * GIB, APP_REGION_READ, &dbase, &dbytes) == APP_REGION_NO_SPACE);
    assert(unchanged(2));
    assert(app_region_acquire(&l, 4, free_before, APP_REGION_READ | APP_REGION_WRITE, &dbase, &dbytes) == 0);
    assert(dbase == cbase + cbytes && dbytes == free_before && app_region_free_bytes(&l) == 0);
    assert(app_region_acquire(&l, 5, 2 * MIB, APP_REGION_READ, &b, &n) == APP_REGION_NO_SPACE);
    assert(app_region_release(&l, 5, dbase) == APP_REGION_NOT_OWNED);     /* not the owner */
    assert(app_region_release(&l, 4, dbase + 2 * MIB) == APP_REGION_NOT_OWNED);
    assert(app_region_release(&l, 4, dbase) == 0 && app_region_free_bytes(&l) == free_before);
    assert(app_region_release(&l, 3, cbase) == 0);                         /* a hole before the free tail */
    assert(app_region_acquire(&l, 6, 4 * MIB, APP_REGION_READ, &b, &n) == 0 && b == cbase);
    assert(app_region_owner_of(&l, cbase) == 6 && app_region_owner_of(&l, cbase + 4 * MIB) == 0);

    /* The ledger holds as many regions as its storage: any owner may take them, the next request is refused. */
    struct app_region_ledger q;
    assert(linit(&q, 3, 9, all, 1) == 0);
    for (unsigned i = 0; i < 9; ++i)
        assert(app_region_acquire(&q, (uint8_t)(7 + i % 2), 2 * MIB, APP_REGION_READ, &b, &n) == 0);
    assert(app_region_acquire(&q, 7, 2 * MIB, APP_REGION_READ, &b, &n) == APP_REGION_QUOTA);
    assert(linit(&q, 3, APP_REGION_MAX_REGIONS + 1, all, 1) == APP_REGION_INVALID);       /* more than the memory can hold */

    /* Fragmentation: two usable ranges; a request larger than either is refused though the sum fits. */
    struct app_region_range split[] = {{APP_REGION_BASE, GIB}, {APP_REGION_BASE + 2 * GIB, GIB}};
    assert(linit(&q, 3, CAP, split, 2) == 0 && app_region_free_bytes(&q) == 2 * GIB);
    assert(app_region_acquire(&q, 1, GIB + 2 * MIB, APP_REGION_READ, &b, &n) == APP_REGION_NO_SPACE);
    assert(app_region_acquire(&q, 1, GIB, APP_REGION_READ, &b, &n) == 0 && b == APP_REGION_BASE);
    assert(app_region_acquire(&q, 2, GIB, APP_REGION_READ, &b, &n) == 0 && b == APP_REGION_BASE + 2 * GIB);

    /* Arena exhaustion leaves the arena and the space untouched. */
    struct app_table_arena small = {arena_pages, arena_pa, 1, 0};
    struct app_space ss = {0};
    assert(app_space_build(&ss, &small, base_l1, &l, 2) == APP_REGION_NO_TABLES && small.used == 0 && !ss.root);
    small.capacity = 2;
    assert(app_space_build(&ss, &small, base_l1, &l, 2) == 0 && small.used == 2);
    /* An application with no region gets the kernel's view with only the root page. */
    struct app_space se;
    assert(app_space_build(&se, &arena, base_l1, &l, 9) == 0 && se.tables == 1);
    EXPECT(walk(app_space_ttbr0(&se), abase, 0), FAULT_PERMISSION);
    EXPECT(walk(app_space_ttbr0(&se), abase, 2), FAULT_NONE);
    assert(app_space_build(&se, &arena, base_l1, &l, 0) == APP_REGION_INVALID);
    /* ASIDs: the width is the hardware's. 8 bits unless ASIDBits reads 16 and TCR_EL1.AS is set; each number once. */
    {
        const uint64_t mmfr_8 = 0x0000000000000000ull, mmfr_16 = 0x0000000000000020ull, as_set = 1ull << 36;
        struct { uint64_t mmfr, tcr; unsigned limit; } widths[] = {
            {mmfr_8, 0, 255}, {mmfr_8, as_set, 255},       /* an 8-bit ASID never uses the upper byte */
            {mmfr_16, 0, 255},                             /* 16 bits in the hardware, AS clear: the upper byte is ignored */
            {mmfr_16, as_set, 65535},
            {mmfr_16 | 0x10, as_set, 255},                 /* an ASIDBits value other than 0 or 2 is not 16 bits */
        };
        for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; ++w) {
            struct app_asid_pool pool;
            app_asid_pool_init(&pool, widths[w].mmfr, widths[w].tcr);
            assert(pool.limit == widths[w].limit);
            for (unsigned i = 1; i <= widths[w].limit; ++i) assert(app_asid_alloc(&pool) == i);
            assert(app_asid_alloc(&pool) == 0 && app_asid_alloc(&pool) == 0);
        }
        assert(app_asid_alloc(0) == 0);
        /* A 16-bit ASID travels in TTBR0 bits 63:48, and its TLB entries stay apart from the 8-bit ones. */
        struct app_space wide;
        struct app_table_arena wide_arena = {arena_pages, arena_pa, 16, 0};
        assert(app_space_build(&wide, &wide_arena, base_l1, &l, 1) == 0);
        wide.asid = 0x1234;
        assert((app_space_ttbr0(&wide) >> 48) == 0x1234 && (app_space_ttbr0(&wide) & APP_PTE_ADDR_MASK) == wide.root_physical);
    }
    large_cases();
    puts("app-region host check passed");
    return 0;
}
