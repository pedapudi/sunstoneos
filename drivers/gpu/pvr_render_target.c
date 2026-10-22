/* SPDX-License-Identifier: MIT
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * Firmware layout follows pvr_hwrt.c; see LICENSE.pvr.
 * Geometry sizing follows Mesa pvr_arch_job_render.c:
 * Copyright © 2022 Imagination Technologies Ltd.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "pvr_render_target.h"
#include "a733_range.h"
#include <string.h>
static uint32_t align(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }
static void put32(unsigned char *p, uint32_t x)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(x >> (8 * i)); }
static void put64(unsigned char *p, uint64_t x)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(x >> (8 * i)); }
int pvr_render_target_require(const struct pvr_render_profile *p, struct pvr_render_requirements *out)
{
    if (!a733_span_excl_nonempty(p, sizeof(*p)) || !a733_span_excl_nonempty(out, sizeof(*out)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)p, sizeof(*p), (uintptr_t)out, sizeof(*out)) ||
        !p->width || !p->height || p->width > 8192 || p->height > 8192 ||
        p->global_maximum_pages < 28 || p->global_maximum_pages > 16384 || (p->global_maximum_pages & 3)) return 0;
    struct pvr_render_requirements r = {0};
    uint32_t x = (p->width + 15) / 16, y = (p->height + 15) / 16;
    r.macro_x = ((x + 7) / 8) * 2; r.macro_y = ((y + 7) / 8) * 2;
    r.tile_max_x = align(x, 2) - 1; r.tile_max_y = align(y, 2) - 1;
    r.region_headers = r.macro_x * r.macro_y * 4;
    uint32_t side = 1, maximum = 4 * (r.macro_x > r.macro_y ? r.macro_x : r.macro_y);
    while (side < maximum) side *= 2;
    r.bytes[PVR_RENDER_VHEAP] = 384 * 4;
    r.bytes[PVR_RENDER_TPC] = side * side * 2;
    uint32_t pt = (p->global_maximum_pages + PVR_RENDER_LOCAL_PAGES + 511) / 512;
    uint32_t pd = (pt + 511) / 512, pc = (pd + 1023) / 1024;
    r.bytes[PVR_RENDER_MLIST0] = r.bytes[PVR_RENDER_MLIST1] = align((pt + pd + pc) * 8, 4096);
    r.bytes[PVR_RENDER_REGION0] = r.bytes[PVR_RENDER_REGION1] = align(r.region_headers * 6, 64);
    for (unsigned i = 0; i < PVR_RENDER_REGIONS; ++i) {
        r.allocation[i] = align(r.bytes[i], 4096); r.payload_bytes += r.allocation[i];
    }
    *out = r; return 1;
}
int pvr_render_target_build(const struct pvr_render_profile *p, const struct pvr_render_target *t)
{
    struct pvr_render_requirements r;
    if (!a733_span_excl_nonempty(t, sizeof(*t)) || !pvr_render_target_require(p, &r)) return 0;
    const void *cpu[] = {t->common, t->data[0], t->data[1]};
    const uint32_t n[] = {88, 384, 384, 64, 64};
    const uint32_t va[] = {t->common_firmware, t->data_firmware[0], t->data_firmware[1],
                          t->local_firmware, t->global_firmware};
    for (unsigned i = 0; i < 5; ++i) {
        if ((va[i] & ((i == 1 || i == 2) ? 63u : 7u)) || va[i] < 0xc0000000u || va[i] > 0xc1000000u - n[i]) return 0;
        for (unsigned j = 0; j < i; ++j) if (va[i] < va[j] + n[j] && va[j] < va[i] + n[i]) return 0;
        if (i < 3) {
            if (!a733_span_excl_nonempty(cpu[i], n[i]) || ((uintptr_t)cpu[i] & (i ? 63 : 7)) ||
                !a733_extents_disjoint_prechecked((uintptr_t)cpu[i], n[i], (uintptr_t)p, sizeof(*p)) ||
                !a733_extents_disjoint_prechecked((uintptr_t)cpu[i], n[i], (uintptr_t)t, sizeof(*t))) return 0;
            for (unsigned j = 0; j < i; ++j) if (!a733_extents_disjoint_prechecked((uintptr_t)cpu[i], n[i], (uintptr_t)cpu[j], n[j])) return 0;
        }
    }
    for (unsigned i = 0; i < PVR_RENDER_REGIONS; ++i) {
        if (!t->gpu[i] || (t->gpu[i] & 4095) || t->gpu[i] > (UINT64_C(1) << 40) - r.allocation[i]) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (t->gpu[i] < t->gpu[j] + r.allocation[j] && t->gpu[j] < t->gpu[i] + r.allocation[i]) return 0;
    }
    unsigned char common[88] = {0}, data[2][384] = {{0}};
    put32(common + 4, (p->width - 1) | (p->height - 1) << 16);
    put64(common + 8, 0x88); put64(common + 16, 0x88);
    put32(common + 28, r.bytes[PVR_RENDER_TPC]);
    put32(common + 32, r.tile_max_x | r.tile_max_y << 12);
    put32(common + 36, r.macro_x * r.macro_y);
    put32(common + 44, r.macro_x << 18); put32(common + 48, r.macro_y << 18);
    /* ISP merge fields are zero for RogueXE with 16-pixel tiles. */
    put32(common + 76, r.region_headers); put32(common + 80, r.macro_x << 16 | r.macro_y);
    for (unsigned i = 0; i < 2; ++i) {
        put64(data[i], t->gpu[PVR_RENDER_MLIST0 + i]); put32(data[i] + 164, t->common_firmware);
        put32(data[i] + 176, t->local_firmware); put32(data[i] + 180, t->global_firmware);
        put64(data[i] + 200, t->gpu[PVR_RENDER_VHEAP]); put32(data[i] + 232, 1);
        put64(data[i] + 240, t->gpu[PVR_RENDER_TPC]);
        put64(data[i] + 256, t->gpu[PVR_RENDER_REGION0 + i]);
    }
    memcpy(t->common, common, sizeof(common));
    for (unsigned i = 0; i < 2; ++i) memcpy(t->data[i], data[i], sizeof(data[i]));
    return 1;
}
