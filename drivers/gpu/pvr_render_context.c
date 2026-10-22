/* SPDX-License-Identifier: MIT
 * Render context fields and initialization follow pvr_queue.c, pvr_context.c,
 * pvr_stream_defs.c and the firmware ABI headers.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_render_context.h"
#include "a733_range.h"
#include <string.h>

static uint64_t get(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void put(unsigned char *p, uint64_t value, unsigned bytes)
{ for (unsigned i = 0; i < bytes; ++i) p[i] = (unsigned char)(value >> (8 * i)); }

int pvr_render_context_build(const struct pvr_render_context_config *c,
    const struct pvr_compute_buffer b[PVR_RENDER_CONTEXT_REGIONS])
{
    if (!a733_span_excl_nonempty(c, sizeof(*c)) ||
        !a733_span_excl_nonempty(b, sizeof(*b) * PVR_RENDER_CONTEXT_REGIONS) ||
        c->bvnc != PVR_FW_A733_BVNC || c->priority > 2 || !c->callstack ||
        (c->callstack & 63) || c->callstack >= (UINT64_C(1) << 40)) return 0;
    static const uint32_t required[] = {680, 160, 32, 32, 0, 32, 0, 32};
    for (unsigned i = 0; i < PVR_RENDER_CONTEXT_REGIONS; ++i) {
        const struct pvr_compute_buffer *x = b + i;
        if (!a733_span_excl_nonempty(x->cpu, x->bytes) ||
            !a733_extents_disjoint_prechecked((uintptr_t)x->cpu, x->bytes, (uintptr_t)c, sizeof(*c)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)x->cpu, x->bytes, (uintptr_t)b, sizeof(*b) * PVR_RENDER_CONTEXT_REGIONS)) return 0;
        if (i == PVR_RENDER_GEOM_RING || i == PVR_RENDER_FRAG_RING) {
            if (x->bytes < 2048 || x->bytes > PVR_CCCB_MAX_BYTES || (x->bytes & (x->bytes - 1))) return 0;
        } else if (x->bytes != required[i]) return 0;
        if ((x->address & 7) || x->address < UINT32_C(0xc0000000) ||
            (uint64_t)x->address + x->bytes > UINT64_C(0xc1000000)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (!a733_extents_disjoint_prechecked((uintptr_t)x->cpu, x->bytes, (uintptr_t)b[j].cpu, b[j].bytes) ||
                !((uint64_t)x->address + x->bytes <= b[j].address ||
                  (uint64_t)b[j].address + b[j].bytes <= x->address)) return 0;
    }
    const unsigned char *vm = b[PVR_RENDER_VM].cpu;
    uint64_t catalogue = get(vm, 8);
    if (!catalogue || (catalogue & 4095) || catalogue >= (UINT64_C(1) << 36) ||
        (catalogue >= 0x1fc00000 && catalogue < 0x1fc04000) || get(vm + 8, 4) != UINT32_MAX) return 0;
    for (unsigned i = 12; i < 32; ++i) if (vm[i]) return 0;
    unsigned char *context = b[PVR_RENDER_CONTEXT].cpu;
    memset(context, 0, PVR_RENDER_CONTEXT_BYTES);
    memset(b[PVR_RENDER_GEOM_STATE].cpu, 0, PVR_RENDER_GEOM_STATE_BYTES);
    memset(b[PVR_RENDER_FRAG_STATE].cpu, 0, PVR_RENDER_FRAG_STATE_BYTES);
    for (unsigned q = 0; q < 2; ++q) {
        unsigned char *p = context + 152 * q;
        put(p, b[q ? PVR_RENDER_FRAG_CONTROL : PVR_RENDER_GEOM_CONTROL].address, 4);
        put(p + 4, b[q ? PVR_RENDER_FRAG_RING : PVR_RENDER_GEOM_RING].address, 4);
        put(p + 24, b[q ? PVR_RENDER_FRAG_STATE : PVR_RENDER_GEOM_STATE].address, 4);
        put(p + 32, c->priority, 4);
        put(p + 60, q ? 3 : 2, 4);
        put(p + 104, 30000, 4);
        put(p + 136, b[PVR_RENDER_VM].address, 4);
        put(p + 140, c->context_id, 4);
        put(p + 144, c->owner_id, 4);
    }
    put((unsigned char *)b[PVR_RENDER_GEOM_STATE].cpu + 8, c->callstack, 8);
    for (unsigned i = 0; i < PVR_RENDER_SWITCH_WORDS; ++i)
        put(context + 304 + 8 * i, c->switch_registers[i], 8);
    return 1;
}
