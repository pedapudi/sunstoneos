/* SPDX-License-Identifier: MIT
 * Compute context fields and initialization follow pvr_queue.c, pvr_context.c,
 * pvr_stream_defs.c and the firmware ABI headers.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_compute_context.h"
#include <string.h>

static int span(const void *p, size_t bytes)
{ return p && bytes && bytes <= UINTPTR_MAX - (uintptr_t)p; }
static int separate(const void *a, size_t as, const void *b, size_t bs)
{ return (uintptr_t)a + as <= (uintptr_t)b || (uintptr_t)b + bs <= (uintptr_t)a; }
static uint64_t get(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void put(unsigned char *p, uint64_t value, unsigned bytes)
{ for (unsigned i = 0; i < bytes; ++i) p[i] = (unsigned char)(value >> (8 * i)); }

int pvr_compute_context_build(const struct pvr_compute_config *c,
    const struct pvr_compute_buffer b[PVR_COMPUTE_REGIONS])
{
    if (!span(c, sizeof(*c)) || !span(b, sizeof(*b) * PVR_COMPUTE_REGIONS) ||
        c->bvnc != PVR_FW_A733_BVNC || c->priority > 2) return 0;
    static const uint32_t required[] = {PVR_COMPUTE_CONTEXT_BYTES, PVR_COMPUTE_STATE_BYTES,
        PVR_CCCB_CONTROL_BYTES, 0, PVR_GPU_VM_CONTEXT_BYTES};
    for (unsigned i = 0; i < PVR_COMPUTE_REGIONS; ++i) {
        const struct pvr_compute_buffer *x = &b[i];
        if (!span(x->cpu, x->bytes) ||
            !separate(x->cpu, x->bytes, c, sizeof(*c)) ||
            !separate(x->cpu, x->bytes, b, sizeof(*b) * PVR_COMPUTE_REGIONS)) return 0;
        if (i == PVR_COMPUTE_COMMANDS) {
            if (x->bytes < 256 || x->bytes > PVR_CCCB_MAX_BYTES || (x->bytes & (x->bytes - 1))) return 0;
        } else if (x->bytes != required[i]) return 0;
        unsigned align = i == PVR_COMPUTE_STATE ? 4 : 8;
        if ((x->address & (align - 1)) || x->address < UINT32_C(0xc0000000) ||
            (uint64_t)x->address + x->bytes > UINT64_C(0xc1000000)) return 0;
        for (unsigned j = 0; j < i; ++j) {
            if (!separate(x->cpu, x->bytes, b[j].cpu, b[j].bytes) ||
                !((uint64_t)x->address + x->bytes <= b[j].address ||
                  (uint64_t)b[j].address + b[j].bytes <= x->address)) return 0;
        }
    }
    const unsigned char *vm = b[PVR_COMPUTE_VM].cpu;
    uint64_t catalogue = get(vm, 8);
    if (!catalogue || (catalogue & 4095) || catalogue >= (UINT64_C(1) << 36) ||
        (catalogue >= 0x1fc00000 && catalogue < 0x1fc04000) ||
        get(vm + 8, 4) != UINT32_MAX) return 0;
    for (unsigned i = 12; i < PVR_GPU_VM_CONTEXT_BYTES; ++i) if (vm[i]) return 0;

    unsigned char *context = b[PVR_COMPUTE_CONTEXT].cpu;
    memset(context, 0, PVR_COMPUTE_CONTEXT_BYTES);
    memset(b[PVR_COMPUTE_STATE].cpu, 0, PVR_COMPUTE_STATE_BYTES);
    put(context, b[PVR_COMPUTE_CONTROL].address, 4);
    put(context + 4, b[PVR_COMPUTE_COMMANDS].address, 4);
    put(context + 24, b[PVR_COMPUTE_STATE].address, 4);
    put(context + 32, c->priority, 4);
    put(context + 60, 4, 4); /* Compute data master. */
    put(context + 104, 30000, 4); /* Firmware hardware-recovery limit in ms. */
    put(context + 136, b[PVR_COMPUTE_VM].address, 4);
    put(context + 140, c->context_id, 4);
    put(context + 144, c->owner_id, 4);
    for (unsigned i = 0; i < PVR_COMPUTE_SWITCH_REGISTERS; ++i)
        put(context + 152 + 8 * i, c->switch_registers[i], 8);
    return 1;
}
