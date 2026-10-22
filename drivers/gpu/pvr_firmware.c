/* SPDX-License-Identifier: MIT
 * Firmware format and loading semantics adapted from pvr_fw_info.h,
 * pvr_fw.c and pvr_fw_util.c, Copyright (c) 2023-2024 Imagination Technologies Ltd.
 * See LICENSE.pvr for the retained MIT terms.
 */
#include "pvr_firmware.h"
#include "a733_range.h"
#include <string.h>

static uint32_t u16(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
static uint32_t u32(const unsigned char *p)
{
    return u16(p) | u16(p + 2) << 16;
}
static uint64_t u64(const unsigned char *p)
{
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static int span(uint32_t offset, uint32_t bytes, uint32_t limit)
{
    return offset <= limit && bytes <= limit - offset;
}

static int decode(const unsigned char *image, size_t bytes, uint64_t expected,
                  struct pvr_fw_plan *p)
{
    if (!a733_span_excl_nonempty(image, bytes) || bytes < 8192 || bytes > PVR_FW_MAX_IMAGE ||
        bytes % 4096 || expected != PVR_FW_A733_BVNC) return 0;
    uint32_t footer = (uint32_t)bytes - 4096;
    const unsigned char *h = image + footer;
    if (u32(h) != 3 || u32(h + 4) != 48 || u32(h + 8) != PVR_FW_LAYOUTS ||
        u32(h + 12) != 24 || u64(h + 16) != expected ||
        u32(h + 24) != 4096 || u32(h + 28) != 1 ||
        u16(h + 32) != 1 || u32(h + 44)) return 0;
    uint32_t info_bytes = u32(h + 40);
    if (info_bytes < 32 || info_bytes > footer - 52 || info_bytes % 8) return 0;
    memset(p, 0, sizeof(*p));
    p->bvnc = expected;
    p->major = u16(h + 32); p->minor = u16(h + 34); p->build = u32(h + 36);
    p->device_info_offset = footer - info_bytes;
    p->device_info_bytes = info_bytes;
    const unsigned char *info = image + p->device_info_offset;
    uint32_t available_words = (info_bytes - 32) / 8;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t count = u64(info + i * 8);
        if (count > available_words) return 0;
        p->device_info_counts[i] = (uint32_t)count;
        available_words -= (uint32_t)count;
    }
    /* The MIPS feature is bit 27 of the first feature-mask word. */
    uint32_t masks = p->device_info_counts[0] + p->device_info_counts[1];
    if (!p->device_info_counts[2]) return 0;
    uint64_t features = u64(info + 32 + masks * 8);
    if (!(features & (UINT64_C(1) << 27)) ||
        features & ((UINT64_C(1) << 25) | (UINT64_C(1) << 37))) return 0;

    unsigned seen = 0;
    for (unsigned i = 0; i < PVR_FW_LAYOUTS; ++i) {
        const unsigned char *r = h + 48 + i * 24;
        uint32_t id = u32(r), type = u32(r + 4);
        if (id < 4 || id > 9 || seen & (1u << (id - 4)) ||
            type != (id <= 6 ? 1u : 2u)) return 0;
        struct pvr_fw_layout v = {id, type, u32(r + 8), u32(r + 12),
                                 u32(r + 16), u32(r + 20)};
        if (!v.allocation_bytes || v.allocation_bytes > v.maximum_bytes ||
            (v.address | v.maximum_bytes | v.allocation_bytes | v.allocation_offset) & 4095 ||
            (uint64_t)v.address + v.maximum_bytes > UINT64_C(0x100000000) ||
            !span(v.allocation_offset, v.allocation_bytes, PVR_FW_MAX_ALLOCATION)) return 0;
        for (unsigned j = 0; j < PVR_FW_LAYOUTS; ++j) {
            if (!(seen & (1u << j))) continue;
            const struct pvr_fw_layout *other = &p->layouts[j];
            if (a733_extents_overlap_prechecked(v.address, v.maximum_bytes, other->address, other->maximum_bytes) ||
                (v.type == other->type && a733_extents_overlap_prechecked(v.allocation_offset, v.allocation_bytes,
                    other->allocation_offset, other->allocation_bytes))) return 0;
        }
        seen |= 1u << (id - 4);
        p->layouts[id - 4] = v;
        uint32_t end = v.allocation_offset + v.allocation_bytes;
        if (end > p->allocation_bytes[type - 1]) p->allocation_bytes[type - 1] = end;
    }

    /* ELF32, little-endian, executable, MIPS. Section tables are not loaded. */
    if (memcmp(image, "\177ELF\1\1\1", 7) || u16(image + 16) != 2 ||
        u16(image + 18) != 8 || u32(image + 20) != 1 ||
        u16(image + 40) != 52 || u16(image + 42) != 32) return 0;
    uint32_t phoff = u32(image + 28), phnum = u16(image + 44);
    p->entry = u32(image + 24);
    if (!phnum || phnum > PVR_FW_MAX_LOADS || phoff < 52 ||
        !span(phoff, phnum * 32, p->device_info_offset)) return 0;
    int entry_found = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const unsigned char *r = image + phoff + i * 32;
        if (u32(r) != 1) continue;
        uint32_t offset = u32(r + 4), address = u32(r + 8);
        uint32_t file_bytes = u32(r + 16), memory_bytes = u32(r + 20);
        uint32_t flags = u32(r + 24), alignment = u32(r + 28);
        if (!memory_bytes || file_bytes > memory_bytes ||
            !span(offset, file_bytes, p->device_info_offset) || flags & ~7u ||
            (uint64_t)address + memory_bytes > UINT64_C(0x100000000) ||
            (alignment && (alignment & (alignment - 1)))) return 0;
        unsigned found = PVR_FW_LAYOUTS;
        for (unsigned j = 0; j < PVR_FW_LAYOUTS; ++j) {
            const struct pvr_fw_layout *v = &p->layouts[j];
            if (address >= v->address &&
                span(address - v->address, memory_bytes, v->allocation_bytes)) {
                found = j;
                break;
            }
        }
        if (found == PVR_FW_LAYOUTS) return 0;
        const struct pvr_fw_layout *v = &p->layouts[found];
        if (!(flags & 4) || (v->type == 1 ? flags != 5 : flags != 6)) return 0;
        struct pvr_fw_load load = {offset, v->type - 1,
            v->allocation_offset + address - v->address, file_bytes, memory_bytes, flags};
        for (unsigned j = 0; j < p->load_count; ++j) {
            const struct pvr_fw_load *other = &p->loads[j];
            if (load.allocation == other->allocation &&
                a733_extents_overlap_prechecked(load.destination, load.memory_bytes, other->destination, other->memory_bytes))
                return 0;
        }
        p->loads[p->load_count++] = load;
        /* MIPS bit 0 selects an instruction mode; the byte address clears it. */
        uint32_t entry = p->entry & ~1u;
        if (v->id == 6 && entry >= address && entry - address < file_bytes && flags & 1)
            entry_found = 1;
    }
    return p->load_count && entry_found;
}

int pvr_fw_requirements(const void *image, size_t bytes, uint64_t expected,
                        struct pvr_fw_plan *out)
{
    struct pvr_fw_plan plan;
    if (!a733_span_excl_nonempty(out, sizeof(*out)) || !a733_span_excl_nonempty(image, bytes) ||
        !a733_extents_disjoint_prechecked((uintptr_t)out, sizeof(*out), (uintptr_t)image, bytes) ||
        !decode(image, bytes, expected, &plan))
        return 0;
    *out = plan;
    return 1;
}

int pvr_fw_stage(const void *image, size_t bytes, uint64_t expected,
                 const struct pvr_fw_buffer buffers[2])
{
    struct pvr_fw_plan plan;
    if (!a733_span_excl_nonempty(buffers, sizeof(*buffers) * 2) || !decode(image, bytes, expected, &plan)) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (buffers[i].bytes != plan.allocation_bytes[i] ||
            !a733_span_excl_nonempty(buffers[i].cpu, buffers[i].bytes) ||
            !a733_extents_disjoint_prechecked((uintptr_t)buffers[i].cpu, buffers[i].bytes, (uintptr_t)image, bytes) ||
            !a733_extents_disjoint_prechecked((uintptr_t)buffers[i].cpu, buffers[i].bytes, (uintptr_t)buffers, sizeof(*buffers) * 2)) return 0;
    }
    if (!a733_extents_disjoint_prechecked((uintptr_t)buffers[0].cpu, buffers[0].bytes, (uintptr_t)buffers[1].cpu, buffers[1].bytes)) return 0;
    for (unsigned i = 0; i < 2; ++i) memset(buffers[i].cpu, 0, buffers[i].bytes);
    for (unsigned i = 0; i < plan.load_count; ++i) {
        const struct pvr_fw_load *load = &plan.loads[i];
        memcpy((unsigned char *)buffers[load->allocation].cpu + load->destination,
               (const unsigned char *)image + load->file_offset, load->file_bytes);
    }
    return 1;
}
