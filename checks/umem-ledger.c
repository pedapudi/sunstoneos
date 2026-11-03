/* Host check of the unified memory ledger (kernel/umem/umem.c, docs/designs/unified-memory.md).
 *
 * A model of the board's memory stands behind the hooks: a page owner map, the policy of each chunk, the metadata
 * pages the ledger carves from itself and a log of the maintenance it asks for. The cases check the ledger's own
 * walk (umem_check) after every group of operations, and the model's view of each result: extents never overlap,
 * every extent meets its alignment, window and physical limit, the policy of every chunk matches the buffers in it,
 * free bytes plus allocated bytes equal the usable bytes, and generations are never reused. */
#include "umem.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB UINT64_C(0x100000)
#define GIB UINT64_C(0x40000000)
#define DRAM_BASE UINT64_C(0x40000000)
#define DRAM_END UINT64_C(0x340000000)
#define LOW UINT64_C(0x42200000)
#define HOLE_LO UINT64_C(0x79000000)
#define HOLE_HI UINT64_C(0x80000000)
#define PAGES ((DRAM_END - DRAM_BASE) / UMEM_PAGE)
#define CHUNKS ((DRAM_END - LOW) / UMEM_CHUNK)

static void fail(const char *what, uint64_t a, uint64_t b)
{
    printf("FAIL: %s (%#" PRIx64 ", %#" PRIx64 ")\n", what, a, b);
    exit(1);
}
#define CHECK(c, what, a, b) do { if (!(c)) fail(what, (uint64_t)(a), (uint64_t)(b)); } while (0)

/* ---- the model ---- */
static uint8_t page_owner[PAGES];                 /* 0 free, else the low byte of the owner (kernel is 0xff) */
static uint8_t model_policy[CHUNKS];
static uint8_t chunk_storage[CHUNKS];
static struct { uint64_t phys; void *ptr; } meta_map[1 << 16];
static unsigned locks, max_lock_depth;
static int convert_fail;
static unsigned converts;
struct op_log { enum umem_op op; uint64_t base, bytes; };
static struct op_log log_ops[64];
static unsigned log_count;
static uint64_t zeroed_bytes, cleaned_bytes;

static void h_lock(void *c) { (void)c; ++locks; if (locks > max_lock_depth) max_lock_depth = locks; CHECK(locks == 1, "lock nesting", locks, 0); }
static void h_unlock(void *c) { (void)c; CHECK(locks == 1, "unlock without lock", locks, 0); --locks; }
static void *h_virt(void *c, uint64_t phys)
{
    (void)c;
    uint64_t h = (phys >> 12) * 0x9e3779b97f4a7c15ull >> 48;
    for (;; h = (h + 1) & 0xffff) {
        if (meta_map[h].ptr && meta_map[h].phys == phys) return meta_map[h].ptr;
        if (!meta_map[h].ptr) {
            meta_map[h].phys = phys;
            meta_map[h].ptr = aligned_alloc(4096, 4096);
            memset(meta_map[h].ptr, 0xa5, 4096);
            return meta_map[h].ptr;
        }
    }
}
static int h_convert(void *c, uint64_t chunk, unsigned policy)
{
    (void)c;
    ++converts;
    CHECK(locks == 1, "convert outside the lock", 0, 0);
    CHECK(!(chunk & (UMEM_CHUNK - 1)) && chunk >= LOW && chunk < DRAM_END, "convert chunk", chunk, 0);
    for (uint64_t p = (chunk - DRAM_BASE) / UMEM_PAGE; p < (chunk - DRAM_BASE + UMEM_CHUNK) / UMEM_PAGE; ++p)
        CHECK(!page_owner[p], "converted chunk holds an allocated page", chunk, p);
    if (convert_fail) return -1;
    CHECK(model_policy[(chunk - LOW) / UMEM_CHUNK] != policy, "converted to the same policy", chunk, policy);
    model_policy[(chunk - LOW) / UMEM_CHUNK] = (uint8_t)policy;
    return 0;
}
static int h_maintain(void *c, enum umem_op op, uint64_t base, uint64_t bytes)
{
    (void)c;
    if (log_count < 64) log_ops[log_count++] = (struct op_log){op, base, bytes};
    if (op == UMEM_OP_ZERO) zeroed_bytes += bytes;
    if (op == UMEM_OP_CLEAN) cleaned_bytes += bytes;
    return 0;
}
static const struct umem_hooks hooks = {0, h_lock, h_unlock, h_virt, h_convert, h_maintain};

static struct umem ledger;
static void *cells0;
static void model_reset(size_t cells)
{
    memset(page_owner, 0, sizeof page_owner);
    memset(model_policy, 0, sizeof model_policy);
    memset(meta_map, 0, sizeof meta_map);
    converts = 0; convert_fail = 0; log_count = 0; zeroed_bytes = cleaned_bytes = 0;
    free(cells0);
    cells0 = calloc(cells, umem_cell_bytes());
    struct umem_range ranges[2] = {{LOW, HOLE_LO - LOW}, {HOLE_HI, DRAM_END - HOLE_HI}};
    CHECK(umem_init(&ledger, &hooks, cells0, cells, chunk_storage, LOW, CHUNKS, ranges, 2) == 0, "init", 0, 0);
    CHECK(umem_check(&ledger) == 0, "init check", umem_check(&ledger), 0);
}

static void mark(uint64_t base, uint64_t bytes, uint8_t owner)
{
    for (uint64_t p = (base - DRAM_BASE) / UMEM_PAGE; p < (base - DRAM_BASE + bytes) / UMEM_PAGE; ++p) {
        CHECK(owner ? !page_owner[p] : page_owner[p], owner ? "extents overlap" : "free of a free page", base, p);
        page_owner[p] = owner;
    }
}
static void expect_policy(uint64_t base, uint64_t bytes, unsigned policy)
{
    for (uint64_t a = base & ~(UMEM_CHUNK - 1); a < base + bytes; a += UMEM_CHUNK)
        CHECK(model_policy[(a - LOW) / UMEM_CHUNK] == policy, "chunk policy of a buffer", a, policy);
}

static struct umem_request req(uint64_t bytes, uint16_t owner)
{
    return (struct umem_request){.bytes = bytes, .owner = owner};
}
static int alloc_ready(struct umem_request r, struct umem_buf *b)
{
    int s = umem_alloc(&ledger, &r, b);
    if (s) return s;
    int d;
    while ((d = umem_prepare_step(&ledger, b, 0)) == 0) {}
    CHECK(d == 1, "prepare", d, 0);
    CHECK(umem_ready(&ledger, b) == 1, "ready", 0, 0);
    return 0;
}
/* Take a buffer and record it in the model, with every constraint checked. */
static void model_take(const struct umem_buf *b, const struct umem_request *r)
{
    uint64_t align = r->align > UMEM_PAGE ? r->align : UMEM_PAGE;
    CHECK(!(b->base & (align - 1)), "alignment", b->base, align);
    CHECK(b->bytes >= r->bytes && b->bytes - r->bytes < UMEM_PAGE, "rounding", b->bytes, r->bytes);
    if (r->phys_max) CHECK(b->base + b->bytes <= r->phys_max, "phys_max", b->base + b->bytes, r->phys_max);
    if (r->window) CHECK((b->base & ~(r->window - 1)) == ((b->base + b->bytes - 1) & ~(r->window - 1)), "window", b->base, r->window);
    CHECK(!(b->base >= HOLE_LO && b->base < HOLE_HI) && b->base >= LOW && b->base + b->bytes <= DRAM_END, "range", b->base, b->bytes);
    mark(b->base, b->bytes, (uint8_t)r->owner);
    expect_policy(b->base, b->bytes, r->policy);
}
static void model_give(const struct umem_buf *b) { mark(b->base, b->bytes, 0); }

/* ---- cases ---- */
static void case_basic(void)
{
    model_reset(UMEM_MIN_CELLS);
    uint64_t total = ledger.usable_bytes;
    CHECK(total == (HOLE_LO - LOW) + (DRAM_END - HOLE_HI), "usable bytes", total, 0);
    struct umem_buf a, b, c, d;
    struct umem_request ra = req(5000, 1), rb = req(3 * MIB, 2);
    CHECK(alloc_ready(ra, &a) == 0, "alloc", 0, 0);
    CHECK(alloc_ready(rb, &b) == 0, "alloc", 0, 0);
    model_take(&a, &ra); model_take(&b, &rb);
    CHECK(a.bytes == 8192, "page rounding", a.bytes, 0);
    CHECK(umem_owner_of(&ledger, a.base) == 1 && umem_owner_of(&ledger, a.base + 8191) == 1 &&
          umem_owner_of(&ledger, a.base + 8192) != 1, "owner_of", a.base, 0);
    CHECK(umem_owner_of(&ledger, HOLE_LO + 0x1000) == 0, "owner of the hole", 0, 0);
    CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), 0);
    /* A request of 1 GiB or more is placed on a 1 GiB boundary when one is free. */
    struct umem_request rg = req(GIB + 2 * MIB, 3);
    CHECK(alloc_ready(rg, &c) == 0, "gib alloc", 0, 0);
    model_take(&c, &rg);
    CHECK(!(c.base & (GIB - 1)), "gib boundary", c.base, 0);
    /* Constraints: physical limit, window, device alignment, pinned address. */
    struct umem_request rl = req(4 * MIB, 4); rl.phys_max = LOW + 16 * MIB;
    CHECK(alloc_ready(rl, &d) == 0, "limited alloc", 0, 0);
    model_take(&d, &rl);
    struct umem_request rw = req(UMEM_PAGE * 3, 4); rw.window = UMEM_PAGE * 4;
    struct umem_buf w;
    CHECK(alloc_ready(rw, &w) == 0, "window alloc", 0, 0);
    model_take(&w, &rw);
    struct umem_request rn = req(20 * MIB, 5); rn.device_mask = UMEM_DEV_NPU;
    struct umem_buf n;
    CHECK(alloc_ready(rn, &n) == 0, "npu alloc", 0, 0);
    rn.align = 16 * MIB; model_take(&n, &rn);
    struct umem_request rn1 = req(2 * MIB, 5); rn1.device_mask = UMEM_DEV_NPU;
    struct umem_buf n1;
    CHECK(alloc_ready(rn1, &n1) == 0, "npu 2 MiB alloc", 0, 0);
    rn1.align = MIB; model_take(&n1, &rn1);
    struct umem_request rg1 = req(300 * 1024, 5); rg1.device_mask = UMEM_DEV_GPU;
    struct umem_buf g;
    CHECK(alloc_ready(rg1, &g) == 0, "gpu alloc", 0, 0);
    rg1.align = 256 * 1024; model_take(&g, &rg1);
    struct umem_request ra1 = req(8 * MIB, 6); ra1.flags = UMEM_AT; ra1.base = 0x180000000ull;
    struct umem_buf p;
    CHECK(alloc_ready(ra1, &p) == 0 && p.base == 0x180000000ull, "pinned alloc", p.base, 0);
    model_take(&p, &ra1);
    struct umem_buf p2;
    CHECK(umem_alloc(&ledger, &ra1, &p2) == UMEM_NO_SPACE, "pinned twice", 0, 0);
    /* A device above its working limit is refused until the limit is raised. */
    struct umem_request rhi = req(MIB, 7); rhi.device_mask = UMEM_DEV_NPU; rhi.flags = UMEM_AT; rhi.base = 0x100000000ull;
    CHECK(umem_alloc(&ledger, &rhi, &p2) == UMEM_INVALID, "npu above its working limit", 0, 0);
    CHECK(umem_device_limit_set(&ledger, 0, UINT64_C(1) << 40) == 0, "limit set", 0, 0);
    CHECK(alloc_ready(rhi, &p2) == 0, "npu above 4 GiB after the limit rises", 0, 0);
    rhi.align = MIB; model_take(&p2, &rhi);
    /* Refusals change nothing. */
    uint64_t before = umem_free_bytes(&ledger);
    struct umem_request big = req(13 * GIB, 8);
    CHECK(umem_alloc(&ledger, &big, &p2) == UMEM_NO_SPACE, "too big", 0, 0);
    struct umem_request bad = req(0, 8);
    CHECK(umem_alloc(&ledger, &bad, &p2) == UMEM_INVALID, "zero bytes", 0, 0);
    bad = req(4096, 0);
    CHECK(umem_alloc(&ledger, &bad, &p2) == UMEM_INVALID, "no owner", 0, 0);
    CHECK(umem_free_bytes(&ledger) == before && umem_check(&ledger) == 0, "refusals change nothing", 0, 0);
    /* Free everything: the free space merges back to the usable ranges. */
    struct umem_buf all[] = {a, b, c, d, w, n, n1, g, p};
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; ++i) { CHECK(umem_free(&ledger, &all[i]) == 0, "free", i, 0); model_give(&all[i]); }
    CHECK(umem_free(&ledger, &(struct umem_buf){0}) != 0, "free of a null handle", 0, 0);
    struct umem_buf last = {0};
    (void)last;
    CHECK(umem_check(&ledger) == 0, "final check", umem_check(&ledger), 0);
    /* The ledger's own pages are the only allocated memory left (plus the freed NPU buffer above 4 GiB, still held). */
    struct umem_extent e[64];
    unsigned kernel = umem_extents_of(&ledger, UMEM_OWNER_KERNEL, e, 64);
    CHECK(kernel == 0 || ledger.meta_bytes == kernel * UMEM_PAGE, "metadata pages are kernel-owned", kernel, ledger.meta_bytes);
}

static void case_handles(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf a, b;
    struct umem_request r = req(MIB, 1);
    CHECK(alloc_ready(r, &a) == 0, "alloc", 0, 0);
    struct umem_extent e;
    int state = -1;
    CHECK(umem_describe(&ledger, &a, &e, &state) == 0 && e.base == a.base && e.bytes == MIB && e.owner == 1 && state == UMEM_CPU,
          "describe", 0, 0);
    CHECK(umem_free(&ledger, &a) == 0, "free", 0, 0);
    CHECK(umem_describe(&ledger, &a, &e, &state) == UMEM_STALE, "describe of a freed buffer", 0, 0);
    CHECK(umem_free(&ledger, &a) == UMEM_NOT_OWNED, "double free", 0, 0);
    CHECK(alloc_ready(r, &b) == 0 && b.base == a.base, "reuse of the extent", b.base, a.base);
    CHECK(b.generation != a.generation, "generation reuse", b.generation, a.generation);
    CHECK(umem_describe(&ledger, &a, &e, &state) == UMEM_STALE, "old handle after reuse", 0, 0);
    CHECK(umem_free(&ledger, &a) == UMEM_NOT_OWNED, "free through an old handle", 0, 0);
    CHECK(umem_free(&ledger, &b) == 0, "free", 0, 0);
    CHECK(umem_check(&ledger) == 0, "check", 0, 0);
}

static void case_prepare(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf a, guard;
    struct umem_request r = req(10 * UMEM_PAGE, 1), rg = req(UMEM_PAGE, 9);
    CHECK(umem_alloc(&ledger, &r, &a) == 0 && umem_ready(&ledger, &a) == 0, "alloc is not ready until prepared", 0, 0);
    CHECK(alloc_ready(rg, &guard) == 0 && guard.base == a.base + a.bytes, "a neighbor that keeps the extent from merging", 0, 0);
    uint64_t z0 = zeroed_bytes, c0 = cleaned_bytes;
    unsigned steps = 0;
    int d;
    while ((d = umem_prepare_step(&ledger, &a, 3 * UMEM_PAGE)) == 0) ++steps;
    CHECK(d == 1 && steps >= 5, "sliced preparation", steps, 0);
    CHECK(zeroed_bytes - z0 == a.bytes && cleaned_bytes - c0 == a.bytes, "zero then clean of the whole buffer", zeroed_bytes, cleaned_bytes);
    CHECK(umem_handoff(&ledger, &a, UMEM_DEVICE, UMEM_CPU, 0, 0) == UMEM_BUSY, "wrong source state", 0, 0);
    /* A never-touched buffer frees clean: a REUSE by the same owner needs no zero and no clean. */
    CHECK(umem_free(&ledger, &a) == 0, "free", 0, 0);
    r.flags = UMEM_REUSE;
    z0 = zeroed_bytes; c0 = cleaned_bytes;
    CHECK(alloc_ready(r, &a) == 0 && zeroed_bytes == z0 && cleaned_bytes == c0, "reuse of a clean extent by its owner", 0, 0);
    /* A buffer the CPU touched frees dirty: the same owner's REUSE still cleans, without zeroing. */
    CHECK(umem_touch(&ledger, &a) == 0 && umem_free(&ledger, &a) == 0, "touch and free", 0, 0);
    z0 = zeroed_bytes; c0 = cleaned_bytes;
    CHECK(alloc_ready(r, &a) == 0 && zeroed_bytes == z0 && cleaned_bytes - c0 == a.bytes, "reuse of a dirty extent cleans", 0, 0);
    /* Another owner gets zeros whatever the flag says. */
    CHECK(umem_free(&ledger, &a) == 0, "free", 0, 0);
    struct umem_request other = req(10 * UMEM_PAGE, 2);
    other.flags = UMEM_REUSE;
    z0 = zeroed_bytes;
    CHECK(alloc_ready(other, &a) == 0 && zeroed_bytes - z0 == a.bytes, "another owner is zeroed", 0, 0);
    /* The dirty flag survives a merge: free two neighbors, one dirty, and allocate across them. */
    struct umem_buf x, y;
    struct umem_request rx = req(UMEM_PAGE * 4, 3);
    CHECK(alloc_ready(rx, &x) == 0 && alloc_ready(rx, &y) == 0, "two buffers", 0, 0);
    CHECK(umem_touch(&ledger, &y) == 0, "touch", 0, 0);
    CHECK(umem_free(&ledger, &x) == 0 && umem_free(&ledger, &y) == 0, "free both", 0, 0);
    struct umem_request rxy = req(UMEM_PAGE * 8, 3);
    rxy.flags = UMEM_REUSE;
    c0 = cleaned_bytes; z0 = zeroed_bytes;
    CHECK(alloc_ready(rxy, &a) == 0, "alloc across the merge", 0, 0);
    CHECK(cleaned_bytes - c0 == a.bytes, "merged extent is dirty", cleaned_bytes - c0, a.bytes);
    CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), 0);
}

static void case_handoff(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf wb, nc;
    struct umem_request rw = req(MIB, 1), rn = req(MIB, 1);
    rn.policy = UMEM_NC;
    CHECK(alloc_ready(rw, &wb) == 0 && alloc_ready(rn, &nc) == 0, "alloc", 0, 0);
    CHECK(converts > 0, "an NC request converts a chunk", converts, 0);
    expect_policy(nc.base, nc.bytes, UMEM_NC);
    log_count = 0;
    struct umem_range set[2] = {{0, UMEM_PAGE}, {MIB / 2, 2 * UMEM_PAGE}};
    CHECK(umem_handoff(&ledger, &wb, UMEM_CPU, UMEM_DEVICE, set, 2) == 0, "cpu to device", 0, 0);
    CHECK(log_count == 2 && log_ops[0].op == UMEM_OP_CLEAN && log_ops[0].base == wb.base && log_ops[0].bytes == UMEM_PAGE &&
          log_ops[1].op == UMEM_OP_CLEAN && log_ops[1].base == wb.base + MIB / 2 && log_ops[1].bytes == 2 * UMEM_PAGE,
          "clean of the write set", log_count, 0);
    log_count = 0;
    CHECK(umem_handoff(&ledger, &wb, UMEM_DEVICE, UMEM_DEVICE, 0, 0) == 0 && log_count == 0, "device to device needs no maintenance", log_count, 0);
    CHECK(umem_handoff(&ledger, &wb, UMEM_DEVICE, UMEM_CPU, set, 2) == 0 && log_count == 2 && log_ops[0].op == UMEM_OP_CLEAN_INVALIDATE,
          "device to cpu cleans and invalidates", log_count, 0);
    log_count = 0;
    CHECK(umem_handoff(&ledger, &nc, UMEM_CPU, UMEM_DEVICE, 0, 0) == 0 && log_count == 1 && log_ops[0].op == UMEM_OP_STORE_BARRIER,
          "NC to device is a store barrier", log_count, 0);
    log_count = 0;
    CHECK(umem_handoff(&ledger, &nc, UMEM_DEVICE, UMEM_CPU, 0, 0) == 0 && log_count == 1 && log_ops[0].op == UMEM_OP_LOAD_BARRIER,
          "NC to cpu is a load barrier", log_count, 0);
    CHECK(umem_handoff(&ledger, &wb, UMEM_CPU, UMEM_DEVICE, &(struct umem_range){MIB, 1}, 1) == UMEM_INVALID, "range outside the buffer", 0, 0);
    CHECK(umem_handoff(&ledger, &wb, UMEM_CPU, UMEM_DEVICE, 0, 0) == 0 && umem_free(&ledger, &wb) == UMEM_BUSY,
          "a device-owned buffer is not freed", 0, 0);
    CHECK(umem_handoff(&ledger, &wb, UMEM_DEVICE, UMEM_RETAINED, 0, 0) == 0 &&
          umem_handoff(&ledger, &wb, UMEM_RETAINED, UMEM_CPU, 0, 0) == UMEM_BUSY, "retained stays retained", 0, 0);
}

/* A model address space: records its mappings. */
struct fake_space { unsigned maps, unmaps, flushes; int busy; uint64_t next; };
static int fs_map(void *s, const struct umem_buf *b, uint64_t o, uint64_t n, unsigned prot, uint64_t *a)
{
    (void)prot; (void)n;
    struct fake_space *f = s; ++f->maps;
    *a = f->next + o; f->next += b->bytes + UMEM_PAGE;
    return 0;
}
static int fs_unmap(void *s, uint64_t a, uint64_t n) { (void)a; (void)n; ++((struct fake_space *)s)->unmaps; return 0; }
static int fs_flush(void *s, uint64_t a, uint64_t n) { (void)a; (void)n; ++((struct fake_space *)s)->flushes; return 0; }
static int fs_quiesced(void *s) { return !((struct fake_space *)s)->busy; }
static const struct umem_space_ops cpu_ops = {UMEM_SPACE_CPU, fs_map, fs_unmap, fs_flush, 0};
static const struct umem_space_ops dev_ops = {UMEM_SPACE_DEVICE, fs_map, fs_unmap, fs_flush, fs_quiesced};

static void case_maps(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf a;
    struct umem_request r = req(4 * MIB, 1);
    struct fake_space cpu = {0, 0, 0, 0, 0x4000}, npu = {0, 0, 0, 0, 0x100000};
    CHECK(umem_alloc(&ledger, &r, &a) == 0, "alloc", 0, 0);
    uint64_t va;
    CHECK(umem_map(&ledger, &a, &cpu_ops, &cpu, 0, a.bytes, UMEM_PROT_READ, &va) == UMEM_BUSY, "mapping before ready", 0, 0);
    while (umem_prepare_step(&ledger, &a, 0) == 0) {}
    CHECK(umem_map(&ledger, &a, &cpu_ops, &cpu, 0, a.bytes, UMEM_PROT_READ | UMEM_PROT_WRITE, &va) == 0, "cpu map", 0, 0);
    uint64_t dva;
    CHECK(umem_map(&ledger, &a, &dev_ops, &npu, MIB, MIB, UMEM_PROT_READ, &dva) == 0 && dva == 0x100000 + MIB, "device map", dva, 0);
    CHECK(umem_map(&ledger, &a, &dev_ops, &npu, a.bytes, 1, UMEM_PROT_READ, &dva) == UMEM_INVALID, "map outside the buffer", 0, 0);
    CHECK(umem_map(&ledger, &a, &dev_ops, &npu, 0, MIB, UMEM_PROT_WRITE, &dva) == UMEM_INVALID, "write-only map", 0, 0);
    CHECK(umem_free(&ledger, &a) == UMEM_BUSY && cpu.unmaps == 0, "free is refused while a device mapping lives", 0, 0);
    npu.busy = 1;
    CHECK(umem_unmap(&ledger, &a, &dev_ops, &npu, 0x100000 + MIB) == UMEM_BUSY && npu.unmaps == 0, "unmap waits for quiesce", 0, 0);
    npu.busy = 0;
    CHECK(umem_unmap(&ledger, &a, &dev_ops, &npu, 0x100000 + MIB) == 0 && npu.unmaps == 1 && npu.flushes == 1, "unmap", 0, 0);
    CHECK(umem_unmap(&ledger, &a, &dev_ops, &npu, 0x100000 + MIB) == UMEM_NOT_OWNED, "second unmap", 0, 0);
    CHECK(umem_free(&ledger, &a) == 0 && cpu.unmaps == 1 && cpu.flushes == 1, "free unmaps the cpu mapping", cpu.unmaps, 0);
    /* The CPU wrote it through the mapping, so the extent frees dirty and the next owner's buffer is cleaned. */
    struct umem_buf b;
    uint64_t c0 = cleaned_bytes;
    CHECK(alloc_ready(r, &b) == 0 && b.base == a.base && cleaned_bytes - c0 == b.bytes, "dirty after a writable cpu mapping", 0, 0);
    CHECK(umem_check(&ledger) == 0, "check", 0, 0);
}

static void case_raw_lookup(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf a, again;
    struct umem_request r = req(3 * MIB, 4);
    r.flags = UMEM_RAW | UMEM_AT; r.base = 0x80000000ull; r.access = UMEM_PROT_READ | UMEM_PROT_WRITE;
    uint64_t z0 = zeroed_bytes, c0 = cleaned_bytes;
    CHECK(umem_alloc(&ledger, &r, &a) == 0 && umem_ready(&ledger, &a) == 1, "a raw buffer is ready at once", 0, 0);
    CHECK(zeroed_bytes == z0 && cleaned_bytes == c0, "no maintenance for a raw buffer", 0, 0);
    struct umem_extent e[2];
    CHECK(umem_extents_of(&ledger, 4, e, 2) == 1 && e[0].base == a.base && e[0].access == (UMEM_PROT_READ | UMEM_PROT_WRITE), "extents of an owner", 0, 0);
    CHECK(umem_lookup(&ledger, a.base, &again) == 0 && again.cell == a.cell && again.generation == a.generation, "lookup by address", 0, 0);
    CHECK(umem_lookup(&ledger, a.base + UMEM_PAGE, &again) == UMEM_NOT_OWNED && umem_lookup(&ledger, 0x90000000ull, &again) == UMEM_NOT_OWNED,
          "lookup of a non-start or a free address", 0, 0);
    CHECK(umem_free(&ledger, &again) == 0 && umem_describe(&ledger, &a, &e[0], 0) == UMEM_STALE, "free through the looked-up handle", 0, 0);
}

static void case_policy(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_buf nc[3], wb;
    struct umem_request rn = req(UMEM_PAGE, 1);
    rn.policy = UMEM_NC;
    for (unsigned i = 0; i < 3; ++i) CHECK(alloc_ready(rn, &nc[i]) == 0, "nc alloc", i, 0);
    CHECK(converts == 1, "three small NC buffers share one converted chunk", converts, 0);
    CHECK((nc[0].base & ~(UMEM_CHUNK - 1)) == (nc[2].base & ~(UMEM_CHUNK - 1)), "same chunk", nc[0].base, nc[2].base);
    expect_policy(nc[0].base, UMEM_PAGE, UMEM_NC);
    /* No WB buffer ever lands in an NC chunk. */
    struct umem_request rw = req(UMEM_PAGE, 2);
    for (unsigned i = 0; i < 512; ++i) {
        CHECK(alloc_ready(rw, &wb) == 0, "wb alloc", i, 0);
        CHECK(model_policy[(wb.base - LOW) / UMEM_CHUNK] == UMEM_WB, "wb in a wb chunk", wb.base, 0);
    }
    CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), 0);
    /* A larger NC buffer converts several aligned chunks. */
    struct umem_buf big;
    struct umem_request rb = req(5 * MIB, 3);
    rb.policy = UMEM_NC;
    unsigned before = converts;
    CHECK(alloc_ready(rb, &big) == 0 && converts - before == 3, "a 5 MiB NC buffer needs three chunks", converts - before, 0);
    expect_policy(big.base, big.bytes, UMEM_NC);
    /* A failing hook leaves the ledger consistent and the request refused. */
    convert_fail = 1;
    struct umem_buf f;
    struct umem_request rf = req(4 * MIB, 4);
    rf.policy = UMEM_NC;
    uint64_t free_before = umem_free_bytes(&ledger);
    CHECK(umem_alloc(&ledger, &rf, &f) == UMEM_HW, "failed conversion", 0, 0);
    CHECK(umem_free_bytes(&ledger) == free_before && umem_check(&ledger) == 0, "consistent after a failed conversion", umem_check(&ledger), 0);
    convert_fail = 0;
    /* NO_CONVERT refuses instead of converting. */
    rf.flags = UMEM_NO_CONVERT;
    rf.bytes = 64 * MIB;
    CHECK(umem_alloc(&ledger, &rf, &f) == UMEM_NO_SPACE, "no conversion allowed", 0, 0);
    /* Free the NC buffers: the chunks stay NC and a new NC request reuses them with no conversion. */
    CHECK(umem_free(&ledger, &big) == 0, "free", 0, 0);
    before = converts;
    CHECK(alloc_ready(rb, &big) == 0 && converts == before, "freed NC chunks are reused", converts - before, 0);
    /* A pinned NC buffer in an unconverted chunk converts it. */
    struct umem_buf pin;
    struct umem_request rp = req(UMEM_PAGE, 5);
    rp.policy = UMEM_NC; rp.flags = UMEM_AT; rp.base = 0x140000000ull;
    CHECK(alloc_ready(rp, &pin) == 0 && pin.base == rp.base, "pinned NC", 0, 0);
    expect_policy(pin.base, pin.bytes, UMEM_NC);
    CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), 0);
}

/* WB memory runs out while NC chunks are free: the free NC chunks convert back. */
static void case_convert_back(void)
{
    model_reset(UMEM_MIN_CELLS);
    struct umem_range tiny[1] = {{LOW, 8 * UMEM_CHUNK}};
    static uint8_t chunks[16];
    free(cells0);
    cells0 = calloc(UMEM_MIN_CELLS, umem_cell_bytes());
    memset(meta_map, 0, sizeof meta_map);
    memset(model_policy, 0, sizeof model_policy);
    CHECK(umem_init(&ledger, &hooks, cells0, UMEM_MIN_CELLS, chunks, LOW, 16, tiny, 1) == 0, "tiny init", 0, 0);
    struct umem_buf nc, wb;
    struct umem_request rn = req(2 * UMEM_CHUNK, 1);
    rn.policy = UMEM_NC;
    CHECK(alloc_ready(rn, &nc) == 0, "nc", 0, 0);
    CHECK(umem_free(&ledger, &nc) == 0, "free nc", 0, 0);
    struct umem_request rw = req(6 * UMEM_CHUNK, 2);
    CHECK(alloc_ready(rw, &wb) == 0, "wb that needs the converted chunks back", 0, 0);
    expect_policy(wb.base, wb.bytes, UMEM_WB);
    CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), 0);
}

/* ---- randomized sequence against the model ---- */
static uint64_t rng_state = 88172645463325252ull;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

struct live { struct umem_buf b; struct umem_request r; };
static struct live *held;
static unsigned held_count, held_capacity;

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void random_ops(unsigned ops, unsigned target_live, uint64_t max_bytes, unsigned check_every, int measure)
{
    uint64_t worst_alloc = 0, worst_free = 0, sum_free = 0, frees = 0;
    uint64_t plain_count = 0, worst_constrained = 0;
    uint32_t *alloc_steps = measure ? malloc(sizeof(uint32_t) * ops) : 0;
    uint32_t last_generation = 0;
    for (unsigned i = 0; i < ops; ++i) {
        int do_free = held_count && (held_count >= target_live || (rnd() % 100) < 45);
        if (do_free) {
            unsigned k = (unsigned)(rnd() % held_count);
            struct live l = held[k];
            held[k] = held[--held_count];
            uint64_t s0 = ledger.steps;
            CHECK(umem_free(&ledger, &l.b) == 0, "free", k, 0);
            uint64_t d = ledger.steps - s0;
            if (d > worst_free) worst_free = d;
            sum_free += d; ++frees;
            model_give(&l.b);
        } else {
            struct umem_request r = req(UMEM_PAGE * (1 + rnd() % (max_bytes / UMEM_PAGE)), (uint16_t)(1 + rnd() % 5));
            uint64_t pick = rnd() % 100;
            if (pick < 12) r.policy = UMEM_NC;
            if (pick >= 12 && pick < 20) r.device_mask = UMEM_DEV_NPU;
            if (pick >= 20 && pick < 26) r.device_mask = UMEM_DEV_GPU;
            if (pick >= 26 && pick < 32) r.phys_max = 0x100000000ull;
            if (pick >= 32 && pick < 36) r.window = UMEM_PAGE << (rnd() % 6 + 4);
            if (pick >= 36 && pick < 44) r.align = UMEM_PAGE << (rnd() % 9);
            if (r.window && r.bytes > r.window) r.bytes = r.window;
            uint64_t eff_align = r.align > UMEM_PAGE ? r.align : UMEM_PAGE;
            if ((r.device_mask & UMEM_DEV_NPU)) { uint64_t n = r.bytes >= 16 * MIB ? 16 * MIB : r.bytes >= MIB ? MIB : r.bytes >= 64 * 1024 ? 64 * 1024 : UMEM_PAGE; if (n > eff_align) eff_align = n; }
            if ((r.device_mask & UMEM_DEV_GPU) && r.bytes >= 256 * 1024 && eff_align < 256 * 1024) eff_align = 256 * 1024;
            uint64_t limit = r.device_mask ? 0x100000000ull : 0;
            if (limit && (!r.phys_max || limit < r.phys_max)) r.phys_max = limit;
            struct umem_buf b;
            uint64_t s0 = ledger.steps;
            int s = umem_alloc(&ledger, &r, &b);
            uint64_t d = ledger.steps - s0;
            if (s == UMEM_NO_SPACE) continue;
            CHECK(s == 0, "alloc", s, i);
            if (d > worst_alloc) worst_alloc = d;
            int plain = !r.policy && !r.device_mask && !r.phys_max && !r.window && !r.align;
            if (plain) { if (alloc_steps) alloc_steps[plain_count] = (uint32_t)d; ++plain_count; }
            else if (d > worst_constrained) worst_constrained = d;
            if (d > worst_alloc) worst_alloc = d;

            CHECK(b.generation > last_generation || b.generation < last_generation / 2 || last_generation == 0, "generation order", b.generation, last_generation);
            last_generation = b.generation;
            struct umem_request chk = r; chk.align = eff_align;
            model_take(&b, &chk);
            while (umem_prepare_step(&ledger, &b, 0) == 0) {}
            if (held_count == held_capacity) { held_capacity = held_capacity ? held_capacity * 2 : 1024; held = realloc(held, held_capacity * sizeof *held); }
            held[held_count++] = (struct live){b, r};
        }
        if (check_every && i % check_every == 0) CHECK(umem_check(&ledger) == 0, "check", umem_check(&ledger), i);
    }
    CHECK(umem_check(&ledger) == 0, "check after the sequence", umem_check(&ledger), 0);
    if (measure) {
        qsort(alloc_steps, (size_t)plain_count, sizeof(uint32_t), cmp_u32);
        uint64_t p99 = alloc_steps[plain_count * 99 / 100], worst_plain = alloc_steps[plain_count - 1];
        printf("  %u live: unconstrained acquire %" PRIu64 " steps at the 99th percentile, %" PRIu64 " worst; constrained acquire %"
               PRIu64 " worst (a request with an alignment, window, limit or non-default policy visits every free extent of the\n"
               "  classes it can fit); release %" PRIu64 " mean, %" PRIu64 " worst\n", held_count, p99, worst_plain, worst_constrained,
               sum_free / frees, worst_free);
        CHECK(p99 <= 400 && worst_plain <= 800 && worst_free <= 400, "step budget", p99, worst_plain);
        free(alloc_steps);
    }
}

static void drain(void)
{
    while (held_count) {
        struct live l = held[--held_count];
        CHECK(umem_free(&ledger, &l.b) == 0, "drain free", 0, 0);
        model_give(&l.b);
    }
    CHECK(umem_check(&ledger) == 0, "drained check", umem_check(&ledger), 0);
    /* Everything is free except the ledger's own pages. */
    struct umem_extent e[1];
    CHECK(ledger.free_bytes == ledger.usable_bytes - ledger.meta_bytes, "free bytes after a drain", ledger.free_bytes, ledger.meta_bytes);
    (void)e;
}

static void case_random(void)
{
    model_reset(UMEM_MIN_CELLS);
    random_ops(60000, 300, 64 * MIB, 500, 0);
    drain();
    model_reset(UMEM_MIN_CELLS);
    random_ops(80000, 2000, 2 * MIB, 2000, 0);
    drain();
}

static void case_scale(void)
{
    model_reset(UMEM_MIN_CELLS);
    random_ops(400000, 100000, 128 * 1024, 100000, 1);
    CHECK(held_count > 30000, "live buffers at scale", held_count, 0);
    printf("  metadata %" PRIu64 " KiB for %u buffers, %" PRIu64 " pages grown at run time\n", ledger.meta_bytes / 1024, held_count,
           ledger.meta_bytes / UMEM_PAGE);
    drain();
}

int main(void)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"basic", case_basic}, {"handles", case_handles}, {"prepare", case_prepare}, {"handoff", case_handoff},
        {"maps", case_maps}, {"raw-lookup", case_raw_lookup}, {"policy", case_policy}, {"convert-back", case_convert_back},
        {"random", case_random}, {"scale", case_scale},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        printf("%s\n", cases[i].name);
        cases[i].run();
    }
    printf("UMEM-LEDGER PASS\n");
    return 0;
}
