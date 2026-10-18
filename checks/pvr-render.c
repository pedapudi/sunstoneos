/* SPDX-License-Identifier: MIT */
#include "reference.h"
#include "pvr_render_job.h"
#include <stdio.h>
static unsigned char native[PVR_RENDER_PROGRAM_BYTES], reference[PVR_RENDER_PROGRAM_BYTES];
static void read_file(const char *path,void *out,size_t n)
{FILE *f=fopen(path,"rb");assert(f && fread(out,1,n,f)==n && fgetc(f)==EOF);assert(!fclose(f));}
int main(int argc,char **argv)
{
    assert(argc==3);
    struct pvr_render_program_image image,expected;
    read_file(argv[1],reference,sizeof(reference));read_file(argv[2],&expected,sizeof(expected));
    assert(pvr_render_programs_build(native,sizeof(native),&image));
    assert(!memcmp(&image,&expected,sizeof(image)));
    unsigned off=8192+PVR_RENDER_OUTPUT_OFFSET;
    for(unsigned i=0;i<PVR_RENDER_OUTPUT_BYTES;i+=4) {
        uint32_t v=(i<64 || i>=64+64*64*4)?PVR_RENDER_GUARD:PVR_RENDER_POISON;
        memcpy(reference+off+i,&v,4);
    }
    assert(!memcmp(native,reference,sizeof(native)));
    unsigned char context[680],geom_state[160],frag_state[32],gc[32],gr[2048],fc[32],fr[2048];
    struct rogue_fwif_fwmemcontext vm={.pc_dev_paddr=0x42000000,.page_cat_base_reg_set=ROGUE_FW_BIF_INVALID_PCSET};
    struct pvr_compute_buffer b[]={
        {context,sizeof(context),0xc0a00000},{geom_state,sizeof(geom_state),0xc0a00400},
        {frag_state,sizeof(frag_state),0xc0a004c0},{gc,sizeof(gc),0xc0a00500},
        {gr,sizeof(gr),0xc0a01000},{fc,sizeof(fc),0xc0a00540},{fr,sizeof(fr),0xc0a02000},
        {&vm,sizeof(vm),0xc0900400}};
    struct pvr_render_context_config config={.bvnc=PVR_FW_A733_BVNC,.callstack=PVR_RENDER_CALLSTACK,.priority=1,.context_id=2,.owner_id=1};
    memcpy(config.switch_registers,image.switch_registers,sizeof(config.switch_registers));
    assert(pvr_render_context_build(&config,b));
    struct rogue_fwif_fwrendercontext ctx={0};
    struct rogue_fwif_fwcommoncontext *commons[]={&ctx.geom_context,&ctx.frag_context};
    for(unsigned i=0;i<2;i++) {
        struct rogue_fwif_fwcommoncontext *c=commons[i];
        c->ccbctl_fw_addr=b[i?PVR_RENDER_FRAG_CONTROL:PVR_RENDER_GEOM_CONTROL].address;
        c->ccb_fw_addr=b[i?PVR_RENDER_FRAG_RING:PVR_RENDER_GEOM_RING].address;
        c->context_state_addr=b[i?PVR_RENDER_FRAG_STATE:PVR_RENDER_GEOM_STATE].address;
        c->priority=1;c->dm=i?PVR_FWIF_DM_FRAG:PVR_FWIF_DM_GEOM;c->max_deadline_ms=30000;
        c->fw_mem_context_fw_addr=b[PVR_RENDER_VM].address;c->server_common_context_id=2;c->pid=1;
    }
    memcpy(&ctx.static_render_context_state,&image.switch_registers,sizeof(ctx.static_render_context_state));
    assert(sizeof(ctx)==sizeof(context) && !memcmp(context,&ctx,sizeof(ctx)));
    struct rogue_fwif_geom_ctx_state gs={0};gs.geom_core[0].geom_reg_vdm_call_stack_pointer_init=PVR_RENDER_CALLSTACK;
    assert(sizeof(gs)==sizeof(geom_state) && !memcmp(geom_state,&gs,sizeof(gs)));
    unsigned char before[680];memcpy(before,context,sizeof(before));b[PVR_RENDER_FRAG_RING].address=b[PVR_RENDER_GEOM_RING].address;
    assert(!pvr_render_context_build(&config,b) && !memcmp(context,before,sizeof(before)));
    struct pvr_render_job_image job;
    assert(pvr_render_job_build(&image,0xc0900100,0xc0a00580,0xc0a00584,&job));
    struct rogue_fwif_cmd_geom g={0};struct rogue_fwif_cmd_frag f={0};
    g.cmd_shared.hwrt_data_fw_addr=f.cmd_shared.hwrt_data_fw_addr=0xc0900100;
    g.regs.vdm_ctrl_stream_base=PVR_RENDER_VDM_ADDRESS;g.regs.tpu_border_colour_table=PVR_RENDER_BORDER_ADDRESS>>2;
    g.regs.ppp_ctrl=REF_PPP;g.regs.te_psg=REF_PSG;
    g.flags=ROGUE_GEOM_FLAGS_FIRSTKICK|ROGUE_GEOM_FLAGS_LASTKICK|ROGUE_GEOM_FLAGS_SINGLE_CORE;
    g.partial_render_geom_frag_fence.addr=0xc0a00580;
    f.regs.usc_pixel_output_ctrl=image.pixel_output;f.regs.isp_bgobjdepth=0x3f800000;
    f.regs.isp_bgobjvals=REF_BG;f.regs.isp_ctl=REF_ISP;
    f.regs.event_pixel_pds_info=image.event_info;f.regs.event_pixel_pds_data=image.event_data;
    memcpy(f.regs.pbe_word[0],image.pbe,sizeof(image.pbe));
    memcpy(f.regs.pds_bgnd,image.background,sizeof(image.background));
    memcpy(f.regs.pds_pr_bgnd,image.background,sizeof(image.background));
    f.regs.tpu_border_colour_table=PVR_RENDER_BORDER_ADDRESS>>2;
    f.flags=ROGUE_FRAG_FLAGS_SINGLE_CORE|ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP;
    assert(sizeof(g)==sizeof(job.geometry) && !memcmp(&g,job.geometry,sizeof(g)));
    assert(sizeof(f)==sizeof(job.fragment) && !memcmp(&f,job.fragment,sizeof(f)));
    struct rogue_fwif_ufo gu={.addr=0xc0a00580,.value=1},fu={.addr=0xc0a00584,.value=1};
    assert(!memcmp(&gu,job.geometry_update,8) && !memcmp(&gu,job.fragment_fence,8) && !memcmp(&fu,job.fragment_update,8));
    struct pvr_render_job_image saved=job;
    assert(!pvr_render_job_build(&image,0xc0900100,0xc0900100,0xc0a00584,&job) && !memcmp(&job,&saved,sizeof(job)));
    puts("PASS render reference: actual compiler bytes, source ABI commands/context and alias refusal");return 0;
}
