/* SPDX-License-Identifier: MIT */
#include "reference.h"
#include "pvr_render_job.h"
#include "pvr_render_texture.h"
void batch_job_reference(const struct pvr_render_job_image *,unsigned,unsigned);
void batch_job_reference(const struct pvr_render_job_image *job,unsigned width,unsigned height)
{
    const struct rogue_fwif_cmd_frag *frag=(const void *)job->fragment;
    const struct rogue_fwif_cmd_geom *geom=(const void *)job->geometry;
    assert(frag->flags==(ROGUE_FRAG_FLAGS_SINGLE_CORE|ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP|
        ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE|ROGUE_FRAG_FLAGS_DEPTHBUFFER|ROGUE_FRAG_FLAGS_STENCILBUFFER));
    assert(frag->regs.isp_bgobjdepth==0x3f800000 && (frag->regs.isp_bgobjvals&0xff)==0);
    assert(frag->regs.isp_zload_store_base==PVR_RENDER_DEPTH_ADDRESS && frag->regs.isp_stencil_load_store_base==PVR_RENDER_DEPTH_ADDRESS);
    assert(frag->zls_stride==width*height*8 && frag->sls_stride==width*height*8);
    assert(geom->regs.vdm_ctrl_stream_base==PVR_RENDER_BATCH_VDM_ADDRESS);
    assert(offsetof(struct rogue_fwif_cmd_frag,regs.isp_zlsctl)==136);
    assert(offsetof(struct rogue_fwif_cmd_frag,regs.isp_zload_store_base)==144);
    assert(offsetof(struct rogue_fwif_cmd_frag,regs.isp_stencil_load_store_base)==152);
    assert(offsetof(struct rogue_fwif_cmd_frag,flags)==464 && offsetof(struct rogue_fwif_cmd_frag,zls_stride)==468 &&
        offsetof(struct rogue_fwif_cmd_frag,sls_stride)==472);
    assert(offsetof(struct rogue_fwif_cmd_geom,regs.vdm_ctrl_stream_base)==16);
}
