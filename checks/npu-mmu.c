/* Walk generated device tables independently of the builder's allocator.
 * This is a format/ownership model, not an NPU hardware emulator. */
#include "a733_npu_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) unsigned char storage[A733_NPU_MMU_MAX_BYTES + 8192];
static unsigned char saved[A733_NPU_MMU_MAX_BYTES];
static struct a733_npu_region regions[A733_NPU_MMU_MAX_REGIONS];
static struct a733_npu_region saved_regions[A733_NPU_MMU_MAX_REGIONS];
static unsigned char *arena = storage + 4096;
static size_t capacity = A733_NPU_MMU_MAX_BYTES;
static uint64_t arena_pa = UINT64_C(0x2740000000);
static struct a733_npu_mmu_image image;

static uint32_t get32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decode a PD register and two table levels; return 0 for translation faults
 * and 2 for write permission faults. Physical addresses are never host pointers. */
static int walk(uint32_t virtual, int write, uint64_t *physical)
{
    static const uint64_t page_sizes[] = {4096, 65536, 1048576, 16777216};
    assert((image.pd_entry & 15u) == 1u);
    uint64_t directory = ((uint64_t)(image.pd_entry & ~15u)) * 256;
    assert(directory == arena_pa);
    uint32_t root = get32(arena + (virtual / 16777216u) * 4u);
    if (!(root & 1)) { assert(root & 2); return 0; }
    unsigned kind = (root / 4) & 3;
    uint64_t page = page_sizes[kind];
    uint64_t table = (directory & UINT64_C(0xFF00000000)) | (root & UINT32_C(0xFFFFFFC0));
    assert(table >= directory && table - directory >= 4096);
    uint64_t byte = table - directory + ((virtual % 16777216u) / page) * 4;
    assert(byte <= image.bytes - 4);
    uint32_t leaf = get32(arena + byte);
    if (!(leaf & 1)) { assert(leaf & 2); return 0; }
    if (write && !(leaf & 4)) return 2;
    assert(leaf & 2);
    uint64_t base = (leaf & UINT32_C(0xFFFFF000)) |
                    ((uint64_t)((leaf / 16) & 255) * UINT64_C(0x100000000));
    assert(base % page == 0);
    *physical = base + virtual % page;
    return 1;
}

static void untouched(const unsigned char *p, size_t bytes, unsigned char value)
{
    for (size_t i = 0; i < bytes; ++i) assert(p[i] == value);
}

static void build(size_t count)
{
    memset(storage, 0xa5, sizeof(storage));
    assert(a733_npu_mmu_build(arena, capacity, arena_pa, regions, count, &image));
    assert(image.bytes <= capacity);
    untouched(storage, 4096, 0xa5);
    untouched(arena + image.bytes, sizeof(storage) - 4096 - image.bytes, 0xa5);
}

static void translated(uint32_t device, int write, uint64_t physical)
{
    uint64_t result = UINT64_MAX;
    assert(walk(device, write, &result) == 1 && result == physical);
}

static void faults(uint32_t device, int write, int reason)
{
    uint64_t result = UINT64_MAX;
    assert(walk(device, write, &result) == reason && result == UINT64_MAX);
}

static void refuses(void *memory, size_t bytes, uint64_t pa,
                     const struct a733_npu_region *input, size_t count,
                     struct a733_npu_mmu_image *output)
{
    memcpy(saved, arena, capacity);
    memcpy(saved_regions, regions, sizeof(regions));
    struct a733_npu_mmu_image before = image;
    assert(!a733_npu_mmu_build(memory, bytes, pa, input, count, output));
    assert(memcmp(saved, arena, capacity) == 0);
    assert(memcmp(saved_regions, regions, sizeof(regions)) == 0);
    assert(memcmp(&before, &image, sizeof(image)) == 0);
}

static void refuse(size_t count)
{
    refuses(arena, capacity, arena_pa, regions, count, &image);
}

int main(void)
{
    /* Noncontiguous physical mappings, permissions, and 16 MiB root crossing. */
    regions[0] = (struct a733_npu_region){UINT64_C(0x212345000), 3 * 4096, 0x0ffff000, A733_NPU_PAGE_4K, 1};
    regions[1] = (struct a733_npu_region){UINT64_C(0xfefffff000), 4096, 0x10100000, A733_NPU_PAGE_4K, 0};
    regions[2] = (struct a733_npu_region){UINT64_C(0xfefffff000), 4096, 0x10200000, A733_NPU_PAGE_4K, 0};
    build(3);
    translated(0x0ffff111, 1, UINT64_C(0x212345111));
    translated(0x10000fff, 0, UINT64_C(0x212346fff));
    translated(0x10001000, 1, UINT64_C(0x212347000));
    translated(0x10100123, 0, UINT64_C(0xfefffff123));
    translated(0x10200123, 0, UINT64_C(0xfefffff123));
    faults(0, 0, 0); faults(0x0fffefff, 0, 0); faults(0x10002000, 1, 0);
    faults(0x10100000, 1, 2); faults(0x20000000, 0, 0);
    assert(image.tables == 2);

    /* Every PA extension value is preserved. Distinct root slots also cover
     * the entire 32-bit device address range, including its last byte. */
    for (unsigned i = 0; i < 256; ++i)
        regions[i] = (struct a733_npu_region){((uint64_t)i << 32) | 0xfffff000u,
            4096, (i << 24) | 0xfff000, A733_NPU_PAGE_4K, i & 1};
    build(256);
    assert(image.tables == 256 && image.bytes == A733_NPU_MMU_MAX_BYTES);
    for (unsigned i = 0; i < 256; ++i) {
        translated((i << 24) | 0xffffff, i & 1, ((uint64_t)i << 32) | 0xffffffffu);
        if (!(i & 1)) faults((i << 24) | 0xfff000, 1, 2);
        faults((i << 24) | 0xffefff, 0, 0);
    }

    /* All supported page sizes and their last byte, with compact table storage. */
    const unsigned shifts[] = {12, 16, 20, 24};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t page = UINT64_C(1) << shifts[i];
        regions[i] = (struct a733_npu_region){UINT64_C(0x1200000000) + i * 16777216u,
            page, (i + 1) * 16777216u, (enum a733_npu_page_size)shifts[i], 1};
    }
    build(4);
    for (unsigned i = 0; i < 4; ++i) {
        translated(regions[i].device, 1, regions[i].physical);
        translated(regions[i].device + (uint32_t)regions[i].bytes - 1, 0,
                   regions[i].physical + regions[i].bytes - 1);
        faults(regions[i].device + (uint32_t)regions[i].bytes, 0, 0);
    }
    assert(image.bytes == 4096 + 16384 + 1024 + 64 + 64);

    /* Refusals must be atomic even if the problem appears in the final region. */
    struct a733_npu_region valid = {0x51000000, 8192, 0x10000000, A733_NPU_PAGE_4K, 1};
    regions[0] = valid;
    build(1);
    regions[1] = (struct a733_npu_region){0x61000000, 4096, 0x10001000, A733_NPU_PAGE_4K, 0};
    refuse(2); /* virtual alias */
    regions[1].device = 0x11000000; regions[1].physical = valid.physical;
    refuse(2); /* physical alias with a writer */
    regions[1].physical = 0x61000000; regions[1].device = 0x10100000;
    regions[1].bytes = 65536; regions[1].page_shift = A733_NPU_PAGE_64K;
    refuse(2); /* mixed page sizes in one root */
    regions[1] = valid; regions[1].device = 0x20000000; regions[1].physical = arena_pa;
    refuse(2); /* table memory exposed to device payload */
    regions[1] = valid; regions[1].device = 0xfffff000; refuse(2);
    regions[1] = valid; regions[1].device = 0x20000000;
    regions[1].physical = UINT64_C(0xfffffff000); refuse(2);
    regions[1].physical = UINT64_C(0x10000000000); refuse(2);
    regions[1] = valid; regions[1].device = 0; refuse(2);
    regions[1] = valid; regions[1].device = 0x20000001; refuse(2);
    regions[1] = valid; regions[1].physical++; refuse(2);
    regions[1] = valid; regions[1].bytes++; refuse(2);
    regions[1] = valid; regions[1].bytes = 0; refuse(2);
    regions[1] = valid; regions[1].writable = 2; refuse(2);
    regions[1] = valid; regions[1].page_shift = (enum a733_npu_page_size)13; refuse(2);
    refuses(arena, 4096, arena_pa, regions, 1, &image); /* table capacity */
    refuses(arena, capacity, UINT64_C(0x27fffff000), regions, 1, &image); /* table high-byte boundary */
    refuses(arena, capacity, UINT64_C(0xfffffff000), regions, 1, &image); /* 40-bit arena overflow */
    refuses(arena + 1, capacity, arena_pa, regions, 1, &image);
    refuses(arena, capacity, arena_pa + 1, regions, 1, &image);
    refuses(arena, capacity - 1, arena_pa, regions, 1, &image);
    refuses(arena, capacity, arena_pa, (const void *)arena, 1, &image);
    refuses(arena, capacity, arena_pa, regions, 1, (void *)arena);
    refuses(arena, capacity, arena_pa, regions, 1, (void *)regions);
    refuses(arena, capacity, arena_pa, NULL, 1, &image);
    refuses(arena, capacity, arena_pa, regions, A733_NPU_MMU_MAX_REGIONS + 1, &image);
    refuses((void *)(UINTPTR_MAX - 4095), capacity, arena_pa, regions, 1, &image);

    build(0); /* Rebuilding an offline image removes every previous mapping. */
    assert(image.bytes == 4096 && image.tables == 0);
    for (unsigned i = 0; i < 256; ++i) faults(i << 24, 0, 0);
    puts("PASS NPU MMU: page walks, permissions, all PA extensions, guards, atomic refusal");
    return 0;
}
