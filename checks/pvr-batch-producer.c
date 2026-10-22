/* SPDX-License-Identifier: MIT
 * Graphics shader construction follows the pinned Mesa PCO pipeline.
 * Copyright (c) 2022, 2025 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_render_texture.h"
#undef PVR_RENDER_OUTPUT_ADDRESS
#define PVR_RENDER_OUTPUT_ADDRESS PVR_RENDER_TEXTURE_OUTPUT_ADDRESS
#include "texture-base.inc"
#include "vk_pipeline_layout.h"
#include "texture-stage.inc"
#include "pvr_descriptor_set.h"
#include "csbgen/rogue/texstate.h"
#include "vk_blend.h"
#include "vk_graphics_state.h"
#include "texture-descriptors.inc"

/* Profile-4 resident programs: one perspective-correct vertex shader, one
 * opaque fragment shader and one fragment shader per fixed blend equation.
 * Every fragment shader shares the coefficient and descriptor programs. */
static const struct { unsigned usc,cap,pds; } placement[PVR_RENDER_BATCH_BLENDS]={
    {PVR_RENDER_BATCH_OPAQUE_USC,512,PVR_RENDER_BATCH_OPAQUE_PDS},
    {PVR_RENDER_BATCH_BLEND_USC0,PVR_RENDER_BATCH_BLEND_CAP0,PVR_RENDER_BATCH_BLEND_PDS0},
    {PVR_RENDER_BATCH_BLEND_USC1,PVR_RENDER_BATCH_BLEND_CAP1,PVR_RENDER_BATCH_BLEND_PDS1},
    {PVR_RENDER_BATCH_BLEND_USC2,PVR_RENDER_BATCH_BLEND_CAP2,PVR_RENDER_BATCH_BLEND_PDS2},
    {PVR_RENDER_BATCH_BLEND_USC3,PVR_RENDER_BATCH_BLEND_CAP3,PVR_RENDER_BATCH_BLEND_PDS3},
};
/* Vulkan equations for the admitted blend selectors 1 through 4. */
static const struct vk_color_blend_attachment_state equations[PVR_RENDER_BATCH_BLENDS]={
    {0},
    {.blend_enable=true,.color_blend_op=VK_BLEND_OP_ADD,.src_color_blend_factor=VK_BLEND_FACTOR_SRC_ALPHA,
     .dst_color_blend_factor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.alpha_blend_op=VK_BLEND_OP_ADD,
     .src_alpha_blend_factor=VK_BLEND_FACTOR_ONE,.dst_alpha_blend_factor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.write_mask=15},
    {.blend_enable=true,.color_blend_op=VK_BLEND_OP_ADD,.src_color_blend_factor=VK_BLEND_FACTOR_ONE,
     .dst_color_blend_factor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.alpha_blend_op=VK_BLEND_OP_ADD,
     .src_alpha_blend_factor=VK_BLEND_FACTOR_ONE,.dst_alpha_blend_factor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.write_mask=15},
    {.blend_enable=true,.color_blend_op=VK_BLEND_OP_ADD,.src_color_blend_factor=VK_BLEND_FACTOR_ONE,
     .dst_color_blend_factor=VK_BLEND_FACTOR_ONE,.alpha_blend_op=VK_BLEND_OP_ADD,
     .src_alpha_blend_factor=VK_BLEND_FACTOR_ONE,.dst_alpha_blend_factor=VK_BLEND_FACTOR_ONE,.write_mask=15},
    {.blend_enable=true,.color_blend_op=VK_BLEND_OP_ADD,.src_color_blend_factor=VK_BLEND_FACTOR_DST_COLOR,
     .dst_color_blend_factor=VK_BLEND_FACTOR_ZERO,.alpha_blend_op=VK_BLEND_OP_ADD,
     .src_alpha_blend_factor=VK_BLEND_FACTOR_DST_ALPHA,.dst_alpha_blend_factor=VK_BLEND_FACTOR_ZERO,.write_mask=15},
};
static unsigned variant;

static void batch_constants(const struct pvr_pds_info *info,uint32_t *data,unsigned vertex)
{
    const unsigned char *cursor=(const void *)info->entries;
    for(unsigned i=0;i<info->entry_count;++i) {
        const struct pvr_const_map_entry *e=(const void *)cursor;
        assert(e->const_offset<info->data_size_in_dwords);
        unsigned bytes=0;uint64_t value=0;unsigned width=1;
        switch(e->type) {
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_LITERAL32: {
            const struct pvr_const_map_entry_literal32 *p=(const void *)cursor;
            value=p->literal_value;bytes=sizeof(*p);break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_DOUTU_ADDRESS: {
            const struct pvr_const_map_entry_doutu_address *p=(const void *)cursor;
            assert(vertex);value=p->doutu_control;
            pvr_set_usc_execution_address64(&value,PVR_RENDER_USC_ADDRESS+DRAW_VERTEX_USC);
            bytes=sizeof(*p);width=2;break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_VERTEX_ATTRIBUTE_ADDRESS: {
            const struct pvr_const_map_entry_vertex_attribute_address *p=(const void *)cursor;
            assert(vertex && p->binding_index==0 && p->stride==24 && p->size_in_dwords==(p->offset?2:4) && (p->offset==0 || p->offset==16));
            value=PVR_RENDER_TEXTURE_VERTEX_ADDRESS+p->offset;bytes=sizeof(*p);width=2;break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_VERTEX_ATTRIBUTE_STRIDE: {
            const struct pvr_const_map_entry_vertex_attribute_stride *p=(const void *)cursor;
            assert(vertex && p->binding_index==0);value=24;bytes=sizeof(*p);break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_DESCRIPTOR_SET: {
            const struct pvr_const_map_entry_descriptor_set *p=(const void *)cursor;
            assert(!vertex && p->descriptor_set==0);value=PVR_RENDER_TEXTURE_DESCRIPTOR_ADDRESS;
            bytes=sizeof(*p);width=2;break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_SPECIAL_BUFFER: {
            const struct pvr_const_map_entry_special_buffer *p=(const void *)cursor;
            assert(!vertex && p->buffer_type==PVR_BUFFER_TYPE_FS_META && p->size_in_dwords==1);
            value=DRAW_META_ADDRESS;bytes=sizeof(*p);width=2;break;
        }
        default:fprintf(stderr,"Unsupported batch constant type=%u offset=%u vertex=%u\n",e->type,e->const_offset,vertex);abort();
        }
        assert(e->const_offset+width<=info->data_size_in_dwords && !(e->const_offset%width));
        memcpy(data+e->const_offset,&value,width*4);cursor+=bytes;
    }
    assert(cursor==(const unsigned char *)info->entries+info->entries_written_size_in_bytes);
}

/* Restricted pvr_arch_tex_state.c/pvr_arch_sampler.c path at the default
 * frame state: RGBA8, one linear layer/mip/sample, nearest clamp-to-edge. */
static void batch_descriptor(uint32_t width,uint32_t height,uint32_t stride)
{
    struct pvr_combined_image_sampler_descriptor d={0};
    PACK(d.image.words,TEXSTATE_IMAGE_WORD0,.textype=ROGUE_TEXSTATE_TEXTYPE_STRIDE,
        .texformat=ROGUE_TEXSTATE_FORMAT_U8U8U8U8,.width=width-1,.height=height-1,
        .swiz0=ROGUE_TEXSTATE_SWIZ_SRCCHAN_0,.swiz1=ROGUE_TEXSTATE_SWIZ_SRCCHAN_1,
        .swiz2=ROGUE_TEXSTATE_SWIZ_SRCCHAN_2,.swiz3=ROGUE_TEXSTATE_SWIZ_SRCCHAN_3);
    PACK(d.image.words+1,TEXSTATE_STRIDE_IMAGE_WORD1,.num_mip_levels=1,
        .stride=stride-1,.texaddr=PVR_DEV_ADDR(PVR_RENDER_TEXTURE_IMAGE_ADDRESS),.alpha_msb=true);
    bool scale=false,roundzero=false,split=false;
    uint32_t pck=pco_pipe_to_pck_format(PIPE_FORMAT_R8G8B8A8_UNORM,&scale,&roundzero,&split);
    assert(pck!=~0u);
    d.image.meta[PCO_IMAGE_META_PCK_INFO]=pck | (scale?PVR_PCK_INFO_SCALE_BIT:0) |
        (split?PVR_PCK_INFO_SPLIT_BIT:0) | (roundzero?PVR_PCK_INFO_ROUNDZERO_BIT:0) | PVR_PCK_INFO_UNORM_BIT;
    d.image.meta[PCO_IMAGE_META_LAYER_SIZE]=stride*height*4;
    float rounding=PVR_HAS_QUIRK(dev,51025)?0.5f:0.0f;
    unsigned lod=util_unsigned_fixed(rounding,ROGUE_TEXSTATE_CLAMP_FRACTIONAL_BITS);
    PACK(d.sampler.words,TEXSTATE_SAMPLER_WORD0,.magfilter=ROGUE_TEXSTATE_FILTER_POINT,
        .minfilter=ROGUE_TEXSTATE_FILTER_POINT,.addrmode_u=ROGUE_TEXSTATE_ADDRMODE_CLAMP_TO_EDGE,
        .addrmode_v=ROGUE_TEXSTATE_ADDRMODE_CLAMP_TO_EDGE,.addrmode_w=ROGUE_TEXSTATE_ADDRMODE_CLAMP_TO_EDGE,
        .dadjust=ROGUE_TEXSTATE_DADJUST_ZERO_UINT,.minlod=lod,.maxlod=lod);
    struct ROGUE_TEXSTATE_SAMPLER_WORD0 gather;
    ROGUE_TEXSTATE_SAMPLER_WORD0_unpack(d.sampler.words,&gather);
    gather.minfilter=ROGUE_TEXSTATE_FILTER_LINEAR;gather.magfilter=ROGUE_TEXSTATE_FILTER_LINEAR;
    ROGUE_TEXSTATE_SAMPLER_WORD0_pack(d.sampler.gather_words,&gather);
    assert(sizeof(d)==80);memcpy(payload+8192+0x10100,&d,sizeof(d));
    /* Mesa's sRGB view of the same image differs only in the gamma field and
     * its pack information. Record setup_pck_info's value for that view. */
    scale=roundzero=split=false;
    uint32_t srgb=pco_pipe_to_pck_format(PIPE_FORMAT_R8G8B8A8_SRGB,&scale,&roundzero,&split);
    if(srgb==~0u)srgb=PVR_PCK_FORMAT_INVALID;
    srgb|=(split?PVR_PCK_INFO_SPLIT_BIT:0) | (scale?PVR_PCK_INFO_SCALE_BIT:0) | (roundzero?PVR_PCK_INFO_ROUNDZERO_BIT:0) |
        (util_format_is_unorm(PIPE_FORMAT_R8G8B8A8_SRGB)?PVR_PCK_INFO_UNORM_BIT:0);
    fprintf(stderr,"BATCH_TEXTURE_STATE bytes=%zu lod=%u pck=%u srgb_pck=%u\n",sizeof(d),lod,d.image.meta[3],srgb);
}

static unsigned coefficient_data,coefficient_code,coefficient_temps,uniform_data,uniform_code;
static void batch_control(const pco_data *vs,const pco_data *fs)
{
    uint32_t *ppp=(void *)(payload+8192+0xf100),*p=ppp;
#define EMIT(name, ...) do { PACK(p,name,__VA_ARGS__);p+=ROGUE_##name##_length; } while(0)
    EMIT(TA_STATE_HEADER,.pres_stream_out_size=true,.pres_ppp_ctrl=true,.pres_varying_word2=true,
        .pres_varying_word1=true,.pres_varying_word0=true,.pres_outselects=true,.pres_wclamp=true,
        .pres_viewport=true,.pres_region_clip=true,.pres_pds_state_ptr0=true,.pres_pds_state_ptr1=true,
        .pres_pds_state_ptr3=true,.pres_ispctl_fb=true,.pres_ispctl_fa=true,.pres_ispctl=true,.pres_ispctl_dbsc=true);
    EMIT(TA_STATE_ISPCTL,.scenable=true,.bpres=true);
    EMIT(TA_STATE_ISPA,.pointlinewidth=15,.dcmpmode=ROGUE_TA_CMPMODE_ALWAYS,
        .dwritedisable=true,.passtype=ROGUE_TA_PASSTYPE_OPAQUE,.objtype=ROGUE_TA_OBJTYPE_TRIANGLE);
    EMIT(TA_STATE_ISPB,.sop1=ROGUE_TA_ISPB_STENCILOP_KEEP,.sop2=ROGUE_TA_ISPB_STENCILOP_KEEP,
        .sop3=ROGUE_TA_ISPB_STENCILOP_KEEP,.scmpmode=ROGUE_TA_CMPMODE_ALWAYS);
    EMIT(TA_STATE_ISPDBSC,.dbindex=0,.scindex=0);
    EMIT(TA_STATE_PDS_SHADERBASE,.addr=PVR_DEV_ADDR(PVR_RENDER_PDS_ADDRESS+PVR_RENDER_BATCH_OPAQUE_PDS));
    EMIT(TA_STATE_PDS_TEXUNICODEBASE,.addr=PVR_DEV_ADDR(DRAW_UNIFORM_PDS+uniform_code));
    EMIT(TA_STATE_PDS_SIZEINFO1,
        .pds_uniformsize=DIV_ROUND_UP(uniform_data,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_UNIFORMSIZE_UNIT_SIZE),
        .pds_varyingsize=DIV_ROUND_UP(coefficient_data,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_VARYINGSIZE_UNIT_SIZE),
        .usc_varyingsize=DIV_ROUND_UP(fs->common.coeffs,ROGUE_TA_STATE_PDS_SIZEINFO1_USC_VARYINGSIZE_UNIT_SIZE),
        .pds_tempsize=DIV_ROUND_UP(coefficient_temps,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_TEMPSIZE_UNIT_SIZE));
    EMIT(TA_STATE_PDS_SIZEINFO2,.pds_tri_merge_disable=true,
        .usc_sharedsize=DIV_ROUND_UP(fs->common.shareds,ROGUE_TA_STATE_PDS_SIZEINFO2_USC_SHAREDSIZE_UNIT_SIZE));
    EMIT(TA_STATE_PDS_VARYINGBASE,.addr=PVR_DEV_ADDR(DRAW_COEFFICIENT_PDS));
    EMIT(TA_STATE_PDS_UNIFORMDATABASE,.addr=PVR_DEV_ADDR(DRAW_UNIFORM_PDS));
    unsigned region_size=16+16*!PVR_HAS_FEATURE(dev,tile_size_16x16);
    EMIT(TA_REGION_CLIP0,.left=0,.right=DIV_ROUND_UP(64,region_size)-1,.mode=ROGUE_TA_REGION_CLIP_MODE_OUTSIDE);
    EMIT(TA_REGION_CLIP1,.top=0,.bottom=DIV_ROUND_UP(64,region_size)-1);
    *p++=fui(32.0f);*p++=fui(32.0f);*p++=fui(32.0f);*p++=fui(32.0f);*p++=fui(0.0f);*p++=fui(1.0f);
    EMIT(TA_WCLAMP,.val=fui(1.0e-13f));
    EMIT(TA_OUTPUT_SEL,.vtxsize=vs->vs.vtxouts);
    /* pvr_arch_cmd_buffer.c:6882-6884 maps smooth outputs to f32_linear. */
    EMIT(TA_STATE_VARYING0,.f32_linear=vs->vs.f32_smooth,.f32_npc=vs->vs.f32_npc);
    EMIT(TA_STATE_VARYING1,.f16_npc=0);
    *p++=0; /* Initial TA_STATE_VARYING2. */
    EMIT(TA_STATE_PPP_CTRL,.drawclippededges=true,.wclampen=true,.clip_mode=ROGUE_TA_CLIP_MODE_FRONT_REAR,
        .cullmode=ROGUE_TA_CULLMODE_NO_CULLING,.flatshade_vtx=ROGUE_TA_FLATSHADE_VTX_VERTEX_0);
    EMIT(TA_STATE_STREAM_OUT0,.stream0_ta_output=false);
    unsigned ppp_words=p-ppp;assert(ppp_words==PVR_RENDER_BATCH_PPP_WORDS);
    /* The native encoder emits one PPP update and index list per batch after
     * this shared vertex state; record the shared words for comparison. */
    p=(void *)(payload+8192+0xf000);
    uint32_t cam,instances;pvr_arch_calculate_vertex_cam_size(dev,vs->vs.vtxouts,true,&cam,&instances);
    EMIT(VDMCTRL_VDM_STATE0,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_VDM_STATE_UPDATE,
        .cam_size=cam,.vs_data_addr_present=true,.vs_other_present=true,
        .uvs_scratch_size_select=ROGUE_VDMCTRL_UVS_SCRATCH_SIZE_SELECT_FIVE,
        .flatshade_control=ROGUE_VDMCTRL_FLATSHADE_CONTROL_VERTEX_0);
    EMIT(VDMCTRL_VDM_STATE2,.vs_pds_data_base_addr=PVR_DEV_ADDR(DRAW_VERTEX_PDS));
    EMIT(VDMCTRL_VDM_STATE3,.vs_pds_code_base_addr=PVR_DEV_ADDR(DRAW_VERTEX_PDS+sizes.vertex_code));
    EMIT(VDMCTRL_VDM_STATE4,.vs_output_size=vs->vs.vtxouts);
    EMIT(VDMCTRL_VDM_STATE5,.vs_max_instances=instances,
        .vs_usc_unified_size=DIV_ROUND_UP(vs->common.vtxins*4,ROGUE_VDMCTRL_VDM_STATE5_VS_USC_UNIFIED_SIZE_UNIT_SIZE),
        .vs_pds_temp_size=DIV_ROUND_UP(sizes.vertex_temp*4,ROGUE_VDMCTRL_VDM_STATE5_VS_PDS_TEMP_SIZE_UNIT_SIZE),
        .vs_pds_data_size=DIV_ROUND_UP(sizes.vertex_data*4,ROGUE_VDMCTRL_VDM_STATE5_VS_PDS_DATA_SIZE_UNIT_SIZE));
    assert((unsigned char *)p==payload+8192+0xf000+PVR_RENDER_BATCH_VDM_STATE_BYTES);
    EMIT(VDMCTRL_STREAM_TERMINATE,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE);
    PACK(payload+8192+0x10060,IPF_SCISSOR_WORD_0,.scw0_xmin=0,.scw0_xmax=64);
    PACK(payload+8192+0x10064,IPF_SCISSOR_WORD_1,.scw1_ymin=0,.scw1_ymax=64);
    uint32_t clear=PVR_RENDER_PIXEL;memcpy(payload+8192+0x10000,&clear,4);
    fprintf(stderr,"BATCH_CONTROL ppp_words=%u cam=%u instances=%u region=%u f32_linear=%u f32_npc=%u coeffs=%u\n",
        ppp_words,cam,instances,region_size,vs->vs.f32_smooth,vs->vs.f32_npc,fs->common.coeffs);
#undef EMIT
}

/* Shared vertex, coefficient and descriptor programs follow
 * pvr_graphics_pipeline_setup_fragment_coeff_program with perspective W. */
static void batch_shared_pds(const pco_data *vs,const pco_data *fs)
{
    assert(vs->common.vtxins==6 && vs->vs.vtxouts==6 && !vs->common.coeffs);
    assert(fs->fs.uses.w && !fs->fs.uses.z && !fs->fs.uses.pntc && vs->vs.f32_smooth==2 && !vs->vs.f32_npc);
    struct pvr_pds_vertex_dma dma[2]={0};
    for(unsigned i=0;i<2;++i) {
        const pco_range *r=&vs->vs.attribs[VERT_ATTRIB_GENERIC0+i];assert(r->count==(i?2:4));
        dma[i]=(struct pvr_pds_vertex_dma){.offset=i*16,.stride=24,.size_in_dwords=r->count,
            .attrib_size_in_bytes=r->count*4,.destination=r->start,.binding_index=0,.divisor=1};
    }
    unsigned char entries[1024]={0};
    struct pvr_pds_info vi={.entries=(void *)entries,.entries_size_in_bytes=sizeof(entries)};
    struct pvr_pds_vertex_primary_program_input in={.dma_list=dma,.dma_count=2};
    pvr_pds_setup_doutu(&in.usc_task_control,0,vs->common.temps,ROGUE_PDSINST_DOUTU_SAMPLE_RATE_INSTANCE,false);
    pvr_pds_generate_vertex_primary_program(&in,NULL,&vi,false,dev);
    unsigned vcode=ALIGN_POT(vi.data_size_in_dwords*4,16);
    assert(vcode+vi.code_size_in_dwords*4<=384);
    pvr_pds_generate_vertex_primary_program(&in,pds(DRAW_VERTEX_PDS+vcode),&vi,false,dev);
    batch_constants(&vi,pds(DRAW_VERTEX_PDS),1);
    fprintf(stderr,"BATCH_VERTEX_PDS data=%u code=%u temps=%u code_offset=%u entries=%u\n",vi.data_size_in_dwords,vi.code_size_in_dwords,vi.temps_required,vcode,vi.entry_count);
    sizes.vertex_data=vi.data_size_in_dwords;sizes.vertex_code=vcode;sizes.vertex_temp=vi.temps_required;

    struct pvr_pds_coeff_loading_program coeff={.z_iterator=~0u};unsigned fpu=0;
    PACK(coeff.FPU_iterators+fpu,PDSINST_DOUT_FIELDS_DOUTI_SRC,.f32_offset=0,.f16_offset=0,
        .shademodel=ROGUE_PDSINST_DOUTI_SHADEMODEL_GOURUAD,.size=ROGUE_PDSINST_DOUTI_SIZE_1D);
    const pco_range *w=&fs->fs.varyings[VARYING_SLOT_POS];assert(w->start==0 && w->count==ROGUE_USC_COEFFICIENT_SET_SIZE);
    coeff.destination[fpu++]=0;
    const pco_range *cr=&fs->fs.varyings[VARYING_SLOT_VAR0],*vr=&vs->vs.varyings[VARYING_SLOT_VAR0];
    assert(cr->start==ROGUE_USC_COEFFICIENT_SET_SIZE && cr->count==2*ROGUE_USC_COEFFICIENT_SET_SIZE && vr->start==4 && vr->count==2);
    /* pos.x, pos.y and pos.z are not iterated; pos.w is. */
    unsigned vtxout=vr->start-3;
    PACK(coeff.FPU_iterators+fpu,PDSINST_DOUT_FIELDS_DOUTI_SRC,.f32_offset=vtxout*sizeof(uint16_t),
        .f16_offset=vtxout*sizeof(uint16_t),.perspective=true,
        .shademodel=ROGUE_PDSINST_DOUTI_SHADEMODEL_GOURUAD,.size=ROGUE_PDSINST_DOUTI_SIZE_2D);
    coeff.destination[fpu++]=cr->start/ROGUE_USC_COEFFICIENT_SET_SIZE;
    coeff.num_fpu_iterators=fpu;
    pvr_pds_coefficient_loading(&coeff,NULL,PDS_GENERATE_SIZES);
    uint32_t scratch[128]={0};pvr_pds_coefficient_loading(&coeff,scratch,PDS_GENERATE_CODEDATA_SEGMENTS);
    unsigned ccode=ALIGN_POT(coeff.data_size*4,16);
    assert(ccode+coeff.code_size*4<=128);
    memcpy(pds(DRAW_COEFFICIENT_PDS),scratch,coeff.data_size*4);
    memcpy(pds(DRAW_COEFFICIENT_PDS+ccode),scratch+coeff.data_size,coeff.code_size*4);
    fprintf(stderr,"BATCH_COEFFICIENT_PDS data=%u code=%u temps=%u code_offset=%u iterators=%u\n",coeff.data_size,coeff.code_size,coeff.temps_used,ccode,fpu);
    coefficient_data=ccode/4;coefficient_temps=coeff.temps_used;

    memset(entries,0,sizeof(entries));
    struct pvr_pds_info ui={.entries=(void *)entries,.entries_size_in_bytes=sizeof(entries)};
    struct pvr_pds_descriptor_program_input desc={.buffer_count=1,.descriptor_set_count=1};
    desc.descriptor_sets[0]=(struct pvr_pds_descriptor_set){.descriptor_set=0,.size_in_dwords=20,.destination=fs->common.desc_sets[0].range.start};
    desc.buffers[0]=(struct pvr_pds_buffer){.type=PVR_BUFFER_TYPE_FS_META,.size_in_dwords=1,.destination=fs->fs.meta.start};
    pvr_pds_generate_descriptor_upload_program(&desc,NULL,&ui);
    unsigned ucode=ALIGN_POT(ui.data_size_in_dwords*4,16);assert(ucode+ui.code_size_in_dwords*4<=128);
    pvr_pds_generate_descriptor_upload_program(&desc,pds(DRAW_UNIFORM_PDS+ucode),&ui);
    batch_constants(&ui,pds(DRAW_UNIFORM_PDS),0);
    uint32_t meta=1u<<PVR_FS_META_SAMPLE_MASK_OFFSET | 1u<<PVR_FS_META_COLOR_WRITE_ENABLE_OFFSET;
    memcpy(payload+8192+0x10040,&meta,4);
    fprintf(stderr,"BATCH_UNIFORM_PDS data=%u code=%u temps=%u code_offset=%u\n",ui.data_size_in_dwords,ui.code_size_in_dwords,ui.temps_required,ucode);
    uniform_data=ucode/4;uniform_code=ucode;
}
static void batch_kick(const pco_data *fs,unsigned usc,unsigned at)
{
    struct pvr_pds_kickusc_program kick={0};uint32_t scratch[128]={0};
    pvr_pds_setup_doutu(&kick.usc_task_control,usc+fs->common.entry_offset,fs->common.temps,0,fs->fs.uses.phase_change);
    pvr_pds_kick_usc(&kick,scratch,0,false,PDS_GENERATE_CODEDATA_SEGMENTS);
    unsigned fcode=ALIGN_POT(kick.data_size*4,16);assert(fcode+kick.code_size*4<=128);
    memcpy(pds(at),scratch,kick.data_size*4);
    memcpy(pds(at+fcode),scratch+kick.data_size,kick.code_size*4);
    fprintf(stderr,"BATCH_FRAGMENT_PDS variant=%u at=%u data=%u code=%u code_offset=%u\n",variant,at,kick.data_size,kick.code_size,fcode);
}

static nir_shader *batch_vertex(void)
{
    nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_VERTEX,pco_nir_options(),"sunstone_batch_vertex");
    nir_variable *position=nir_create_variable_with_location(b.shader,nir_var_shader_in,VERT_ATTRIB_GENERIC0,glsl_vec4_type());
    nir_variable *uv=nir_create_variable_with_location(b.shader,nir_var_shader_in,VERT_ATTRIB_GENERIC1,glsl_vec2_type());
    nir_variable *out_position=nir_create_variable_with_location(b.shader,nir_var_shader_out,VARYING_SLOT_POS,glsl_vec4_type());
    nir_variable *out_uv=nir_create_variable_with_location(b.shader,nir_var_shader_out,VARYING_SLOT_VAR0,glsl_vec2_type());
    out_uv->data.interpolation=INTERP_MODE_SMOOTH;
    nir_store_var(&b,out_position,nir_load_var(&b,position),15);
    nir_store_var(&b,out_uv,nir_load_var(&b,uv),3);
    nir_jump(&b,nir_jump_return);
    return b.shader;
}
static nir_shader *batch_fragment(void)
{
    nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,pco_nir_options(),"sunstone_batch_fragment");
    nir_variable *uv=nir_create_variable_with_location(b.shader,nir_var_shader_in,VARYING_SLOT_VAR0,glsl_vec2_type());
    uv->data.interpolation=INTERP_MODE_SMOOTH;
    nir_variable *sampler=nir_variable_create(b.shader,nir_var_uniform,
        glsl_sampler_type(GLSL_SAMPLER_DIM_2D,false,false,GLSL_TYPE_FLOAT),"texture");
    sampler->data.descriptor_set=0;sampler->data.binding=0;
    nir_deref_instr *deref=nir_build_deref_var(&b,sampler);
    nir_def *coord=nir_load_var(&b,uv),*lod=nir_imm_float(&b,0);
    nir_tex_instr *tex=nir_tex_instr_create(b.shader,4);
    tex->op=nir_texop_txl;tex->sampler_dim=GLSL_SAMPLER_DIM_2D;
    tex->dest_type=nir_type_float32;tex->coord_components=2;
    tex->src[0]=(nir_tex_src){.src_type=nir_tex_src_texture_deref,.src=nir_src_for_ssa(&deref->def)};
    tex->src[1]=(nir_tex_src){.src_type=nir_tex_src_sampler_deref,.src=nir_src_for_ssa(&deref->def)};
    tex->src[2]=(nir_tex_src){.src_type=nir_tex_src_coord,.src=nir_src_for_ssa(coord)};
    tex->src[3]=(nir_tex_src){.src_type=nir_tex_src_lod,.src=nir_src_for_ssa(lod)};
    nir_def_init(&tex->instr,&tex->def,4,32);nir_builder_instr_insert(&b,&tex->instr);
    nir_variable *output=nir_create_variable_with_location(b.shader,nir_var_shader_out,FRAG_RESULT_DATA0,
        variant?glsl_vec4_type():glsl_uint_type());
    nir_store_var(&b,output,variant?&tex->def:nir_pack_unorm_4x8(&b,&tex->def),variant?15:1);
    nir_jump(&b,nir_jump_return);
    return b.shader;
}
static pco_shader *batch_compile(pco_ctx *ctx,nir_shader *nir,pco_data *data,struct vk_pipeline_layout *layout)
{
    pvr_init_descriptors(data,nir,layout);
    pco_lower_nir(ctx,nir,data);pco_postprocess_nir(ctx,nir,data);
    if(nir->info.stage==MESA_SHADER_VERTEX) {
        pvr_alloc_vs_sysvals(data,nir);pvr_alloc_vs_attribs(data,nir);pvr_alloc_vs_varyings(data,nir);
    } else {
        pvr_alloc_fs_sysvals(data,nir);pvr_alloc_fs_varyings(data,nir);
        data->fs.outputs[FRAG_RESULT_DATA0]=(pco_range){.start=0,.count=1};
    }
    pvr_setup_descriptors(data,nir,layout);
    pco_shader *s=pco_trans_nir(ctx,nir,data,NULL);assert(s);
    ralloc_steal(s,nir);pco_process_ir(ctx,s);pco_encode_ir(ctx,s);
    const pco_common_data *c=&pco_shader_data(s)->common;
    assert(!c->scratch && !c->spilled_temps && !c->uses.barriers && !c->uses.atomics);
    if(nir->info.stage==MESA_SHADER_FRAGMENT)fprintf(stderr,"BATCH_BLEND variant=%u fbfetch=%u phase_change=%u olchk_skip=%u blend_consts=%u\n",
        variant,pco_shader_data(s)->fs.uses.fbfetch,pco_shader_data(s)->fs.uses.phase_change,
        pco_shader_data(s)->fs.uses.olchk_skip,pco_shader_data(s)->fs.blend_consts.count);
    fprintf(stderr,"BATCH_DESCRIPTOR variant=%u stage=%u shareds=%u range=%u:%u spill_reservation=%u:%u\n",
        variant,nir->info.stage,c->shareds,c->desc_sets[0].range.start,c->desc_sets[0].range.count,
        c->spill_info.start,c->spill_info.count);
    return s;
}
static void save(const char *directory,const char *name,const void *data,size_t bytes)
{
    char path[4096];assert(snprintf(path,sizeof(path),"%s/%s",directory,name)<(int)sizeof(path));
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,bytes,f)==bytes);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);glsl_type_singleton_init_or_ref();
    struct pvr_device_runtime_info runtime={.core_count=1};pco_ctx *ctx=pco_ctx_create(dev,&runtime,NULL);assert(ctx);
    struct pvr_descriptor_set_layout_binding binding={.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptor_count=1,.stage_flags=BITFIELD_BIT(MESA_SHADER_FRAGMENT),.stride=sizeof(struct pvr_combined_image_sampler_descriptor)};
    struct pvr_descriptor_set_layout set={.binding_count=1,.bindings=&binding,
        .stage_flags=BITFIELD_BIT(MESA_SHADER_FRAGMENT),.size=sizeof(struct pvr_combined_image_sampler_descriptor)};
    struct vk_pipeline_layout layout={.set_count=1,.set_layouts={&set.vk}};
    programs();fragment();
    unsigned char vertex_bytes[256];unsigned vertex_size=0;
    pco_data first_vs={0},first_fs={0};
    for(variant=0;variant<PVR_RENDER_BATCH_BLENDS;++variant) {
        nir_shader *vs=batch_vertex(),*fs=batch_fragment();pco_data vd={0},fd={0};
        vd.vs.attrib_formats[VERT_ATTRIB_GENERIC0]=PIPE_FORMAT_R32G32B32A32_FLOAT;
        vd.vs.attrib_formats[VERT_ATTRIB_GENERIC1]=PIPE_FORMAT_R32G32_FLOAT;
        fd.fs.output_formats[FRAG_RESULT_DATA0]=variant?PIPE_FORMAT_R8G8B8A8_UNORM:PIPE_FORMAT_R32_UINT;fd.fs.z_replicate=~0u;
        if(variant) {
            struct vk_color_blend_state cb={.attachment_count=1};cb.attachments[0]=equations[variant];
            pvr_init_fs_blend(&fd,&cb);
        }
        pco_preprocess_nir(ctx,vs,&vd);pco_preprocess_nir(ctx,fs,&fd);
        pco_link_nir(ctx,vs,fs,&vd,&fd);pco_rev_link_nir(ctx,vs,fs);
        pco_shader *vertex=batch_compile(ctx,vs,&vd,&layout),*pixel=batch_compile(ctx,fs,&fd,&layout);
        const pco_data *v=pco_shader_data(vertex),*f=pco_shader_data(pixel);
        if(!variant) {
            char path[4096];assert(snprintf(path,sizeof(path),"%s/vertex.bin",argv[1])<(int)sizeof(path));
            record(vertex,MESA_SHADER_VERTEX,path);
            vertex_size=pco_shader_binary_size(vertex);assert(vertex_size<=sizeof(vertex_bytes));
            memcpy(vertex_bytes,pco_shader_binary_data(vertex),vertex_size);
            upload(vertex,DRAW_VERTEX_USC,256);first_vs=*v;first_fs=*f;
        } else {
            /* Every variant links the identical resident vertex program. */
            assert(pco_shader_binary_size(vertex)==vertex_size && !memcmp(pco_shader_binary_data(vertex),vertex_bytes,vertex_size));
            assert(f->common.coeffs==first_fs.common.coeffs && f->common.shareds==first_fs.common.shareds &&
                f->fs.varyings[VARYING_SLOT_VAR0].start==first_fs.fs.varyings[VARYING_SLOT_VAR0].start &&
                f->fs.uses.w==first_fs.fs.uses.w && f->fs.meta.start==first_fs.fs.meta.start &&
                f->common.desc_sets[0].range.start==first_fs.common.desc_sets[0].range.start);
            assert(f->fs.uses.fbfetch && !f->fs.uses.phase_change && !f->fs.blend_consts.count);
        }
        char path[4096];assert(snprintf(path,sizeof(path),"%s/fragment-%u.bin",argv[1],variant)<(int)sizeof(path));
        record(pixel,MESA_SHADER_FRAGMENT,path);
        upload(pixel,placement[variant].usc,placement[variant].cap);
        if(!variant)batch_shared_pds(v,f);
        batch_kick(f,placement[variant].usc,placement[variant].pds);
        ralloc_free(vertex);ralloc_free(pixel);
    }
    batch_control(&first_vs,&first_fs);
    batch_descriptor(64,64,64);
    save(argv[1],"payload.bin",payload,sizeof(payload));
    ralloc_free(ctx);glsl_type_singleton_decref();return 0;
}
