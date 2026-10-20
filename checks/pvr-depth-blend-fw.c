/* SPDX-License-Identifier: MIT */
#include "reference.h"
#include "pvr_render_job.h"
#include "pvr_render_texture.h"
void depth_job_reference(const struct pvr_render_job_image *,unsigned,unsigned);
void depth_job_reference(const struct pvr_render_job_image *job,unsigned width,unsigned height)
{
    const struct rogue_fwif_cmd_frag *frag=(const void *)job->fragment;
    const struct rogue_fwif_ufo *pr=(const void *)job->fragment_pr_update,*final=(const void *)job->fragment_update;
    assert(pr->value==13 && final->value==14 && pr->addr==final->addr);
    assert(frag->flags==(ROGUE_FRAG_FLAGS_SINGLE_CORE|ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP|
        ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE|ROGUE_FRAG_FLAGS_DEPTHBUFFER));
    assert(frag->regs.isp_bgobjdepth==0x3f800000 && frag->regs.isp_zload_store_base==PVR_RENDER_DEPTH_ADDRESS);
    assert(frag->zls_stride==width*height*4);
    assert(offsetof(struct rogue_fwif_cmd_frag,regs.isp_zlsctl)==136);
    assert(offsetof(struct rogue_fwif_cmd_frag,regs.isp_zload_store_base)==144);
    assert(offsetof(struct rogue_fwif_cmd_frag,flags)==464 && offsetof(struct rogue_fwif_cmd_frag,zls_stride)==468);
}
