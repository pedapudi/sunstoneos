/* SPDX-License-Identifier: MIT
 * MIPS mapping and boot semantics adapted from pvr_vm_mips.c, pvr_fw_mips.c,
 * pvr_rogue_mips.h, pvr_rogue_cr_defs.h and pvr_device_info.c.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_mips.h"
#include <string.h>

#define HEAP_BASE UINT32_C(0xc0000000)
#define HEAP_BYTES UINT32_C(0x01000000)
#define PAGE PVR_MIPS_PAGE_BYTES

static int range(const void *p, size_t n)
{
    return p && n && n <= UINTPTR_MAX - (uintptr_t)p;
}
static int overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}
static int separate(const void *a, size_t an, const void *b, size_t bn)
{
    return !overlap((uintptr_t)a, an, (uintptr_t)b, bn);
}
static uint64_t read64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void put(unsigned char *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

/* Parameter-bearing feature IDs, in the order defined by the firmware ABI.
 * Boolean feature bits consume no parameter word. Decode the entire known
 * stream so absent earlier features cannot silently shift PHYS_BUS_WIDTH.
 */
static int compatible(const unsigned char *image, const struct pvr_fw_plan *p)
{
    static const unsigned char values[] = {
        1, 3, 9, 10, 11, 12, 13, 14, 20, 21, 23, 24, 25, 26,
        28, 29, 30, 31, 36, 42, 43, 44, 46, 51, 52, 57, 60, 63, 66, 67, 68
    };
    const unsigned char *info = image + p->device_info_offset;
    const uint32_t *count = p->device_info_counts;
    if (count[0] != 1 || count[1] != 1 || !count[2] || count[2] > 2) return 0;
    /* This target requires the 36-bit address-zero workaround (BRN 63553). */
    if (!(read64(info + 32) & (UINT64_C(1) << 9))) return 0;
    const unsigned char *features = info + 48;
    if (count[2] == 2 && read64(features + 8) >> 7) return 0; /* 71 known bits. */
    const unsigned char *params = features + count[2] * 8;
    unsigned index = 0;
    int width_found = 0;
    for (unsigned i = 0; i < sizeof(values); ++i) {
        unsigned bit = values[i];
        if (bit / 64 >= count[2] || !(features[bit / 8] & (1u << (bit % 8)))) continue;
        if (index == count[3]) return 0;
        uint64_t value = read64(params + index++ * 8);
        if (bit == 36) {
            if (value != 36) return 0;
            width_found = 1;
        }
    }
    return width_found && index == count[3];
}

static struct pvr_mips_object mapping(const struct pvr_fw_plan *p,
                                      const struct pvr_mips_buffer *b,
                                      const struct pvr_mips_object *objects, size_t i)
{
    if (i >= 2) return objects[i - 2];
    return (struct pvr_mips_object){b[i].pages, b[i].page_count,
        i ? p->layouts[3].address : HEAP_BASE, PVR_MIPS_UNCACHED};
}

static int valid_page(uint64_t p)
{
    /* Remap 5 captures page zero; remaps 1..3 capture the boot aliases.
     * Exclude the full sensitive 16 KiB range described by the reference.
     */
    return p && !(p & (PAGE - 1)) && p < (UINT64_C(1) << 36) &&
           !(p >= UINT64_C(0x1fc00000) && p < UINT64_C(0x1fc04000));
}

static int outputs_separate(const struct pvr_fw_buffer outputs[4],
                             const void *input, size_t bytes)
{
    if (!range(input, bytes)) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (!separate(outputs[i].cpu, outputs[i].bytes, input, bytes)) return 0;
    return 1;
}

int pvr_mips_prepare(const void *image, size_t bytes,
                     const struct pvr_mips_buffer b[3],
                     const struct pvr_mips_object *objects, size_t count,
                     struct pvr_mips_boot *boot)
{
    struct pvr_fw_plan p;
    if (!range(b, 3 * sizeof(*b)) || count > PVR_MIPS_MAX_OBJECTS ||
        (count && !range(objects, count * sizeof(*objects))) ||
        !range(boot, sizeof(*boot)) ||
        !pvr_fw_requirements(image, bytes, PVR_FW_A733_BVNC, &p) ||
        !compatible(image, &p)) return 0;

    /* These sections have architectural addresses; they are not arbitrary ELF
     * destinations. Main code/data must start at their allocation origins.
     */
    if (p.layouts[0].address != HEAP_BASE || p.layouts[0].allocation_offset ||
        p.layouts[3].allocation_offset || p.layouts[1].address != 0x9fc02000 ||
        p.layouts[2].address != 0xbfc00000 || p.layouts[4].address != 0xbfc01000 ||
        p.layouts[5].address != 0xcf600000 || p.entry != 0xbfc00001) return 0;
    const unsigned special[] = {1, 2, 4, 5};
    for (unsigned i = 0; i < 4; ++i)
        if (p.layouts[special[i]].allocation_bytes != PAGE) return 0;

    struct pvr_fw_buffer out[4] = {b[0].storage, b[1].storage, b[2].storage,
                                   {boot, sizeof(*boot)}};
    for (unsigned i = 0; i < 4; ++i) {
        if (!range(out[i].cpu, out[i].bytes)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (!separate(out[i].cpu, out[i].bytes, out[j].cpu, out[j].bytes)) return 0;
    }
    if (!outputs_separate(out, image, bytes) ||
        !outputs_separate(out, b, 3 * sizeof(*b)) ||
        (count && !outputs_separate(out, objects, count * sizeof(*objects)))) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        size_t size = i == 2 ? PVR_MIPS_TABLE_BYTES : p.allocation_bytes[i];
        if (b[i].storage.bytes != size || b[i].page_count != size / PAGE ||
            !outputs_separate(out, b[i].pages, b[i].page_count * sizeof(uint64_t))) return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!objects[i].page_count || objects[i].page_count > HEAP_BYTES / PAGE ||
            !outputs_separate(out, objects[i].pages,
                              objects[i].page_count * sizeof(uint64_t))) return 0;
    }

    /* Validate virtual ownership before writing any table or firmware byte. */
    uint32_t mapped = 0;
    for (size_t i = 0; i < count + 2; ++i) {
        struct pvr_mips_object m = mapping(&p, b, objects, i);
        uint64_t size = (uint64_t)m.page_count * PAGE;
        if (m.address < HEAP_BASE || m.address & (PAGE - 1) ||
            (uint64_t)m.address + size > (uint64_t)HEAP_BASE + HEAP_BYTES ||
            (m.cache_policy != PVR_MIPS_UNCACHED && m.cache_policy != PVR_MIPS_WRITE_THROUGH))
            return 0;
        mapped += m.page_count;
        if (mapped > HEAP_BYTES / PAGE) return 0;
        for (size_t j = 0; j < i; ++j) {
            struct pvr_mips_object other = mapping(&p, b, objects, j);
            if (overlap(m.address, size, other.address, (uint64_t)other.page_count * PAGE))
                return 0;
        }
    }
    /* Include table backing in physical ownership checks. Allocation size is
     * bounded by 4096 mapped pages plus four table pages. No scratch heap.
     */
    for (size_t i = 0; i < count + 3; ++i) {
        struct pvr_mips_object m = i == count + 2 ?
            (struct pvr_mips_object){b[2].pages, 4, 0, 0} : mapping(&p, b, objects, i);
        for (uint32_t page = 0; page < m.page_count; ++page) {
            uint64_t address = m.pages[page];
            if (!valid_page(address)) return 0;
            for (size_t j = 0; j <= i; ++j) {
                struct pvr_mips_object other = j == count + 2 ?
                    (struct pvr_mips_object){b[2].pages, 4, 0, 0} : mapping(&p, b, objects, j);
                uint32_t end = j == i ? page : other.page_count;
                for (uint32_t k = 0; k < end; ++k)
                    if (address == other.pages[k]) return 0;
            }
        }
    }

    uint64_t boot_code = b[0].pages[p.layouts[2].allocation_offset / PAGE];
    uint64_t exceptions = b[0].pages[p.layouts[1].allocation_offset / PAGE];
    uint64_t boot_data = b[1].pages[p.layouts[4].allocation_offset / PAGE];
    uint64_t stack = b[1].pages[p.layouts[5].allocation_offset / PAGE];
    struct pvr_mips_boot result = {36, PVR_MIPS_WRITES, {
        {0x0810, 4, 0x1cf80}, /* Register window and microMIPS boot ISA. */
        {0x0818, 8, 0x1fc00001}, {0x0820, 8, boot_code | 12},
        {0x0858, 8, 1},          {0x0860, 8, boot_code | 12}, /* BRN 63553. */
        {0x0828, 8, 0x1fc01001}, {0x0830, 8, boot_data | 12},
        {0x0838, 8, 0x1fc02001}, {0x0840, 8, exceptions | 12},
        {0x0b50, 8, 0}, {0x08c8, 4, 0}
    }};
    struct pvr_fw_buffer staging[2] = {b[0].storage, b[1].storage};
    if (!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, staging)) return 0;
    /* All remaining operations are infallible under the ownership contract. */
    unsigned char *table = b[2].storage.cpu;
    memset(table, 0, PVR_MIPS_TABLE_BYTES);
    for (size_t i = 0; i < count + 2; ++i) {
        struct pvr_mips_object m = mapping(&p, b, objects, i);
        uint32_t entry = (m.address - HEAP_BASE) / PAGE;
        for (uint32_t page = 0; page < m.page_count; ++page)
            put(table + 4 * (entry + page), (m.pages[page] >> 6) | (m.cache_policy << 3) | 7, 4);
    }
    unsigned char *config = (unsigned char *)b[1].storage.cpu + p.layouts[4].allocation_offset;
    put(config, stack, 8);
    put(config + 8, 0x01800000, 8); /* A733 GPU register resource. */
    for (unsigned i = 0; i < 4; ++i) put(config + 16 + i * 8, b[2].pages[i], 8);
    put(config + 48, 12, 4);
    put(config + 52, 4, 4);
    put(config + 56, 0, 8);
    *boot = result;
    return 1;
}

int pvr_mips_unmap_stopped(const struct pvr_mips_buffer *table,
    const struct pvr_mips_object *objects,size_t count)
{
    if(!range(table,sizeof(*table)) || !range(table->storage.cpu,PVR_MIPS_TABLE_BYTES) ||
       table->storage.bytes!=PVR_MIPS_TABLE_BYTES || !count || count>PVR_MIPS_MAX_OBJECTS ||
       !range(objects,count*sizeof(*objects)) ||
       !separate(table->storage.cpu,PVR_MIPS_TABLE_BYTES,table,sizeof(*table)) ||
       !separate(table->storage.cpu,PVR_MIPS_TABLE_BYTES,objects,count*sizeof(*objects)))return 0;
    unsigned char *bytes=table->storage.cpu;
    for(size_t i=0;i<count;++i) {
        const struct pvr_mips_object *o=objects+i;
        if(!o->page_count || o->page_count>PVR_MIPS_TABLE_BYTES/4 ||
           !range(o->pages,o->page_count*sizeof(*o->pages)) ||
           !separate(table->storage.cpu,PVR_MIPS_TABLE_BYTES,o->pages,o->page_count*sizeof(*o->pages)) ||
           (o->cache_policy!=PVR_MIPS_UNCACHED && o->cache_policy!=PVR_MIPS_WRITE_THROUGH) ||
           o->address<UINT32_C(0xc0000000) ||
           (o->address&4095) || (uint64_t)o->address+(uint64_t)o->page_count*4096>UINT64_C(0xc1000000))return 0;
        uint32_t first=(o->address-UINT32_C(0xc0000000))/4096;
        for(unsigned p=0;p<o->page_count;++p) {
            uint32_t value=0;for(unsigned b=0;b<4;++b)value|=(uint32_t)bytes[4*(first+p)+b]<<(8*b);
            if(!valid_page(o->pages[p]) || value!=((o->pages[p]>>6)|(o->cache_policy<<3)|7))return 0;
        }
        for(size_t j=0;j<i;++j)if((uint64_t)o->address<(uint64_t)objects[j].address+objects[j].page_count*4096u &&
            (uint64_t)objects[j].address<(uint64_t)o->address+o->page_count*4096u)return 0;
    }
    for(size_t i=0;i<count;++i)memset(bytes+4*((objects[i].address-UINT32_C(0xc0000000))/4096),0,objects[i].page_count*4u);
    return 1;
}
