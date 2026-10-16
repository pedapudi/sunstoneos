/* SPDX-License-Identifier: MIT
 * GPU translation and firmware context semantics adapted from pvr_mmu.c,
 * pvr_vm.c and pvr_rogue_mmu_defs.h. Copyright (c) 2023 Imagination Technologies Ltd.
 * See LICENSE.pvr for retained terms.
 */
#include "pvr_gpu_vm.h"
#include <string.h>

#define VA_LIMIT (UINT64_C(1) << 40)
#define PA_LIMIT (UINT64_C(1) << 36)
#define TABLE_BYTES 4096u

static int range(const void *p, size_t n)
{
    return p && n && n <= UINTPTR_MAX - (uintptr_t)p;
}
static int separate(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a + an <= (uintptr_t)b || (uintptr_t)b + bn <= (uintptr_t)a;
}
static int size_code(uint32_t shift)
{
    switch (shift) {
    case 12: return 0; case 14: return 1; case 16: return 2;
    case 18: return 3; case 20: return 4; case 21: return 5;
    default: return -1;
    }
}
static int plan(const struct pvr_gpu_mapping *maps, size_t count,
                 struct pvr_gpu_vm_requirements *out)
{
    if (count > VA_LIMIT / 4096 || count > SIZE_MAX / sizeof(*maps) ||
        (count && !range(maps, count * sizeof(*maps)))) return 0;
    struct pvr_gpu_vm_requirements r = {0};
    uint64_t end = 0;
    uint32_t last_catalogue = UINT32_MAX, last_region = UINT32_MAX, last_shift = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct pvr_gpu_mapping *m = &maps[i];
        if (size_code(m->page_shift) < 0 || !m->bytes || m->address < end ||
            m->address >= VA_LIMIT || m->bytes > VA_LIMIT - m->address ||
            !m->physical || m->physical >= PA_LIMIT || m->bytes > PA_LIMIT - m->physical ||
            m->flags & ~15u) return 0;
        uint64_t mask = (UINT64_C(1) << m->page_shift) - 1;
        if ((m->address | m->physical | m->bytes) & mask) return 0;
        end = m->address + m->bytes;
        uint32_t first_catalogue = (uint32_t)(m->address >> 30);
        uint32_t final_catalogue = (uint32_t)((end - 1) >> 30);
        uint32_t first_region = (uint32_t)(m->address >> 21);
        uint32_t final_region = (uint32_t)((end - 1) >> 21);
        if (first_region == last_region && last_shift != m->page_shift) return 0;
        r.directory_pages += final_catalogue - first_catalogue + (first_catalogue != last_catalogue);
        r.leaf_tables += final_region - first_region + (first_region != last_region);
        r.mapped_bytes += m->bytes;
        r.mapped_pages += (uint32_t)(m->bytes >> m->page_shift);
        last_catalogue = final_catalogue; last_region = final_region; last_shift = m->page_shift;
    }
    r.table_pages = 1 + r.directory_pages + r.leaf_tables;
    r.table_bytes = (uint64_t)r.table_pages * TABLE_BYTES;
    *out = r;
    return 1;
}

int pvr_gpu_vm_require(const struct pvr_gpu_mapping *maps, size_t count,
                        struct pvr_gpu_vm_requirements *out)
{
    struct pvr_gpu_vm_requirements r;
    if (!range(out, sizeof(*out)) || !plan(maps, count, &r) ||
        (count && !separate(out, sizeof(*out), maps, count * sizeof(*maps)))) return 0;
    *out = r;
    return 1;
}
static int outputs_separate(const struct pvr_gpu_vm_tables *t, void *context,
                             const void *input, size_t bytes)
{
    return range(input, bytes) && separate(t->cpu, t->bytes, input, bytes) &&
        separate(context, PVR_GPU_VM_CONTEXT_BYTES, input, bytes);
}
static void put(unsigned char *p, uint64_t value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static uint64_t flags(uint32_t f)
{
    return ((uint64_t)(f & 7) << 1) | (f & PVR_GPU_VM_FW_PROTECT ? UINT64_C(1) << 62 : 0) | 1;
}

int pvr_gpu_vm_build(const struct pvr_gpu_mapping *maps, size_t count,
                      const struct pvr_gpu_vm_tables *t, void *context, size_t context_bytes)
{
    struct pvr_gpu_vm_requirements r;
    if (!range(t, sizeof(*t)) || !plan(maps, count, &r) ||
        context_bytes != PVR_GPU_VM_CONTEXT_BYTES || !range(context, context_bytes) ||
        t->bytes != r.table_bytes || t->page_count != r.table_pages ||
        !range(t->cpu, t->bytes) || !separate(t->cpu, t->bytes, context, context_bytes) ||
        !outputs_separate(t, context, t, sizeof(*t)) ||
        !outputs_separate(t, context, t->physical_pages, t->page_count * sizeof(uint64_t)) ||
        (count && !outputs_separate(t, context, maps, count * sizeof(*maps)))) return 0;
    for (uint32_t i = 0; i < t->page_count; ++i) {
        uint64_t address = t->physical_pages[i];
        if (!address || address >= PA_LIMIT || address & 4095 ||
            (i && address <= t->physical_pages[i - 1])) return 0;
    }
    /* Sorted backing permits a bounded binary search per data span. */
    for (size_t i = 0; i < count; ++i) {
        uint32_t lo = 0, hi = t->page_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (t->physical_pages[mid] < maps[i].physical) lo = mid + 1;
            else hi = mid;
        }
        if (lo < t->page_count && t->physical_pages[lo] < maps[i].physical + maps[i].bytes) return 0;
    }

    memset(t->cpu, 0, t->bytes);
    uint32_t used = 1, last_catalogue = UINT32_MAX, last_region = UINT32_MAX;
    unsigned char *root = t->cpu, *directory = NULL, *leaf = NULL;
    for (size_t i = 0; i < count; ++i) {
        const struct pvr_gpu_mapping *m = &maps[i];
        uint64_t step = UINT64_C(1) << m->page_shift;
        for (uint64_t off = 0; off < m->bytes; off += step) {
            uint64_t address = m->address + off;
            uint32_t catalogue = (uint32_t)(address >> 30), region = (uint32_t)(address >> 21);
            if (catalogue != last_catalogue) {
                directory = root + (size_t)used * TABLE_BYTES;
                put(root + catalogue * 4, (t->physical_pages[used++] >> 8) | 1, 4);
                last_catalogue = catalogue;
            }
            if (region != last_region) {
                leaf = root + (size_t)used * TABLE_BYTES;
                put(directory + (region & 511) * 8,
                    t->physical_pages[used++] | ((uint64_t)size_code(m->page_shift) << 1) | 1, 8);
                last_region = region;
            }
            uint32_t index = (uint32_t)((address & 0x1fffff) >> m->page_shift);
            put(leaf + index * 8, (m->physical + off) | flags(m->flags), 8);
        }
    }
    memset(context, 0, PVR_GPU_VM_CONTEXT_BYTES);
    put(context, t->physical_pages[0], 8);
    put((unsigned char *)context + 8, UINT32_MAX, 4);
    return 1;
}
