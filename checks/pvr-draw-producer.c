/* SPDX-License-Identifier: MIT
 * Graphics shader construction follows the pinned Mesa PCO pipeline.
 * Copyright (c) 2022, 2025 Imagination Technologies Ltd. See LICENSE.pvr.
 */
int fixed_clear_producer_main(int argc,char **argv);
#define main fixed_clear_producer_main
#include "pvr-render-producer.c"
#undef main
#include "draw-allocators.inc"
#include "pvr_iface.h"
#include "csbgen/rogue/ipf.h"
void pvr_arch_calculate_vertex_cam_size(const struct pvr_device_info *dev_info,uint32_t vs_output_size,
    bool raster_enable,uint32_t *cam_size_out,uint32_t *vs_max_instances_out);
#include "draw-cam.inc"

#define DRAW_VERTEX_ADDRESS (PVR_RENDER_GENERAL_ADDRESS+0x60000)
#define DRAW_VERTEX_USC 2304u
#define DRAW_FRAGMENT_USC 2560u
#define DRAW_VERTEX_PDS 1792u
#define DRAW_COEFFICIENT_PDS 2176u
#define DRAW_FRAGMENT_PDS 2304u
#define DRAW_UNIFORM_PDS 2432u
#define DRAW_META_ADDRESS (PVR_RENDER_GENERAL_ADDRESS+0x10040)
static struct {unsigned vertex_data,vertex_code,vertex_temp,coeff_data,coeff_temp,uniform_data,uniform_code;} sizes;

static void draw_control(const pco_data *vs,const pco_data *fs)
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
    EMIT(TA_STATE_PDS_SHADERBASE,.addr=PVR_DEV_ADDR(DRAW_FRAGMENT_PDS));
    EMIT(TA_STATE_PDS_TEXUNICODEBASE,.addr=PVR_DEV_ADDR(DRAW_UNIFORM_PDS+sizes.uniform_code));
    EMIT(TA_STATE_PDS_SIZEINFO1,
        .pds_uniformsize=DIV_ROUND_UP(sizes.uniform_data,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_UNIFORMSIZE_UNIT_SIZE),
        .pds_varyingsize=DIV_ROUND_UP(sizes.coeff_data,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_VARYINGSIZE_UNIT_SIZE),
        .usc_varyingsize=DIV_ROUND_UP(fs->common.coeffs,ROGUE_TA_STATE_PDS_SIZEINFO1_USC_VARYINGSIZE_UNIT_SIZE),
        .pds_tempsize=DIV_ROUND_UP(sizes.coeff_temp,ROGUE_TA_STATE_PDS_SIZEINFO1_PDS_TEMPSIZE_UNIT_SIZE));
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
    EMIT(TA_STATE_VARYING0,.f32_npc=vs->vs.f32_npc);
    EMIT(TA_STATE_VARYING1,.f16_npc=0);
    *p++=0; /* Initial TA_STATE_VARYING2. */
    EMIT(TA_STATE_PPP_CTRL,.drawclippededges=true,.wclampen=true,.clip_mode=ROGUE_TA_CLIP_MODE_FRONT_REAR,
        .cullmode=ROGUE_TA_CULLMODE_NO_CULLING,.flatshade_vtx=ROGUE_TA_FLATSHADE_VTX_VERTEX_0);
    EMIT(TA_STATE_STREAM_OUT0,.stream0_ta_output=false);
    unsigned ppp_words=p-ppp;assert(ppp_words<=31);
    p=(void *)(payload+8192+0xf000);
    EMIT(VDMCTRL_PPP_STATE0,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_PPP_STATE_UPDATE,
        .addrmsb=PVR_DEV_ADDR(PVR_RENDER_GENERAL_ADDRESS+0xf100),.word_count=ppp_words);
    EMIT(VDMCTRL_PPP_STATE1,.addrlsb=PVR_DEV_ADDR(PVR_RENDER_GENERAL_ADDRESS+0xf100));
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
    EMIT(VDMCTRL_INDEX_LIST0,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_INDEX_LIST,.index_count_present=true,
        .primitive_topology=ROGUE_VDMCTRL_PRIMITIVE_TOPOLOGY_TRI_LIST,
        .degen_cull_enable=PVR_HAS_FEATURE(dev,vdm_degenerate_culling));
    assert((unsigned char *)p==payload+8192+0xf020);
    EMIT(VDMCTRL_INDEX_LIST2,.index_count=3);
    EMIT(VDMCTRL_STREAM_TERMINATE,.block_type=ROGUE_VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE);
    PACK(payload+8192+0x10060,IPF_SCISSOR_WORD_0,.scw0_xmin=0,.scw0_xmax=64);
    PACK(payload+8192+0x10064,IPF_SCISSOR_WORD_1,.scw1_ymin=0,.scw1_ymax=64);
    uint32_t clear=PVR_RENDER_PIXEL;memcpy(payload+8192+0x10000,&clear,4);
    fprintf(stderr,"DRAW_CONTROL ppp_words=%u vdm_bytes=%zu cam=%u instances=%u region=%u\n",ppp_words,
        (size_t)((unsigned char *)p-(payload+8192+0xf000)),cam,instances,region_size);
#undef EMIT
}

static void patch_constants(const struct pvr_pds_info *info,uint32_t *data,unsigned vertex)
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
            assert(vertex && p->binding_index==0 && p->stride==32 && p->size_in_dwords==4 && (p->offset==0 || p->offset==16));
            value=DRAW_VERTEX_ADDRESS+p->offset;bytes=sizeof(*p);width=2;break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_VERTEX_ATTRIBUTE_STRIDE: {
            const struct pvr_const_map_entry_vertex_attribute_stride *p=(const void *)cursor;
            assert(vertex && p->binding_index==0);value=32;bytes=sizeof(*p);break;
        }
        case PVR_PDS_CONST_MAP_ENTRY_TYPE_SPECIAL_BUFFER: {
            const struct pvr_const_map_entry_special_buffer *p=(const void *)cursor;
            assert(!vertex && p->buffer_type==PVR_BUFFER_TYPE_FS_META && p->size_in_dwords==1);
            value=DRAW_META_ADDRESS;bytes=sizeof(*p);width=2;break;
        }
        default:fprintf(stderr,"Unsupported draw constant type=%u offset=%u vertex=%u\n",e->type,e->const_offset,vertex);abort();
        }
        assert(e->const_offset+width<=info->data_size_in_dwords && !(e->const_offset%width));
        memcpy(data+e->const_offset,&value,width*4);cursor+=bytes;
    }
    assert(cursor==(const unsigned char *)info->entries+info->entries_written_size_in_bytes);
}
static void draw_pds(pco_shader *vertex,pco_shader *pixel)
{
    const pco_data *vs=pco_shader_data(vertex),*fs=pco_shader_data(pixel);
    assert(vs->common.vtxins==8 && vs->vs.vtxouts==8 && !vs->common.shareds && !vs->common.coeffs);
    assert(fs->common.coeffs==16 && fs->common.shareds==1 && fs->fs.meta.start==0 && fs->fs.meta.count==1);
    assert(!fs->fs.uses.z && !fs->fs.uses.w && vs->vs.f32_npc==4);
    struct pvr_pds_vertex_dma dma[2]={0};
    for(unsigned i=0;i<2;++i) {
        const pco_range *r=&vs->vs.attribs[VERT_ATTRIB_GENERIC0+i];assert(r->count==4);
        dma[i]=(struct pvr_pds_vertex_dma){.offset=i*16,.stride=32,.size_in_dwords=4,
            .attrib_size_in_bytes=16,.destination=r->start,.binding_index=0,.divisor=1};
    }
    unsigned char entries[1024]={0};
    struct pvr_pds_info vi={.entries=(void *)entries,.entries_size_in_bytes=sizeof(entries)};
    struct pvr_pds_vertex_primary_program_input in={.dma_list=dma,.dma_count=2};
    pvr_pds_setup_doutu(&in.usc_task_control,0,vs->common.temps,ROGUE_PDSINST_DOUTU_SAMPLE_RATE_INSTANCE,false);
    pvr_pds_generate_vertex_primary_program(&in,NULL,&vi,false,dev);
    unsigned vcode=ALIGN_POT(vi.data_size_in_dwords*4,16);
    assert(vcode+vi.code_size_in_dwords*4<=384);
    pvr_pds_generate_vertex_primary_program(&in,pds(DRAW_VERTEX_PDS+vcode),&vi,false,dev);
    patch_constants(&vi,pds(DRAW_VERTEX_PDS),1);
    fprintf(stderr,"DRAW_VERTEX_PDS data=%u code=%u temps=%u code_offset=%u entries=%u\n",vi.data_size_in_dwords,vi.code_size_in_dwords,vi.temps_required,vcode,vi.entry_count);

    struct pvr_pds_coeff_loading_program coeff={.num_fpu_iterators=1,.z_iterator=~0u};
    const pco_range *cr=&fs->fs.varyings[VARYING_SLOT_VAR0],*vr=&vs->vs.varyings[VARYING_SLOT_VAR0];
    assert(cr->start==0 && cr->count==16 && vr->start==4 && vr->count==4);
    PACK(coeff.FPU_iterators,PDSINST_DOUT_FIELDS_DOUTI_SRC,.f32_offset=0,.f16_offset=0,
        .shademodel=ROGUE_PDSINST_DOUTI_SHADEMODEL_GOURUAD,.size=ROGUE_PDSINST_DOUTI_SIZE_4D);
    coeff.destination[0]=0;
    pvr_pds_coefficient_loading(&coeff,NULL,PDS_GENERATE_SIZES);
    uint32_t scratch[128]={0};pvr_pds_coefficient_loading(&coeff,scratch,PDS_GENERATE_CODEDATA_SEGMENTS);
    unsigned ccode=ALIGN_POT(coeff.data_size*4,16);
    assert(ccode+coeff.code_size*4<=128);
    memcpy(pds(DRAW_COEFFICIENT_PDS),scratch,coeff.data_size*4);
    memcpy(pds(DRAW_COEFFICIENT_PDS+ccode),scratch+coeff.data_size,coeff.code_size*4);
    fprintf(stderr,"DRAW_COEFFICIENT_PDS data=%u code=%u temps=%u code_offset=%u\n",coeff.data_size,coeff.code_size,coeff.temps_used,ccode);

    struct pvr_pds_kickusc_program kick={0};
    pvr_pds_setup_doutu(&kick.usc_task_control,DRAW_FRAGMENT_USC+fs->common.entry_offset,fs->common.temps,0,fs->fs.uses.phase_change);
    memset(scratch,0,sizeof(scratch));pvr_pds_kick_usc(&kick,scratch,0,false,PDS_GENERATE_CODEDATA_SEGMENTS);
    unsigned fcode=ALIGN_POT(kick.data_size*4,16);assert(fcode+kick.code_size*4<=128);
    memcpy(pds(DRAW_FRAGMENT_PDS),scratch,kick.data_size*4);
    memcpy(pds(DRAW_FRAGMENT_PDS+fcode),scratch+kick.data_size,kick.code_size*4);
    fprintf(stderr,"DRAW_FRAGMENT_PDS data=%u code=%u code_offset=%u\n",kick.data_size,kick.code_size,fcode);

    memset(entries,0,sizeof(entries));
    struct pvr_pds_info ui={.entries=(void *)entries,.entries_size_in_bytes=sizeof(entries)};
    struct pvr_pds_descriptor_program_input desc={.buffer_count=1};
    desc.buffers[0]=(struct pvr_pds_buffer){.type=PVR_BUFFER_TYPE_FS_META,.size_in_dwords=1,.destination=fs->fs.meta.start};
    pvr_pds_generate_descriptor_upload_program(&desc,NULL,&ui);
    unsigned ucode=ALIGN_POT(ui.data_size_in_dwords*4,16);assert(ucode+ui.code_size_in_dwords*4<=128);
    pvr_pds_generate_descriptor_upload_program(&desc,pds(DRAW_UNIFORM_PDS+ucode),&ui);
    patch_constants(&ui,pds(DRAW_UNIFORM_PDS),0);
    uint32_t meta=1u<<PVR_FS_META_SAMPLE_MASK_OFFSET | 1u<<PVR_FS_META_COLOR_WRITE_ENABLE_OFFSET;
    memcpy(payload+8192+0x10040,&meta,4);
    fprintf(stderr,"DRAW_UNIFORM_PDS data=%u code=%u temps=%u code_offset=%u\n",ui.data_size_in_dwords,ui.code_size_in_dwords,ui.temps_required,ucode);
    sizes.vertex_data=vi.data_size_in_dwords;sizes.vertex_code=vcode;sizes.vertex_temp=vi.temps_required;
    sizes.coeff_data=ccode/4;sizes.coeff_temp=coeff.temps_used;sizes.uniform_data=ucode/4;sizes.uniform_code=ucode;
    draw_control(vs,fs);
}

static nir_shader *draw_vertex(void)
{
    nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_VERTEX,pco_nir_options(),"sunstone_vertex_color");
    nir_variable *position=nir_create_variable_with_location(b.shader,nir_var_shader_in,VERT_ATTRIB_GENERIC0,glsl_vec4_type());
    nir_variable *color=nir_create_variable_with_location(b.shader,nir_var_shader_in,VERT_ATTRIB_GENERIC1,glsl_vec4_type());
    nir_variable *out_position=nir_create_variable_with_location(b.shader,nir_var_shader_out,VARYING_SLOT_POS,glsl_vec4_type());
    nir_variable *out_color=nir_create_variable_with_location(b.shader,nir_var_shader_out,VARYING_SLOT_VAR0,glsl_vec4_type());
    out_color->data.interpolation=INTERP_MODE_NOPERSPECTIVE;
    nir_store_var(&b,out_position,nir_load_var(&b,position),15);
    nir_store_var(&b,out_color,nir_load_var(&b,color),15);
    nir_jump(&b,nir_jump_return);
    return b.shader;
}
static nir_shader *draw_fragment(void)
{
    nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,pco_nir_options(),"sunstone_interpolated_color");
    nir_variable *color=nir_create_variable_with_location(b.shader,nir_var_shader_in,VARYING_SLOT_VAR0,glsl_vec4_type());
    color->data.interpolation=INTERP_MODE_NOPERSPECTIVE;
    nir_variable *output=nir_create_variable_with_location(b.shader,nir_var_shader_out,FRAG_RESULT_DATA0,glsl_uint_type());
    nir_store_var(&b,output,nir_pack_unorm_4x8(&b,nir_load_var(&b,color)),1);
    nir_jump(&b,nir_jump_return);
    return b.shader;
}
static pco_shader *draw_compile(pco_ctx *ctx,nir_shader *nir,pco_data *data)
{
    pco_lower_nir(ctx,nir,data);
    pco_postprocess_nir(ctx,nir,data);
    if(nir->info.stage==MESA_SHADER_VERTEX) {
        pvr_alloc_vs_sysvals(data,nir);
        pvr_alloc_vs_attribs(data,nir);
        pvr_alloc_vs_varyings(data,nir);
    } else {
        pvr_alloc_fs_sysvals(data,nir);
        pvr_alloc_fs_varyings(data,nir);
        data->fs.outputs[FRAG_RESULT_DATA0]=(pco_range){.start=0,.count=1};
    }
    pco_shader *s=pco_trans_nir(ctx,nir,data,NULL);assert(s);
    ralloc_steal(s,nir);pco_process_ir(ctx,s);pco_encode_ir(ctx,s);
    return s;
}
static void record(pco_shader *s,unsigned stage,const char *path)
{
    const pco_data *m=pco_shader_data(s);
    fprintf(stderr,"DRAW_META stage=%u bytes=%u entry=%u temps=%u inputs=%u outputs=%u coeffs=%u shareds=%u scratch=%u spilled=%u z=%u w=%u meta=%u:%u\n",
        stage,pco_shader_binary_size(s),m->common.entry_offset,m->common.temps,m->common.vtxins,
        stage==MESA_SHADER_VERTEX?m->vs.vtxouts:0,m->common.coeffs,m->common.shareds,m->common.scratch,
        m->common.spilled_temps,stage==MESA_SHADER_FRAGMENT?m->fs.uses.z:0,
        stage==MESA_SHADER_FRAGMENT?m->fs.uses.w:0,stage==MESA_SHADER_FRAGMENT?m->fs.meta.start:0,
        stage==MESA_SHADER_FRAGMENT?m->fs.meta.count:0);
    FILE *f=fopen(path,"wb");assert(f);
    unsigned bytes=pco_shader_binary_size(s);
    assert(fwrite(pco_shader_binary_data(s),1,bytes,f)==bytes);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==4);glsl_type_singleton_init_or_ref();
    struct pvr_device_runtime_info runtime={.core_count=1};
    pco_ctx *ctx=pco_ctx_create(dev,&runtime,NULL);assert(ctx);
    nir_shader *vs=draw_vertex(),*fs=draw_fragment();pco_data vd={0},fd={0};
    vd.vs.attrib_formats[VERT_ATTRIB_GENERIC0]=PIPE_FORMAT_R32G32B32A32_FLOAT;
    vd.vs.attrib_formats[VERT_ATTRIB_GENERIC1]=PIPE_FORMAT_R32G32B32A32_FLOAT;
    fd.fs.output_formats[FRAG_RESULT_DATA0]=PIPE_FORMAT_R32_UINT;
    fd.fs.z_replicate=~0u;
    pco_preprocess_nir(ctx,vs,&vd);pco_preprocess_nir(ctx,fs,&fd);
    pco_link_nir(ctx,vs,fs,&vd,&fd);pco_rev_link_nir(ctx,vs,fs);
    pco_shader *vertex=draw_compile(ctx,vs,&vd),*pixel=draw_compile(ctx,fs,&fd);
    record(vertex,MESA_SHADER_VERTEX,argv[1]);record(pixel,MESA_SHADER_FRAGMENT,argv[2]);
    programs();fragment();
    upload(vertex,DRAW_VERTEX_USC,256);upload(pixel,DRAW_FRAGMENT_USC,256);
    draw_pds(vertex,pixel);
    FILE *f=fopen(argv[3],"wb");assert(f);assert(fwrite(payload,1,sizeof(payload),f)==sizeof(payload));assert(!fclose(f));
    ralloc_free(vertex);ralloc_free(pixel);ralloc_free(ctx);
    glsl_type_singleton_decref();return 0;
}
