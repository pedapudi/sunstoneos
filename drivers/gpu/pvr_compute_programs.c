/*
 * Copyright © 2022, 2025 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "pvr_compute_programs.h"
#include "a733_range.h"
#include "pvr_compute_usc.inc"
_Static_assert(sizeof(usc_store) == PVR_COMPUTE_USC_STORE_BYTES, "USC store size");
_Static_assert(sizeof(usc_load) == PVR_COMPUTE_USC_LOAD_BYTES, "USC load size");

const uint32_t pvr_compute_program_bytes[PVR_PROGRAM_REGIONS] = {
    576, 576, 128, 128, 128, 128, 64, 16448, 16448, 512
};
static int gpu_range(uint64_t a, uint64_t n)
{ return a && n && a < (UINT64_C(1) << 40) && n <= (UINT64_C(1) << 40) - a; }
static int heap(const struct pvr_compute_program_heap *h, int relative)
{
    return gpu_range(h->base, h->bytes) && !(h->base & 4095u) &&
           !(h->bytes & 4095u) && (!relative || h->bytes <= (UINT64_C(1) << 32));
}
static int contains(const struct pvr_compute_program_heap *h,
                    const struct pvr_compute_program_span *s)
{ return a733_extent_within(h->base, h->bytes, s->gpu, s->bytes); }
static void put32(void *p, size_t off, uint32_t v)
{
    unsigned char *d = p;
    for (unsigned i = 0; i < 4; ++i) d[off + i] = (unsigned char)(v >> (8u * i));
}
static void copy_words(void *p, const uint32_t *w, size_t n)
{ for (size_t i = 0; i < n; ++i) put32(p, 4 * i, w[i]); }
static void shared_program(void *p, uint64_t state, uint32_t usc)
{
    /* DOUTW constants low word first: DOUTW writes data word 0 to the even
     * register, which Mesa's store and load programs read as the low half
     * (usclib/common.cl:181, pvr_iface.h:117-122). Mesa's save-restore
     * generator stores the high word first (pvr_pds.c:3664-3667), which
     * disagrees with its own shader; port 3-1 showed the rule on the job's
     * PDS constants (docs/designs/gpu-first-job.md, set 27 post-mortem). */
    put32(p, 0, (uint32_t)state); put32(p, 4, (uint32_t)(state >> 32));
    put32(p, 8, 2);
    put32(p, 16, (uint32_t)(state + 48));
    put32(p, 20, (uint32_t)((state + 48) >> 32)); put32(p, 24, 0x80004002u);
    /* DOUTU uses an ordinary low/high pair; ten temps allocate three blocks. */
    put32(p, 32, usc); put32(p, 36, ((PVR_COMPUTE_USC_TEMPS + 3u) / 4u) << 3);
    /* IF0->IF1 branch, predicated halt, restore IF0, two DOUTWs, DOUTU/end. */
    static const uint32_t code[] = {
        0xc1100001u, 0xdb000000u, 0xc1080001u,
        0xf0020001u, 0xf0060201u, 0xfc000402u
    };
    copy_words((unsigned char *)p + 64, code, 6);
}
static uint64_t pds0(const struct pvr_compute_program_config *c, unsigned i,
                     uint32_t code_offset)
{
    uint64_t off = c->buffers[i].gpu - c->pds.base;
    return (off << 32) | (off + code_offset);
}
int pvr_compute_programs_build(const struct pvr_compute_program_config *c,
                              struct pvr_compute_program_result *out)
{
    if (!a733_span_excl_nonempty(c, sizeof(*c)) || !a733_span_excl_nonempty(out, sizeof(*out)) ||
        (uintptr_t)c % _Alignof(struct pvr_compute_program_config) ||
        (uintptr_t)out % _Alignof(struct pvr_compute_program_result) ||
        a733_extents_overlap_prechecked((uintptr_t)c, sizeof(*c), (uintptr_t)out, sizeof(*out)) ||
        c->bvnc != PVR_FW_A733_BVNC || c->execution_cores != 1 ||
        !c->admission_token || !c->admit ||
        c->excluded_count < 5 || c->excluded_count > PVR_COMPUTE_PROGRAM_EXCLUSIONS ||
        !heap(&c->pds, 1) || !heap(&c->usc, 1) || !heap(&c->general, 0) ||
        a733_extents_overlap_prechecked(c->pds.base, c->pds.bytes, c->usc.base, c->usc.bytes) ||
        a733_extents_overlap_prechecked(c->pds.base, c->pds.bytes, c->general.base, c->general.bytes) ||
        a733_extents_overlap_prechecked(c->usc.base, c->usc.bytes, c->general.base, c->general.bytes)) return 0;
    for (unsigned i = 0; i < c->excluded_count; ++i) {
        const struct pvr_compute_program_span *s = &c->excluded[i];
        if (!a733_span_excl_nonempty(s->cpu, s->bytes) || !gpu_range(s->gpu, s->bytes) ||
            a733_extents_overlap_prechecked((uintptr_t)s->cpu, s->bytes, (uintptr_t)out, sizeof(*out))) return 0;
    }
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS; ++i) {
        const struct pvr_compute_program_span *s = &c->buffers[i];
        const struct pvr_compute_program_heap *h = i < 2 ? &c->usc :
            (i == PVR_PROGRAM_CDM_RESUME ? &c->general : &c->pds);
        if (s->bytes != pvr_compute_program_bytes[i] || !a733_span_excl_nonempty(s->cpu, s->bytes) ||
            !gpu_range(s->gpu, s->bytes) || (s->gpu & 63u) || !contains(h, s) ||
            a733_extents_overlap_prechecked((uintptr_t)s->cpu, s->bytes, (uintptr_t)c, sizeof(*c)) ||
            a733_extents_overlap_prechecked((uintptr_t)s->cpu, s->bytes, (uintptr_t)out, sizeof(*out))) return 0;
        for (unsigned j = 0; j < i; ++j) {
            const struct pvr_compute_program_span *b = &c->buffers[j];
            if (a733_extents_overlap_prechecked((uintptr_t)s->cpu, s->bytes, (uintptr_t)b->cpu, b->bytes) ||
                a733_extents_overlap_prechecked(s->gpu, s->bytes, b->gpu, b->bytes)) return 0;
        }
        for (unsigned j = 0; j < c->excluded_count; ++j) {
            const struct pvr_compute_program_span *b = &c->excluded[j];
            if (a733_extents_overlap_prechecked((uintptr_t)s->cpu, s->bytes, (uintptr_t)b->cpu, b->bytes) ||
                a733_extents_overlap_prechecked(s->gpu, s->bytes, b->gpu, b->bytes)) return 0;
        }
    }
    /* All validation, including the external proof, precedes output mutation. */
    if (c->admit(c->admission_context, c) != 1) return 0;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS; ++i) {
        unsigned char *d = c->buffers[i].cpu;
        for (size_t j = 0; j < c->buffers[i].bytes; ++j) d[j] = 0;
    }
    copy_words(c->buffers[PVR_PROGRAM_USC_STORE].cpu, usc_store, 136);
    copy_words(c->buffers[PVR_PROGRAM_USC_LOAD].cpu, usc_load, 122);
    for (unsigned i = 0; i < 4; ++i)
        shared_program(c->buffers[PVR_PROGRAM_PDS_STORE_A + i].cpu,
                       c->buffers[PVR_PROGRAM_SHARED_A + i / 2].gpu,
                       (uint32_t)(c->buffers[i % 2].gpu - c->usc.base));
    static const uint32_t terminate[] = {
        0xc1100001u, 0xdb000000u, 0xc1080001u, 0xf0000005u, 0xd3000000u
    };
    copy_words((unsigned char *)c->buffers[PVR_PROGRAM_PDS_TERMINATE].cpu + 16,
               terminate, 5);
    out->switch_registers[0] = pds0(c, PVR_PROGRAM_PDS_STORE_A, 64);
    /* Sequential, any USC, unified size four, data size four, fence. */
    out->switch_registers[1] = (1u << 29) | (1u << 27) | (4u << 21) | (4u << 1) | 1u;
    out->switch_registers[2] = pds0(c, PVR_PROGRAM_PDS_TERMINATE, 16);
    /* Terminate frees shared partitions and has one 16-byte data unit. */
    out->switch_registers[3] = (1u << 29) | (1u << 27) | (1u << 20) | (1u << 1) | 1u;
    out->switch_registers[4] = pds0(c, PVR_PROGRAM_PDS_LOAD_A, 64);
    out->switch_registers[5] = pds0(c, PVR_PROGRAM_PDS_STORE_B, 64);
    out->switch_registers[6] = pds0(c, PVR_PROGRAM_PDS_LOAD_B, 64);
    out->cdm_resume_address = c->buffers[PVR_PROGRAM_CDM_RESUME].gpu;
    return 1;
}
