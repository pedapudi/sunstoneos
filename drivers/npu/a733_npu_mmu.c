/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2017 - 2024 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************/
/* Translation format adapted from the pinned VIPLite 2 MMU implementation.
 * The allocator and offline admission policy are native. See docs/npu-mmu.md. */
#include "a733_npu_mmu.h"
#include "a733_range.h"

#define PA_LIMIT (UINT64_C(1) << 40)
#define VA_LIMIT (UINT64_C(1) << 32)
#define ROOT_BYTES 4096u
#define ROOT_ENTRIES 256u
#define BLOCK_BYTES (UINT64_C(1) << 24)
#define INVALID_ENTRY 2u

static void word(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static unsigned table_bytes(unsigned shift)
{
    unsigned size = (1u << (24u - shift)) * 4u;
    return size < 64u ? 64u : size;
}

int a733_npu_mmu_build(void *arena, size_t capacity, uint64_t physical,
                       const struct a733_npu_region *regions, size_t count,
                       struct a733_npu_mmu_image *result)
{
    unsigned shifts[ROOT_ENTRIES] = {0};
    size_t offsets[ROOT_ENTRIES] = {0};
    size_t used = ROOT_BYTES;
    unsigned tables = 0;
    if (count > A733_NPU_MMU_MAX_REGIONS ||
        !a733_span_incl_nonempty(arena, capacity) || !a733_span_incl_nonempty(result, sizeof(*result)) ||
        (count && !a733_span_incl_nonempty(regions, count * sizeof(*regions))) ||
        ((uintptr_t)arena & 4095u) || (physical & 4095u) ||
        capacity < ROOT_BYTES || capacity > A733_NPU_MMU_MAX_BYTES || (capacity & 63u) ||
        physical >= PA_LIMIT || capacity > PA_LIMIT - physical ||
        (physical >> 32) != ((physical + capacity - 1) >> 32)) return 0;
    size_t metadata_bytes = count * sizeof(*regions);
    if (a733_extents_intersect((uintptr_t)arena, capacity, (uintptr_t)result, sizeof(*result)) ||
        a733_extents_intersect((uintptr_t)arena, capacity, (uintptr_t)regions, metadata_bytes) ||
        a733_extents_intersect((uintptr_t)result, sizeof(*result), (uintptr_t)regions, metadata_bytes)) return 0;

    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_region *r = &regions[i];
        unsigned shift = (unsigned)r->page_shift;
        if (shift != 12 && shift != 16 && shift != 20 && shift != 24) return 0;
        uint64_t page = UINT64_C(1) << shift;
        if (!r->bytes || !r->device || r->writable > 1 ||
            ((r->physical | r->bytes | r->device) & (page - 1)) ||
            r->physical >= PA_LIMIT || r->bytes > PA_LIMIT - r->physical ||
            r->bytes > VA_LIMIT - r->device ||
            a733_extents_intersect(r->physical, r->bytes, physical, capacity)) return 0;
        for (size_t j = 0; j < i; ++j) {
            const struct a733_npu_region *prev = &regions[j];
            if (a733_extents_intersect(r->device, r->bytes, prev->device, prev->bytes) ||
                ((r->writable || prev->writable) &&
                 a733_extents_intersect(r->physical, r->bytes, prev->physical, prev->bytes))) return 0;
        }
        unsigned first = r->device >> 24;
        unsigned last = (unsigned)(((uint64_t)r->device + r->bytes - 1) >> 24);
        for (unsigned root = first; root <= last; ++root) {
            if (shifts[root] && shifts[root] != shift) return 0;
            shifts[root] = shift;
        }
    }
    for (unsigned root = 0; root < ROOT_ENTRIES; ++root) {
        if (!shifts[root]) continue;
        offsets[root] = used;
        used += table_bytes(shifts[root]);
        ++tables;
    }
    if (used > capacity) return 0;

    /* All validation precedes mutation. No input resides in either output. */
    uint8_t *memory = arena;
    for (size_t i = 0; i < used; ++i) memory[i] = 0;
    for (unsigned root = 0; root < ROOT_ENTRIES; ++root) {
        if (!shifts[root]) {
            word(memory + root * 4u, INVALID_ENTRY);
            continue;
        }
        unsigned mode = (shifts[root] - 12u) / 4u;
        word(memory + root * 4u, (uint32_t)(physical + offsets[root]) | (mode << 2) | 1u);
        unsigned entries = 1u << (24u - shifts[root]);
        for (unsigned slot = 0; slot < entries; ++slot)
            word(memory + offsets[root] + slot * 4u, INVALID_ENTRY);
    }
    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_region *r = &regions[i];
        uint64_t page = UINT64_C(1) << r->page_shift;
        for (uint64_t offset = 0; offset < r->bytes; offset += page) {
            uint64_t device = r->device + offset;
            uint64_t pa = r->physical + offset;
            unsigned root = (unsigned)(device >> 24);
            unsigned slot = (unsigned)((device & (BLOCK_BYTES - 1)) >> r->page_shift);
            uint32_t entry = (uint32_t)pa | (uint32_t)((pa >> 32) << 4) |
                             (r->writable << 2) | 3u;
            word(memory + offsets[root] + slot * 4u, entry);
        }
    }
    *result = (struct a733_npu_mmu_image){
        .physical = physical, .bytes = used,
        .pd_entry = (uint32_t)((physical >> 12) << 4) | 1u, .tables = tables
    };
    return 1;
}
