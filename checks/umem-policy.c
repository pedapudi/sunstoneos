/* Host check of the cache-policy conversion (kernel/umem/umem_policy.c, docs/designs/unified-memory.md).
 *
 * A model of AArch64 stage-1 translation stands behind umem_cpu_ops: three roots (the kernel's, and two application
 * roots with their own level-2 table for the first gigabyte above the boot image), a TLB that keeps every translation it
 * walked until a TLBI removes it, and cores that touch the chunk between every instruction, as speculation does. After
 * each primitive the model collects every cache attribute through which the chunk is reachable, in any table or any TLB
 * entry. The conversion passes only if that set never holds two attributes (no mismatched alias), if DC CIVAC completes
 * through the old valid translation before the break, and if the whole sequence has the break-before-make order. */
#include "umem.h"
#include "umem_policy.h"
#include "a733_device_memory_layout.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what, uint64_t a, uint64_t b) { printf("FAIL: %s (%#" PRIx64 ", %#" PRIx64 ")\n", what, a, b); exit(1); }
#define CHECK(c, what, a, b) do { if (!(c)) fail(what, (uint64_t)(a), (uint64_t)(b)); } while (0)

#define ATTR(d) (((d) >> 2) & 7)
#define BLOCK UINT64_C(0x200000)
#define GIB UINT64_C(0x40000000)

static uint64_t *l1[3], *kernel_l2, *private_l2[2];
static struct tlb { int used; unsigned root; uint64_t va, desc, span; } tlb[4096];

static uint64_t *page(void) { uint64_t *p = aligned_alloc(4096, 4096); memset(p, 0, 4096); return p; }

/* A translation of va in a root: the descriptor of its 2 MiB block, or 0 when invalid. */
static uint64_t walk(unsigned root, uint64_t va)
{
    uint64_t e = l1[root][va >> 30];
    if ((e & 3) == 1) return e;                          /* a 1 GiB block */
    if ((e & 3) != 3) return 0;
    uint64_t d = ((uint64_t *)(uintptr_t)(e & ~UINT64_C(0xfff)))[(va >> 21) & 511];
    return (d & 3) == 1 ? d : 0;
}
static uint64_t translate(unsigned root, uint64_t va)
{
    for (unsigned i = 0; i < 4096; ++i)
        if (tlb[i].used && tlb[i].root == root && tlb[i].va == (va & ~(BLOCK - 1))) return tlb[i].desc;
    uint64_t d = walk(root, va);
    if (d)
        for (unsigned i = 0; i < 4096; ++i)
            if (!tlb[i].used) {
                uint64_t span = (l1[root][va >> 30] & 3) == 1 ? GIB : BLOCK;
                tlb[i] = (struct tlb){1, root, va & ~(BLOCK - 1), d, span}; break;
            }
    return d;
}

static uint64_t watched;                                  /* the chunk under conversion */
static unsigned mismatches, ops_seen;
static char sequence[256];
static unsigned sequence_length;
static int civac_fails, skip_tlbi;

static void note(char c) { if (sequence_length < sizeof sequence - 1) sequence[sequence_length++] = c; }

/* Every attribute through which the chunk is reachable right now. */
static void audit(void)
{
    if (!watched) return;
    int seen[8] = {0};
    for (unsigned r = 0; r < 3; ++r) {
        uint64_t d = walk(r, watched);
        if (d) seen[ATTR(d)] = 1;
    }
    for (unsigned i = 0; i < 4096; ++i)
        if (tlb[i].used && tlb[i].va == watched) seen[ATTR(tlb[i].desc)] = 1;
    unsigned kinds = 0;
    for (unsigned a = 0; a < 8; ++a) kinds += seen[a];
    if (kinds > 1) ++mismatches;
}
/* The cores touch the chunk between instructions and fill the TLBs. */
static void speculate(void)
{
    if (!watched) return;
    for (unsigned r = 0; r < 3; ++r) translate(r, watched + (uint64_t)(ops_seen % 16) * 4096);
    ++ops_seen;
    audit();
}

static void op_store(void *c, uint64_t *e, uint64_t v) { (void)c; *e = v; note((v & 1) ? 'M' : 'B'); speculate(); }
static void op_dsb(void *c) { (void)c; note('d'); speculate(); }
static void op_isb(void *c) { (void)c; note('i'); speculate(); }
static void op_tlbi(void *c, uint64_t va)
{
    (void)c;
    note('t');
    if (!skip_tlbi)
        /* Invalidation of a block covers all cached fragments derived from
         * its descriptor (Linux arm64 fix ff4c5a0de1f2, tlbflush.h). */
        for (unsigned i = 0; i < 4096; ++i)
            if (tlb[i].used && va >= (tlb[i].va & ~(tlb[i].span - 1)) &&
                va - (tlb[i].va & ~(tlb[i].span - 1)) < tlb[i].span) tlb[i].used = 0;
    speculate();
}
static int op_civac(void *c, uint64_t va, uint64_t bytes)
{
    (void)c;
    note('v');
    CHECK(sequence_length == 1, "cache maintenance precedes every table edit", sequence_length, 0);
    /* Cache maintenance by VA needs a valid translation in the executing core's root (the kernel's), page by page
     * through the 2 MiB block. */
    for (uint64_t a = va; a < va + bytes; a += BLOCK) CHECK(translate(0, a) != 0, "DC CIVAC through an unmapped address", a, 0);
    speculate();
    return civac_fails ? -1 : 0;
}
static const struct umem_cpu_ops cpu = {0, op_store, op_dsb, op_isb, op_tlbi, op_civac};

static uint64_t split_pages_storage[UMEM_POLICY_UPPER_SLOTS][512] __attribute__((aligned(4096)));
static struct umem_el1_map map;
static struct umem_policy_table kernel_table, private_tables[2];

static void setup(void)
{
    for (unsigned r = 0; r < 3; ++r) l1[r] = page();
    kernel_l2 = page(); private_l2[0] = page(); private_l2[1] = page();
    memset(tlb, 0, sizeof tlb);
    memset(split_pages_storage, 0, sizeof split_pages_storage);
    /* The kernel's table: slot 1 is a level-2 table of 2 MiB write-back blocks, slots 2 to 12 are 1 GiB blocks. The
     * first 64 blocks of slot 1 are EL0 read-write (the AP bits differ), like the boot table's. */
    for (unsigned j = 0; j < 512; ++j) {
        uint64_t pa = UINT64_C(0x40000000) + j * BLOCK;
        kernel_l2[j] = A733_EL1_DRAM_BLOCK(pa) | (j < 64 ? UINT64_C(1) << 6 : 0);
    }
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned slot = 2; slot < 13; ++slot) l1[r][slot] = A733_EL1_DRAM_BLOCK((uint64_t)slot << 30);
    }
    l1[0][1] = (uint64_t)(uintptr_t)kernel_l2 | 3;
    for (unsigned a = 0; a < 2; ++a) {
        memcpy(private_l2[a], kernel_l2, 4096);
        l1[1 + a][1] = (uint64_t)(uintptr_t)private_l2[a] | 3;
    }
    watched = 0; mismatches = 0; ops_seen = 0; sequence_length = 0; civac_fails = 0; skip_tlbi = 0;
    kernel_table = (struct umem_policy_table){kernel_l2, 0, 0};
    CHECK(umem_policy_init(&map, &cpu, l1[0], &kernel_table, split_pages_storage) == 0, "init", 0, 0);
    private_tables[0] = (struct umem_policy_table){private_l2[0], UINT64_C(0x40000000), 0};
    private_tables[1] = (struct umem_policy_table){private_l2[1], UINT64_C(0x40000000), 0};
    umem_policy_add_table(&map, &private_tables[0]);
    umem_policy_add_table(&map, &private_tables[1]);
    umem_policy_note_space(&map);
    umem_policy_note_space(&map);
}

static int convert(uint64_t chunk, unsigned policy)
{
    watched = chunk;
    sequence_length = 0; mismatches = 0;
    int r = umem_policy_convert(&map, chunk, policy);
    sequence[sequence_length] = 0;
    watched = 0;
    return r;
}

static void case_slot1(void)
{
    setup();
    uint64_t chunk = UINT64_C(0x50000000);       /* block 128 of slot 1 */
    uint64_t before_a = private_l2[0][128];
    for (unsigned r = 0; r < 3; ++r) CHECK(ATTR(translate(r, chunk)) == 1, "write-back before", r, 0);   /* the TLBs now hold it */
    CHECK(convert(chunk, UMEM_NC) == 0, "convert to NC", 0, 0);
    CHECK(mismatches == 0, "no mismatched alias at any instruction", mismatches, 0);
    printf("  sequence %s\n", sequence);
    CHECK(strcmp(sequence, "vBBBdtdiMMMdi") == 0, "clean-before-break and break-before-make order", 0, 0);
    for (unsigned r = 0; r < 3; ++r) CHECK(ATTR(walk(r, chunk)) == 2, "NC in every root", r, 0);
    for (unsigned r = 0; r < 3; ++r) CHECK(ATTR(translate(r, chunk)) == 2, "NC in every TLB", r, 0);
    CHECK((private_l2[0][128] & ~(UINT64_C(7) << 2)) == (before_a & ~(UINT64_C(7) << 2)), "only the attribute changes", private_l2[0][128], before_a);
    /* The neighbors keep theirs. */
    CHECK(ATTR(kernel_l2[127]) == 1 && ATTR(kernel_l2[129]) == 1 && ATTR(private_l2[1][129]) == 1, "neighbors", 0, 0);
    /* The EL0 read-write blocks keep their access bits when they change. */
    uint64_t low = UINT64_C(0x42200000);
    CHECK(convert(low, UMEM_NC) == 0 && (kernel_l2[0x11] & (UINT64_C(1) << 6)), "AP bits survive", kernel_l2[0x11], 0);
    CHECK(mismatches == 0, "no mismatched alias", mismatches, 0);
    /* And back. */
    CHECK(convert(chunk, UMEM_WB) == 0 && mismatches == 0, "convert back", mismatches, 0);
    for (unsigned r = 0; r < 3; ++r) CHECK(ATTR(translate(r, chunk)) == 1, "write-back again", r, 0);
    CHECK(convert(chunk, UMEM_WB) == -1, "no change is refused", 0, 0);
}

static void case_split(void)
{
    setup();
    /* Splitting is a boot step, before any application root copies the table. */
    map.spaces = 0;
    uint64_t chunk = UINT64_C(0x100000000) + 3 * BLOCK;
    CHECK(convert(chunk, UMEM_NC) == -1, "an unsplit slot refuses", 0, 0);
    for (unsigned r = 0; r < 3; ++r) {
        (void)translate(r, chunk);
        (void)translate(r, UINT64_C(0x120000000));
    }
    CHECK(umem_policy_split(&map, 4) == 0, "split slot 4", 0, 0);
    for (unsigned i = 0; i < 4096; ++i) CHECK(!tlb[i].used, "split removes translations away from slot base", i, 0);
    for (unsigned j = 0; j < 512; ++j) {
        uint64_t d = split_pages_storage[2][j];
        CHECK((d & 3) == 1 && ATTR(d) == 1 && (d & UINT64_C(0x0000ffffffe00000)) == (UINT64_C(0x100000000) + j * BLOCK), "split entries", j, d);
    }
    CHECK((l1[0][4] & 3) == 3 && ((l1[0][4] & ~UINT64_C(0xfff)) == (uint64_t)(uintptr_t)split_pages_storage[2]), "level-1 entry is the table", l1[0][4], 0);
    CHECK(umem_policy_split(&map, 4) == 0, "split twice", 0, 0);
    CHECK(umem_policy_split(&map, 1) == -1 && umem_policy_split(&map, 13) == -1, "slots that cannot split", 0, 0);
    /* The application roots still hold 1 GiB blocks; the conversion would leave them write-back, so it is refused
     * for a slot that has roots to copy. */
    setup();
    CHECK(umem_policy_split(&map, 4) == -1, "no split once an application root exists", 0, 0);
    setup();
    map.spaces = 0;
    CHECK(umem_policy_split(&map, 4) == 0, "split", 0, 0);
    /* Only the kernel root and the split page remain as tables of that gigabyte; the application roots copy later. */
    watched = 0;
    CHECK(convert(chunk, UMEM_NC) == 0, "convert in a split slot", 0, 0);
    CHECK(ATTR(split_pages_storage[2][3]) == 2 && ATTR(split_pages_storage[2][2]) == 1, "only that block", 0, 0);
}

static void case_refusals(void)
{
    setup();
    uint64_t chunk = UINT64_C(0x50000000);
    CHECK(convert(chunk + 4096, UMEM_NC) == -1 && convert(UINT64_C(0x30000000), UMEM_NC) == -1, "unaligned or below DRAM", 0, 0);
    CHECK(convert(chunk, 5) == -1, "bad policy", 0, 0);
    CHECK(convert(UINT64_C(0x340000000), UMEM_NC) == -1 &&
          convert(UINT64_C(0x10000000000), UMEM_NC) == -1, "upper bounds precede split-mask shifts", 0, 0);
    kernel_l2[128] ^= BLOCK;
    CHECK(convert(chunk, UMEM_NC) == -1 && sequence_length == 0, "wrong physical output refuses without maintenance", 0, 0);
    kernel_l2[128] = A733_EL1_DRAM_BLOCK(chunk) & ~(UINT64_C(7) << 2);
    CHECK(convert(chunk, UMEM_NC) == -1 && sequence_length == 0, "device attribute refuses", 0, 0);
    kernel_l2[128] = 0;                                             /* an unmapped chunk */
    CHECK(convert(chunk, UMEM_NC) == -1, "a table without the block", 0, 0);
    kernel_l2[128] = A733_EL1_DRAM_BLOCK(chunk);
    civac_fails = 1;
    uint64_t before = kernel_l2[129];
    CHECK(convert(UINT64_C(0x50200000), UMEM_NC) == -1, "a failed invalidate is reported", 0, 0);
    CHECK(kernel_l2[129] == before && private_l2[0][129] == before && private_l2[1][129] == before,
          "cache refusal changes no mapping", 0, 0);
    civac_fails = 0;
    /* The ledger's convert hook with the real conversion: an NC request converts and the model sees one sequence. */
    static uint8_t chunks[64];
    static struct umem u;
    static uint8_t cells[64 * 128];
    struct umem_hooks h = {&map, 0, 0, 0, umem_policy_convert, 0};
    struct umem_range range = {UINT64_C(0x50000000), 32 * BLOCK};
    CHECK(umem_init(&u, &h, cells, 64, chunks, UINT64_C(0x50000000), 64, &range, 1) == 0, "ledger over the model", 0, 0);
    struct umem_buf b;
    struct umem_request r = {.bytes = 4096, .owner = 1, .policy = UMEM_NC};
    civac_fails = 1;
    sequence_length = 0;
    CHECK(umem_alloc(&u, &r, &b) == UMEM_HW, "ledger reports cache refusal", 0, 0);
    CHECK(chunks[0] == UMEM_WB && ATTR(kernel_l2[128]) == 1 && umem_check(&u) == 0,
          "cache refusal keeps ledger and mappings WB", 0, 0);
    civac_fails = 0;
    sequence_length = 0;
    CHECK(umem_alloc(&u, &r, &b) == 0, "NC alloc", 0, 0);
    CHECK(ATTR(kernel_l2[(b.base - UINT64_C(0x40000000)) / BLOCK]) == 2, "the chunk of the buffer is NC", b.base, 0);
    CHECK(umem_check(&u) == 0, "ledger check", umem_check(&u), 0);
    /* A free subextent of a chunk containing a live WB allocation cannot convert. */
    struct umem_buf in_use;
    struct umem_request occupied = {.bytes = 4096, .base = UINT64_C(0x50400000), .owner = 7,
                                    .flags = UMEM_AT | UMEM_RAW};
    CHECK(umem_alloc(&u, &occupied, &in_use) == 0, "WB allocation", 0, 0);
    r.base = occupied.base + 4096; r.flags = UMEM_AT;
    sequence_length = 0;
    CHECK(umem_alloc(&u, &r, &b) == UMEM_NO_SPACE && sequence_length == 0,
          "chunk in use refuses without cache or table operations", 0, 0);
    CHECK(umem_owner_of(&u, occupied.base) == 7 && umem_check(&u) == 0, "live allocation is preserved", 0, 0);
}

/* Negative control: with the TLB invalidate removed, a stale write-back translation meets the new non-cacheable entries,
 * and the audit must see the mismatch. */
static void case_negative(void)
{
    setup();
    uint64_t chunk = UINT64_C(0x50000000);
    for (unsigned r = 0; r < 3; ++r) translate(r, chunk);
    skip_tlbi = 1;
    CHECK(convert(chunk, UMEM_NC) == 0 && mismatches > 0, "the audit sees a stale translation", mismatches, 0);
}

int main(void)
{

    case_slot1();
    case_split();
    case_refusals();
    case_negative();
    printf("UMEM-POLICY PASS\n");
    return 0;
}
