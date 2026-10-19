/* SPDX-License-Identifier: MIT
 * Firmware commands follow pvr_job.c, pvr_queue.c and pvr_rogue_fwif_client.h.
 * Register values follow Mesa pvr_arch_job_render.c and Rogue register XML.
 * Copyright (c) 2022-2025 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_render_job.h"
#include <string.h>
static void put(unsigned char *p, uint64_t v, unsigned n)
{ for(unsigned i=0;i<n;i++) p[i]=(unsigned char)(v>>(i*8)); }
int pvr_render_job_build_frame(const struct pvr_render_program_image *p,
    uint32_t hwrt,uint32_t gufo,uint32_t fufo,uint32_t sequence,struct pvr_render_job_image *out)
{
    if (!sequence || sequence>UINT32_MAX/2u || !p || !out || (uintptr_t)p>UINTPTR_MAX-sizeof(*p) ||
        (uintptr_t)out>UINTPTR_MAX-sizeof(*out) ||
        !((uintptr_t)p+sizeof(*p)<=(uintptr_t)out || (uintptr_t)out+sizeof(*out)<=(uintptr_t)p) ||
        (hwrt&63) || hwrt<0xc0000000u || hwrt>0xc0fffe80u ||
        (gufo&3) || (fufo&3) || gufo<0xc0000000u || fufo<0xc0000000u ||
        gufo>=0xc1000000u || fufo>=0xc1000000u || gufo==fufo ||
        (gufo>=hwrt && gufo<hwrt+384) || (fufo>=hwrt && fufo<hwrt+384)) return 0;
    struct pvr_render_job_image r={0};
    put(r.geometry+4,hwrt,4);put(r.fragment+4,hwrt,4);
    put(r.geometry+16,PVR_RENDER_VDM_ADDRESS,8);
    put(r.geometry+24,PVR_RENDER_BORDER_ADDRESS>>2,8);
    put(r.geometry+44,0x202,4); /* W clamp, fixed-point format one. */
    put(r.geometry+48,1u<<17,4); /* Complete on terminate; single layer stride zero. */
    put(r.geometry+80,11,4); /* FIRST, LAST, SINGLE_CORE. */
    put(r.geometry+84,gufo,4);put(r.geometry+88,sequence-1,4);
    put(r.fragment+16,p->pixel_output,4);
    put(r.fragment+52,0x3f800000,4); /* Default depth one, no depth attachment. */
    put(r.fragment+56,0x300,4); /* Background tag and mask. */
    /* 64x64 has four tiles per axis, avoiding the small-surface exceptions to
     * SKIP_INIT_HDRS. One tile in flight is the source-supported bounded mode. */
    put(r.fragment+68,0x80030000,4);
    put(r.fragment+76,p->event_info,4);put(r.fragment+88,p->event_data,4);
    for(unsigned i=0;i<3;i++) {
        put(r.fragment+168+8*i,p->pbe[i],8);
        put(r.fragment+368+8*i,p->background[i],8);
        put(r.fragment+416+8*i,p->background[i],8);
    }
    put(r.fragment+360,PVR_RENDER_BORDER_ADDRESS>>2,8);
    put(r.fragment+464,(1u<<3)|(1u<<26),4); /* MC1; prevent compute overlap. */
    put(r.geometry_update,gufo,4);put(r.geometry_update+4,sequence,4);
    put(r.fragment_fence,gufo,4);put(r.fragment_fence+4,sequence,4);
    put(r.fragment_update,fufo,4);put(r.fragment_update+4,sequence,4);
    *out=r;return 1;
}
int pvr_render_job_build(const struct pvr_render_program_image *p,
    uint32_t hwrt,uint32_t gufo,uint32_t fufo,struct pvr_render_job_image *out)
{ return pvr_render_job_build_frame(p,hwrt,gufo,fufo,1,out); }
