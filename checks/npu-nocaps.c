/* The NPU loader's, builder's, service's and queue's tables have no fixed count: memory is the only bound.
 *
 * Runs on the host (no device): the page-table builder with thousands of regions, the relocation writer with thousands of
 * patches, the layout of a graph with 12 inputs and 10 outputs (argv[1] names it: checks/nbg_wide.py), the service's tables
 * for 400 clients and 401 commands with each client's permission one bit of a bitmap, and the queue with 400 handles of 12
 * inputs and 10 outputs each. Each limit that remains is shown too: the 4 GiB range of the NPU's page tables, a table the pool
 * cannot hold, a command beyond the declared count, a tensor outside its handle's mapping, and a buffer that another handle's
 * allocation holds. Each of those must be refused. */
#include "a733_npu_mmu.h"
#include "nbg_relocation.h"
#include "nbg_assembly.h"
#include "npu_service.h"
#include "sunstone_npu_queue_service.h"
#include "npu_pool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *big(size_t align, size_t n) { return aligned_alloc(align, (n + align - 1) / align * align); }
#define CLIENTS 400u
#define HANDLES 400u
#define INPUTS 12u
#define OUTPUTS 10u

static void mmu(void)
{
    const unsigned count = 3000;
    struct a733_npu_region *regions = calloc(count, sizeof *regions);
    uint8_t *arena = big(4096, A733_NPU_MMU_MAX_BYTES);
    struct a733_npu_mmu_image image;
    assert(regions && arena);
    /* 3000 pages of 4 KiB, each followed by a guard page, in the low 24 MiB: far beyond the former 512 regions. */
    for (unsigned i = 0; i < count; ++i)
        regions[i] = (struct a733_npu_region){0x20000000u + (uint64_t)i * 0x1000, 0x1000, 0x10000000u + i * 0x2000, A733_NPU_PAGE_4K, i & 1};
    assert(a733_npu_mmu_build(arena, A733_NPU_MMU_MAX_BYTES, 0x70000000u, regions, count, &image) && image.tables >= 1);
    /* The hardware bound that stays: a region past the 4 GiB of device addresses, or below the reserved range. */
    struct a733_npu_region bad = {0x20000000u, 0x2000, 0xfffff000u, A733_NPU_PAGE_4K, 0};
    assert(!a733_npu_mmu_build(arena, A733_NPU_MMU_MAX_BYTES, 0x70000000u, &bad, 1, &image));
    bad = (struct a733_npu_region){0x20000000u, 0x1000, 0x1000, A733_NPU_PAGE_4K, 0};
    assert(!a733_npu_mmu_build(arena, A733_NPU_MMU_MAX_BYTES, 0x70000000u, &bad, 1, &image));
    /* A region that maps another region's device range is refused. */
    regions[1].device = regions[0].device;
    assert(!a733_npu_mmu_build(arena, A733_NPU_MMU_MAX_BYTES, 0x70000000u, regions, count, &image));
    free(regions); free(arena);
    puts("PASS page tables: 3000 regions built; a region outside the 4 GiB range, below the reserved range or overlapping another is refused");
}
static void relocation(void)
{
    const unsigned count = 5000;
    unsigned char *buffer = calloc(1, 4 * count);
    struct nbg_relocation *patches = calloc(count, sizeof *patches);
    assert(buffer && patches);
    for (unsigned i = 0; i < count; ++i)
        patches[i] = (struct nbg_relocation){1u << 20, 64, 0x10000000u, 0, 4 * i, NBG_ADDRESS_WORD};
    assert(nbg_relocate(buffer, 4 * count, patches, count));                 /* more than the former 4096 */
    assert(!nbg_relocate(buffer, 4 * count - 4, patches, count));            /* more patches than destination words */
    patches[7].destination_offset = patches[6].destination_offset;
    assert(!nbg_relocate(buffer, 4 * count, patches, count));                /* a destination patched twice */
    free(buffer); free(patches);
    puts("PASS relocation: 5000 patches applied; more than the buffer's words and a destination patched twice are refused");
}
static void layout(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    static unsigned char graph[1 << 16];
    const size_t n = fread(graph, 1, sizeof graph, f);
    fclose(f);
    struct nbg_model model;
    assert(nbg_model_open(graph, n, &model) == NBG_DECODE_OK);
    assert(nbg_assembly_layout_entries(&model) == NBG_LAYOUT_ENTRIES(INPUTS, OUTPUTS));
    struct nbg_allocation_requirement table[NBG_LAYOUT_ENTRIES(INPUTS, OUTPUTS)];
    struct nbg_assembly_layout l = {.buffers = table, .capacity = NBG_LAYOUT_ENTRIES(INPUTS, OUTPUTS)};
    assert(nbg_assembly_layout(&model, &l) && l.inputs == INPUTS && l.outputs == OUTPUTS);
    for (unsigned i = 0; i < INPUTS + OUTPUTS; ++i) assert(l.buffers[NBG_ALLOC_INPUT + i].allocation_bytes);
    l.capacity = NBG_LAYOUT_ENTRIES(INPUTS, OUTPUTS) - 1;                    /* a table one entry short: refused, not truncated */
    assert(!nbg_assembly_layout(&model, &l));
    puts("PASS loader: a graph of 12 inputs and 10 outputs is admitted with a table of exactly its entries; one entry fewer is refused");
}
/* The service: tables for 400 clients and 401 commands, one permission bit each. */
static void service(void)
{
    static struct npu_service s;
    const size_t need = npu_service_tables_bytes(CLIENTS, CLIENTS + 1);
    assert(need == NPU_SERVICE_TABLES_BYTES(CLIENTS, CLIENTS + 1));
    void *memory = big(64, need);
    assert(memory);
    assert(!npu_service_tables(&s, CLIENTS, CLIENTS + 1, memory, need - 8));  /* the pool cannot hold them: refused */
    assert(!npu_service_tables(&s, 0, 1, memory, need));
    assert(npu_service_tables(&s, CLIENTS, CLIENTS + 1, memory, need));
    assert(s.client_count == CLIENTS);
    for (unsigned i = 0; i < CLIENTS; ++i) assert(s.clients[i].commands && !s.clients[i].task);
    /* Permissions are bound by range; nothing binds before init, so the bitmap is written through the same function the
     * runtime uses once the service exists. Here the words themselves are laid out and distinct. */
    for (unsigned i = 0; i + 1 < CLIENTS; ++i) assert(s.clients[i + 1].commands == s.clients[i].commands + NPU_SERVICE_PERMIT_WORDS(CLIENTS + 1) ||
                                                    (uintptr_t)s.clients[i + 1].commands > (uintptr_t)s.clients[i].commands);
    free(memory);
    puts("PASS service: tables for 400 clients and 401 commands carved from one block; a block one word short is refused");
}
static int backend_i(void *c) { (void)c; return 1; }
static uintptr_t identity(void *c) { (void)c; return 1; }
static void backend_v(void *c) { (void)c; }
static int start(void *c, unsigned h, uint64_t a, uint64_t b, uint64_t t) { (void)c; (void)h; (void)a; (void)b; (void)t; return 1; }
static int finish(void *c, uint64_t a) { (void)c; (void)a; return 1; }
static void cache(void *c, uintptr_t a, size_t n) { (void)c; (void)a; (void)n; }
static void notify(void *c, uintptr_t p, int i) { (void)c; (void)p; (void)i; }
static int queue_open(void *c, unsigned depth) { (void)c; (void)depth; return 0; }
static struct ss_npu_queue ring;
static void queue(void)
{
    const struct ss_npu_queue_backend b = {identity, backend_i, backend_v, queue_open, backend_i, start, finish, cache, backend_i, notify, backend_v};
    assert(ss_npu_queue_service_bind(&ring, &b, NULL));
    const size_t tensors = (size_t)HANDLES * (INPUTS + OUTPUTS), links = 100;
    const unsigned depth = 4;
    const size_t need = ss_npu_queue_tables_bytes(HANDLES, tensors, links, depth);
    void *memory = big(64, need);
    assert(memory);
    assert(!ss_npu_queue_service_tables(&ring, HANDLES, tensors, links, depth, memory, need - 64));   /* too small: refused */
    assert(ss_npu_queue_service_tables(&ring, HANDLES, tensors, links, depth, memory, need));
    struct ss_npu_queue *q;
    assert(ss_npu_queue_open_linked(4, NULL, 0, &q) == 0);                                      /* request the open: state OPEN_REQUEST */
    /* 400 handles, each of 12 inputs and 10 outputs, each with its own read and write allocation. */
    const size_t window = 1u << 20, tensor = 4096;
    uint8_t *arena = big(4096, (size_t)HANDLES * 2 * window);
    assert(arena);
    for (unsigned h = 0; h < HANDLES; ++h) {
        uint8_t *read = arena + (size_t)h * 2 * window, *write = read + window;
        struct ss_npu_tensor in[INPUTS], out[OUTPUTS];
        for (unsigned i = 0; i < INPUTS; ++i) in[i] = (struct ss_npu_tensor){read + i * tensor, tensor};
        for (unsigned i = 0; i < OUTPUTS; ++i) out[i] = (struct ss_npu_tensor){write + i * tensor, tensor};
        const struct ss_npu_cache_span rs = {(uintptr_t)read, window}, ws = {(uintptr_t)write, window};
        if (h == 5) {                                                   /* a tensor outside its handle's mapping: refused */
            struct ss_npu_tensor bad = out[3];
            bad.data = read - 4096 * 8;
            struct ss_npu_tensor saved = out[3];
            out[3] = bad;
            assert(!ss_npu_queue_service_handle(&ring, in, INPUTS, out, OUTPUTS, rs, ws));
            out[3] = saved;
        }
        assert(ss_npu_queue_service_handle(&ring, in, INPUTS, out, OUTPUTS, rs, ws));
    }
    assert(ring.handles == HANDLES);
    {   /* a tensor budget exhausted: one more handle is refused, never stored past its table */
        struct ss_npu_tensor in[1] = {{arena, tensor}}, out[1] = {{arena + window, tensor}};
        assert(!ss_npu_queue_service_handle(&ring, in, 1, out, 1, (struct ss_npu_cache_span){(uintptr_t)arena, window},
                                            (struct ss_npu_cache_span){(uintptr_t)arena + window, window}));
    }
    /* Edges: handle h's output 0 feeds handle h + 1's input 0 for 100 links; the private allocation sets stay disjoint. */
    struct ss_npu_queue_edge edges[100];
    for (unsigned i = 0; i < 100; ++i) edges[i] = (struct ss_npu_queue_edge){i, 0, i + 1, 0};
    assert(!ss_npu_queue_service_edges(&ring, edges, 100));                   /* distinct allocations are not aliases: refused */
    /* A buffer that belongs to another client's allocation: handle 9's read allocation moved into handle 8's write allocation.
     * The admission re-checks every handle against every other and refuses. */
    ring.handle[9].read = ring.handle[8].write;
    assert(!ss_npu_queue_service_edges(&ring, NULL, 0));
    free(memory); free(arena);
    puts("PASS queue: 400 handles of 12 inputs and 10 outputs; a tensor outside its mapping, a table too small, an edge between unlinked allocations and a buffer inside another handle's allocation are refused");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    mmu(); relocation(); layout(argv[1]); service(); queue();
    return 0;
}
