/* SPDX-License-Identifier: MIT
 * Shader construction and fixed render state follow Mesa pvr_usc.c,
 * pvr_arch_job_context.c, pvr_arch_cmd_buffer.c and pvr_arch_job_common.c.
 * Copyright (c) 2022, 2025 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_render_programs.h"
#include "pvr_pds.h"
#include "pco/pco.h"
#include "pco/pco_data.h"
#include "pco/pco_common.h"
#include "nir/nir_builder.h"
#include "common/device_info/bxm-4-64.h"
#include "pvr_types.h"
#include <stdio.h>
#include <stdlib.h>
#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(a) (a).addr
#define __pvr_make_address(a) PVR_DEV_ADDR(a)
#include "csbgen/rogue/cr.h"
#include "csbgen/rogue/vdm.h"
#include "csbgen/rogue/ppp.h"
#include "csbgen/rogue/pds.h"
#include "csbgen/rogue/pbestate.h"
#include "reference-usc.inc"
#define PACK(dst, name, ...) do { struct ROGUE_##name v = {__VA_ARGS__}; ROGUE_##name##_pack(dst, &v); } while (0)
static unsigned char payload[PVR_RENDER_PROGRAM_BYTES];
static struct pvr_render_program_image image;
static const struct pvr_device_info *dev = &pvr_device_info_36_56_104_183;
static uint32_t *pds(unsigned off) { assert(off < 4096 && !(off & 3)); return (void *)(payload + off); }
static pco_shader *compile(pco_ctx *ctx, nir_shader *nir, pco_data *data)
{
    pco_preprocess_nir(ctx, nir, data); pco_lower_nir(ctx, nir, data);
    pco_postprocess_nir(ctx, nir, data);
    pco_shader *s = pco_trans_nir(ctx, nir, data, NULL);
    ralloc_steal(s, nir); pco_process_ir(ctx, s); pco_encode_ir(ctx, s); return s;
}
static pco_shader *clear(pco_ctx *ctx)
{
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, pco_nir_options(), "sunstone_rgba8_clear");
    pco_data data = {0};
    data.fs.output_formats[FRAG_RESULT_DATA0] = PIPE_FORMAT_R32_UINT;
    data.fs.outputs[FRAG_RESULT_DATA0] = (pco_range){.start=0, .count=1};
    nir_create_variable_with_location(b.shader, nir_var_shader_out, FRAG_RESULT_DATA0, glsl_uint_type());
    nir_store_output(&b, nir_load_preamble(&b,1,32,.base=0), nir_imm_int(&b,0),
        .base=0,.component=0,.src_type=nir_type_invalid|32,.write_mask=1,
        .io_semantics.location=FRAG_RESULT_DATA0,.io_semantics.num_slots=1);
    nir_jump(&b,nir_jump_return); return compile(ctx,b.shader,&data);
}
static pco_shader *eot(pco_ctx *ctx, const uint32_t state[2])
{
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,pco_nir_options(),"sunstone_rgba8_eot");
    nir_intrinsic_instr *emit = nir_emitpix_pco(&b,nir_imm_int(&b,state[0]),nir_imm_int(&b,state[1]));
    nir_intrinsic_set_freep(emit,true); nir_jump(&b,nir_jump_return);
    pco_data data={.fs.uses.olchk_skip=true}; return compile(ctx,b.shader,&data);
}
static void upload(pco_shader *s,unsigned off,unsigned cap)
{
    unsigned n=pco_shader_binary_size(s); assert(n+32<=cap);
    memcpy(payload+4096+off,pco_shader_binary_data(s),n);
    fprintf(stderr,"USC offset=%u bytes=%u temps=%u entry=%u\n",off,n,
        pco_shader_data(s)->common.temps,pco_shader_data(s)->common.entry_offset);
}
static void programs(void)
{
    const pco_precomp_data *nop=(const void *)vs_nop_common_0_common;
    const pco_precomp_data *common[2]={(const void *)cs_store_sr_1024_common_0_common,(const void *)cs_load_sr_256_common_0_common};
    assert(nop->size_dwords*4+32<=128);
    memcpy(payload+4096,nop->binary,nop->size_dwords*4);
    struct pvr_pds_kickusc_program sync={0};
    pvr_pds_setup_doutu(&sync.usc_task_control,0,nop->temps,0,false);
    pvr_pds_kick_usc(&sync,pds(0),0,false,PDS_GENERATE_CODEDATA_SEGMENTS);
    assert((sync.code_size+sync.data_size)*4<=128);
    struct pvr_pds_event_program ev={0};
    pvr_pds_generate_pixel_event(&ev,pds(128),PDS_GENERATE_CODE_SEGMENT,dev);
    assert(ev.code_size*4<=128);
    for(unsigned i=0;i<2;i++) {
        assert(common[i]->size_dwords*4+32<=576);
        memcpy(payload+4096+128+i*576,common[i]->binary,common[i]->size_dwords*4);
    }
    PACK(image.switch_registers,CR_VDM_CONTEXT_STATE_BASE,.addr=PVR_DEV_ADDR(PVR_RENDER_GENERAL_ADDRESS+0xc000));
    PACK(image.switch_registers+2,CR_TA_CONTEXT_STATE_BASE,.addr=PVR_DEV_ADDR(PVR_RENDER_GENERAL_ADDRESS+0xd000));
    for(unsigned bank=0;bank<2;bank++) for(unsigned resume=0;resume<2;resume++) {
        unsigned off=256+bank*256+resume*128;
        uint64_t state=PVR_RENDER_GENERAL_ADDRESS+bank*0x5000;
        struct pvr_pds_shared_storing_program sr={.doutw_control={.dest_store=PDS_UNIFIED_STORE,
            .num_const64=2,.doutw_data={state,state+48},.last_instruction=false}};
        pvr_pds_setup_doutu(&sr.usc_task.usc_task_control,128+resume*576,common[resume]->temps,0,false);
        pvr_pds_generate_shared_storing_program(&sr,pds(off),PDS_GENERATE_DATA_SEGMENT,dev);
        unsigned code=ALIGN_POT(sr.data_size*4,ROGUE_VDMCTRL_PDS_STATE2_PDS_CODE_ADDR_ALIGNMENT);
        uint32_t *end=pvr_pds_generate_shared_storing_program(&sr,pds(off+code),PDS_GENERATE_CODE_SEGMENT,dev);
        assert((unsigned char *)end<=payload+off+128);
        unsigned idx=3+bank*10+resume*3;
        PACK((uint32_t *)(image.switch_registers+idx),VDMCTRL_PDS_STATE0,
            .dm_target=ROGUE_VDMCTRL_DM_TARGET_VDM,
            .usc_target=resume?ROGUE_VDMCTRL_USC_TARGET_ALL:ROGUE_VDMCTRL_USC_TARGET_ANY,
            .usc_unified_size=DIV_ROUND_UP(64,ROGUE_VDMCTRL_PDS_STATE0_USC_UNIFIED_SIZE_UNIT_SIZE),
            .pds_data_size=sr.data_size*4/ROGUE_VDMCTRL_PDS_STATE0_PDS_DATA_SIZE_UNIT_SIZE);
        PACK((uint32_t *)(image.switch_registers+idx)+1,VDMCTRL_PDS_STATE1,
            .pds_data_addr=PVR_DEV_ADDR(off),.sd_type=ROGUE_VDMCTRL_SD_TYPE_PDS,.sd_next_type=ROGUE_VDMCTRL_SD_TYPE_PDS);
        PACK(image.switch_registers+idx+1,VDMCTRL_PDS_STATE2,.pds_code_addr=PVR_DEV_ADDR(off+code));
        unsigned pt=768+bank*256+resume*128, ds,cs;
        uint64_t ptaddr=PVR_RENDER_GENERAL_ADDRESS+0xa000+bank*4096;
        uint32_t temp[64]={0};
        if(!resume) {
            struct pvr_pds_stream_out_terminate_program p={.pds_persistent_temp_size_to_store=32,
                .dev_address_for_storing_persistent_temp=ptaddr};
            pvr_pds_generate_stream_out_terminate_program(&p,temp,PDS_GENERATE_DATA_SEGMENT,dev);
            ds=p.stream_out_terminate_pds_data_size;
            pvr_pds_generate_stream_out_terminate_program(&p,temp+ds,PDS_GENERATE_CODE_SEGMENT,dev);
            cs=p.stream_out_terminate_pds_code_size;
        } else {
            struct pvr_pds_stream_out_init_program p={.num_buffers=1,.pds_buffer_data_size={32},.dev_address_for_buffer_data={ptaddr}};
            pvr_pds_generate_stream_out_init_program(&p,temp,false,PDS_GENERATE_DATA_SEGMENT,dev);
            ds=p.stream_out_init_pds_data_size;
            pvr_pds_generate_stream_out_init_program(&p,temp+ds,false,PDS_GENERATE_CODE_SEGMENT,dev);
            cs=p.stream_out_init_pds_code_size;
        }
        unsigned aligned=ALIGN_POT(ds*4,ROGUE_TA_STATE_STREAM_OUT1_PDS_DATA_SIZE_UNIT_SIZE);
        assert(aligned+cs*4<=128); memcpy(pds(pt),temp,ds*4);memcpy(pds(pt+aligned),temp+ds,cs*4);
        PACK((uint32_t *)(image.switch_registers+idx+2),TA_STATE_STREAM_OUT1,
            .sync=true,.pds_data_size=aligned/ROGUE_TA_STATE_STREAM_OUT1_PDS_DATA_SIZE_UNIT_SIZE);
        PACK((uint32_t *)(image.switch_registers+idx+2)+1,TA_STATE_STREAM_OUT2,.pds_data_addr=PVR_DEV_ADDR(pt));
    }
}
static void fragment(void)
{
    struct pvr_device_runtime_info runtime={.core_count=1};
    pco_ctx *ctx=pco_ctx_create(dev,&runtime,NULL);assert(ctx);
    pco_shader *bg=clear(ctx);upload(bg,1280,512);
    struct pvr_pds_kickusc_program kick={0};
    pvr_pds_setup_doutu(&kick.usc_task_control,1280+pco_shader_data(bg)->common.entry_offset,
        pco_shader_data(bg)->common.temps,0,pco_shader_data(bg)->fs.uses.phase_change);
    uint32_t temp[64]={0};pvr_pds_kick_usc(&kick,temp,0,false,PDS_GENERATE_CODEDATA_SEGMENTS);
    unsigned code=ALIGN_POT(kick.data_size*4,16);
    assert(code+kick.code_size*4<=128);
    memcpy(pds(1280),temp,kick.data_size*4);memcpy(pds(1280+code),temp+kick.data_size,kick.code_size*4);
    struct pvr_pds_pixel_shader_sa_program tex={.num_texture_dma_kicks=1};
    pvr_pds_generate_pixel_shader_sa_code_segment(&tex,pds(1408));assert(tex.code_size*4<=128);
    PACK(&tex.texture_dma_address[0],PDSINST_DOUT_FIELDS_DOUTD_SRC0,.sbase=PVR_DEV_ADDR(PVR_RENDER_GENERAL_ADDRESS+0x10000));
    PACK(&tex.texture_dma_control[0],PDSINST_DOUT_FIELDS_DOUTD_SRC1,.dest=ROGUE_PDSINST_DOUTD_DEST_COMMON_STORE,.bsize=1);
    pvr_pds_generate_pixel_shader_sa_texture_state_data(&tex,pds(1536),dev);assert(tex.data_size*4<=128);
    PACK(image.background,CR_PDS_BGRND0_BASE,.shader_addr=PVR_DEV_ADDR(1280),.texunicode_addr=PVR_DEV_ADDR(1408));
    PACK(image.background+1,CR_PDS_BGRND1_BASE,.texturedata_addr=PVR_DEV_ADDR(1536));
    PACK(image.background+2,CR_PDS_BGRND3_SIZEINFO,
        .usc_sharedsize=DIV_ROUND_UP(1,ROGUE_CR_PDS_BGRND3_SIZEINFO_USC_SHAREDSIZE_UNIT_SIZE),
        .pds_texturestatesize=DIV_ROUND_UP(tex.data_size,ROGUE_CR_PDS_BGRND3_SIZEINFO_PDS_TEXTURESTATESIZE_UNIT_SIZE),
        .pds_tempsize=DIV_ROUND_UP(pco_shader_data(bg)->common.temps,ROGUE_CR_PDS_BGRND3_SIZEINFO_PDS_TEMPSIZE_UNIT_SIZE));
    uint32_t state[2];
    PACK(state,PBESTATE_STATE_WORD0,.address_low=PVR_DEV_ADDR(PVR_RENDER_OUTPUT_ADDRESS));
    PACK(state+1,PBESTATE_STATE_WORD1,.address_high=PVR_DEV_ADDR(PVR_RENDER_OUTPUT_ADDRESS),
        .source_format=ROGUE_PBESTATE_SOURCE_FORMAT_8_PER_CHANNEL,.norm=true,.packmode=ROGUE_PBESTATE_PACKMODE_U8U8U8U8);
    PACK(image.pbe,PBESTATE_REG_WORD0,.tilerelative=true,.memlayout=ROGUE_PBESTATE_MEMLAYOUT_LINEAR,
        .linestride=63/ROGUE_PBESTATE_REG_WORD0_LINESTRIDE_UNIT_SIZE,
        .swiz_chan0=ROGUE_PBESTATE_SWIZ_SOURCE_CHAN0,.swiz_chan1=ROGUE_PBESTATE_SWIZ_SOURCE_CHAN1,
        .swiz_chan2=ROGUE_PBESTATE_SWIZ_SOURCE_CHAN2,.swiz_chan3=ROGUE_PBESTATE_SWIZ_SOURCE_CHAN3);
    PACK(image.pbe+1,PBESTATE_REG_WORD1,.maxclip_x=63,.maxclip_y=63);
    pco_shader *end=eot(ctx,state);upload(end,1792,512);
    struct pvr_pds_event_program ev={0};
    pvr_pds_setup_doutu(&ev.task_control,1792+pco_shader_data(end)->common.entry_offset,pco_shader_data(end)->common.temps,0,false);
    pvr_pds_generate_pixel_event(&ev,pds(1664),PDS_GENERATE_DATA_SEGMENT,dev);assert(ev.data_size*4<=128);
    PACK(&image.event_data,CR_EVENT_PIXEL_PDS_DATA,.addr=PVR_DEV_ADDR(1664));
    PACK(&image.event_info,CR_EVENT_PIXEL_PDS_INFO,
        .const_size=DIV_ROUND_UP(ev.data_size,ROGUE_CR_EVENT_PIXEL_PDS_INFO_CONST_SIZE_UNIT_SIZE),
        .usc_sr_size=DIV_ROUND_UP(2,ROGUE_CR_EVENT_PIXEL_PDS_INFO_USC_SR_SIZE_UNIT_SIZE));
    PACK(&image.pixel_output,CR_USC_PIXEL_OUTPUT_CTRL,.width=ROGUE_CR_PIXEL_WIDTH_2REGISTERS,.partition_mask=4095);
    image.event_code=128;
    ralloc_free(bg);ralloc_free(end);ralloc_free(ctx);
}
int main(int argc,char **argv)
{
    assert(argc==3);glsl_type_singleton_init_or_ref();programs();fragment();
    uint32_t constant=PVR_RENDER_PIXEL;memcpy(payload+8192+0x10000,&constant,4);
    PACK(payload+8192+0xf000,VDMCTRL_STREAM_TERMINATE,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE);
    FILE *p=fopen(argv[1],"wb");assert(p);assert(fwrite(payload,1,sizeof(payload),p)==sizeof(payload));assert(!fclose(p));
    p=fopen(argv[2],"wb");assert(p);assert(fwrite(&image,1,sizeof(image),p)==sizeof(image));assert(!fclose(p));
    glsl_type_singleton_decref();return 0;
}
