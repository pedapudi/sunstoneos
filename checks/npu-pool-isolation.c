/* The NPU's page tables reach exactly the buffers of its pool, wherever the pool lies.
 *
 * docs/designs/application-memory-layout.md, step 5. The NPU's containment is its MMU: the device sees only the pages
 * that the tables map. This check lays a pool out as the runtime does (drivers/npu/a733_npu_runtime.c, place: tables,
 * the table arena, the bootstrap stream, a CPU-only staging area, then one command, constant, read and write buffer for
 * each slot, every buffer followed by an unmapped guard page in device space), builds the tables with the production
 * memory layer at several pool addresses, and walks them with a walker that shares no code with the builder. For every
 * device page of the 32-bit space it requires that
 *
 *  - the pages reached are exactly the pages of the declared buffers, at the declared device addresses, with write
 *    permission exactly where the buffer's role gives it;
 *  - nothing else of the pool is reached (the table arena, the staging area, the bytes the layout leaves unused), nor
 *    the 2 MiB below and above the pool, nor the neighbouring kernel and application memory that a pool in the slot 1
 *    area borders;
 *  - the guard page after every buffer is not present;
 * and that a buffer that overlaps the table arena is refused. The pool addresses cover the legacy pool, a pool sized from
 * the declaration, pools in the slot 1 area (one at its first block, one at its last) and, as a property of the layer
 * only, one above 4 GiB. */
#include "a733_npu_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096u
#define MIB (UINT64_C(1) << 20)
#define DEVICE_BASE 0x10000000u
#define SLOTS 3u
#define COMMAND (64u * 1024)
#define CONSTANT (128u * 1024)
#define READ (256u * 1024)
#define WRITE (256u * 1024)

struct pool { const char *name; uint64_t base, bytes; };

static uint64_t round_up(uint64_t v, uint64_t m) { return (v + m - 1) / m * m; }

uint8_t *host_base;
static int translate(void *cookie, uintptr_t cpu, int write, struct aarch64_translation *out)
{
    const struct pool *p = cookie;
    (void)write;
    if (cpu < (uintptr_t)host_base || cpu - (uintptr_t)host_base >= p->bytes) return 0;
    *out = (struct aarch64_translation){p->base + (cpu - (uintptr_t)host_base), 0xff, 3};
    return 1;
}
static int cache(void *cookie, enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{ (void)cookie; (void)op; (void)cpu; (void)bytes; return 1; }
static void barrier(void *cookie) { (void)cookie; }

static struct a733_npu_memory memory;

struct page { uint32_t device; uint64_t physical; int writable; };
static struct page reached[4096];
static unsigned reached_count;
static uint64_t forbidden[8][2];
static unsigned forbidden_count;

static uint32_t word(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The walker: the directory has 256 entries of 16 MiB; an entry that is present holds the physical address of a table
 * of 4 KiB leaf entries (mode 0), each present when its low two bits are 3, with physical bits 39..32 in bits 11..4 and
 * the write permission in bit 2. It reads only the arena, at the physical addresses the entries hold. */
static void walk(const uint8_t *arena, uint64_t arena_physical, size_t arena_bytes)
{
    reached_count = 0;
    for (unsigned root = 0; root < 256; ++root) {
        uint32_t entry = word(arena + root * 4);
        if ((entry & 3) != 1) { assert(entry == 2); continue; }
        assert(((entry >> 2) & 3) == 0);                      /* 4 KiB pages */
        uint64_t table = (entry & ~UINT32_C(63)) - (arena_physical & 0xffffffff);
        assert(table >= 4096 && table + 4096 * 4 <= arena_bytes);
        for (unsigned slot = 0; slot < 4096; ++slot) {
            uint32_t leaf = word(arena + table + slot * 4);
            if ((leaf & 3) != 3) { assert(leaf == 2); continue; }
            assert(reached_count < sizeof reached / sizeof *reached);
            reached[reached_count++] = (struct page){root << 24 | slot << 12,
                ((uint64_t)((leaf >> 4) & 0xff) << 32) | (leaf & 0xfffff000u), (leaf >> 2) & 1};
        }
    }
}

static const struct page *find_device(uint32_t device)
{
    for (unsigned i = 0; i < reached_count; ++i) if (reached[i].device == device) return &reached[i];
    return 0;
}

static void forbid(uint64_t base, uint64_t bytes)
{ forbidden[forbidden_count][0] = base; forbidden[forbidden_count++][1] = bytes; }

static int run(const struct pool *pool, int mutate_overlap)
{
    host_base = mmap(0, pool->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(host_base != MAP_FAILED && !((uintptr_t)host_base & 4095));
    /* Device address layout, as place(): the bootstrap first, then the groups; a guard page follows every buffer. */
    struct { uint32_t device; uint64_t offset, bytes; enum a733_npu_buffer_role role; } list[1 + SLOTS * 4];
    unsigned n = 0;
    uint64_t device = DEVICE_BASE;
    /* CPU side: tables, arena, bootstrap, staging, then the groups on 2 MiB boundaries (place()). */
    uint64_t span = (UINT64_C(1) << 24) * (((DEVICE_BASE + 4 * MIB * SLOTS * 8) >> 24) - (DEVICE_BASE >> 24) + 1);
    uint64_t arena_bytes = PAGE + (span >> 24) * 16384u;
    uint64_t offset = 16384;                                  /* the runtime's own tables, never device-visible */
    uint64_t arena_offset = offset; offset += arena_bytes;
    uint64_t bootstrap_offset = offset; offset += COMMAND;
    uint64_t staging_offset = offset; offset += 256 * 1024;   /* the CPU-only staging area */
    list[n++] = (typeof(list[0])){(uint32_t)device, bootstrap_offset, COMMAND, A733_NPU_BUFFER_BOOTSTRAP}; device += COMMAND + PAGE;
    uint64_t command0 = round_up(offset, 2 * MIB);
    uint64_t constant0 = command0 + SLOTS * COMMAND;
    uint64_t read0 = round_up(constant0 + SLOTS * CONSTANT, 2 * MIB);
    uint64_t write0 = round_up(read0 + SLOTS * READ, 2 * MIB);
    uint64_t end = round_up(write0 + SLOTS * WRITE, 2 * MIB);
    assert(end <= pool->bytes);
    for (unsigned s = 0; s < SLOTS; ++s) {
        list[n++] = (typeof(list[0])){(uint32_t)device, command0 + s * COMMAND, COMMAND, A733_NPU_BUFFER_COMMAND}; device += COMMAND + PAGE;
        list[n++] = (typeof(list[0])){(uint32_t)device, constant0 + s * CONSTANT, CONSTANT, A733_NPU_BUFFER_CONSTANT}; device += CONSTANT + PAGE;
        list[n++] = (typeof(list[0])){(uint32_t)device, read0 + s * READ, READ, A733_NPU_BUFFER_READ}; device += READ + PAGE;
        list[n++] = (typeof(list[0])){(uint32_t)device, write0 + s * WRITE, WRITE, A733_NPU_BUFFER_WRITE}; device += WRITE + PAGE;
    }
    assert(device < DEVICE_BASE + span);
    struct a733_npu_buffer buffers[1 + SLOTS * 4];
    for (unsigned i = 0; i < n; ++i)
        buffers[i] = (struct a733_npu_buffer){(uintptr_t)host_base + list[i].offset, pool->base + list[i].offset,
                                              list[i].bytes, list[i].device, list[i].role};
    if (mutate_overlap) {          /* a buffer whose bytes lie in the table arena must be refused */
        buffers[1].cpu = (uintptr_t)host_base + arena_offset; buffers[1].physical = pool->base + arena_offset;
        struct a733_npu_memory_ops ops = {translate, cache, barrier};
        memset(&memory, 0, sizeof memory);
        int accepted = a733_npu_memory_init(&memory, host_base + arena_offset, arena_bytes, pool->base + arena_offset,
                                            buffers, n, &ops, (void *)pool);
        munmap(host_base, pool->bytes);
        return accepted;
    }
    struct a733_npu_memory_ops ops = {translate, cache, barrier};
    memset(&memory, 0, sizeof memory);
    assert(a733_npu_memory_init(&memory, host_base + arena_offset, arena_bytes, pool->base + arena_offset,
                                buffers, n, &ops, (void *)pool));
    walk(host_base + arena_offset, pool->base + arena_offset, arena_bytes);

    /* Exactly the declared pages, at the declared addresses, with the permission of the role. */
    unsigned expected_pages = 0;
    for (unsigned i = 0; i < n; ++i) {
        int writes = list[i].role == A733_NPU_BUFFER_WRITE || list[i].role == A733_NPU_BUFFER_SCRATCH;
        for (uint64_t at = 0; at < list[i].bytes; at += PAGE) {
            const struct page *p = find_device((uint32_t)(list[i].device + at));
            assert(p && p->physical == pool->base + list[i].offset + at && p->writable == writes);
            ++expected_pages;
        }
        /* The guard page after the buffer is not present, and no page stands between two buffers. */
        assert(!find_device((uint32_t)(list[i].device + list[i].bytes)));
    }
    assert(reached_count == expected_pages);                  /* nothing else, anywhere in the 32-bit space */
    assert(!find_device(0) && !find_device(DEVICE_BASE - PAGE) && !find_device(0x400000));
    /* The memory that a page table must never expose: the arena and the runtime's tables, the staging area, the unused
     * bytes of the pool, and the 2 MiB on each side of the pool. */
    forbidden_count = 0;
    forbid(pool->base, 16384);
    forbid(pool->base + arena_offset, arena_bytes);
    forbid(pool->base + staging_offset, 256 * 1024);
    forbid(pool->base + end, pool->bytes - end);
    forbid(pool->base + offset, command0 - offset);
    if (pool->base >= 2 * MIB) forbid(pool->base - 2 * MIB, 2 * MIB);
    forbid(pool->base + pool->bytes, 2 * MIB);
    for (unsigned i = 0; i < reached_count; ++i)
        for (unsigned f = 0; f < forbidden_count; ++f)
            assert(reached[i].physical + PAGE <= forbidden[f][0] || reached[i].physical >= forbidden[f][0] + forbidden[f][1] ||
                   !forbidden[f][1]);
    for (unsigned i = 0; i < reached_count; ++i)
        assert(reached[i].physical >= pool->base && reached[i].physical + PAGE <= pool->base + pool->bytes);
    munmap(host_base, pool->bytes);
    return (int)expected_pages;
}

int main(void)
{
    const struct pool pools[] = {
        {"legacy pool", 0x47400000u, 8 * MIB},
        {"pool sized from the declaration", 0x45a00000u, 22 * MIB},
        {"slot 1 area, first block, 96 MiB", 0x48000000u, 96 * MIB},
        {"slot 1 area, last blocks", 0x78000000u, 16 * MIB},
        {"above 4 GiB (the layer only; the board has not run it)", UINT64_C(0x180000000), 96 * MIB},
    };
    for (unsigned i = 0; i < sizeof pools / sizeof *pools; ++i) {
        int pages = run(&pools[i], 0);
        assert(pages > 0);
        assert(run(&pools[i], 1) == 0);
        printf("PASS %s: %d device pages, all of them declared buffers; overlap with the tables refused\n", pools[i].name, pages);
    }
    puts("PASS NPU pool isolation: the tables reach only the buffers, at every pool address");
    return 0;
}
