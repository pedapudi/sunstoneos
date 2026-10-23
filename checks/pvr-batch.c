/* SPDX-License-Identifier: MIT
 * Compare the native profile-4 encoders with pinned Mesa programs, packers and
 * enumeration helpers. Nothing here executes GPU instructions.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvr_render_texture.h"
#include "pvr_render_job.h"
#include "common/device_info/bxm-4-64.h"
#include "hwdef/rogue_hw_utils.h"
#include "util/u_math.h"
#include "vulkan/vulkan_core.h"
#include "util/macros.h"
#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(a) (a).addr
#define __pvr_make_address(a) PVR_DEV_ADDR(a)
#include "pvr_types.h"
#include "csbgen/rogue/cr.h"
#include "csbgen/rogue/ppp.h"
#include "csbgen/rogue/vdm.h"
#include "csbgen/rogue/texstate.h"
/* Extracted unchanged from pinned pvr_csb_enum_helpers.h and pvr_arch_sampler.c. */
#include "batch-mesa.inc"
void batch_job_reference(const struct pvr_render_job_image *,unsigned,unsigned);
static unsigned char payload[PVR_RENDER_PROGRAM_BYTES],reference[PVR_RENDER_PROGRAM_BYTES];
static const struct pvr_device_info *dev=&pvr_device_info_36_56_104_183;

static void programs(void)
{
    struct pvr_render_program_image image;
    assert(pvr_render_texture_programs_build_profile(payload,sizeof(payload),64,64,4,&image));
    /* Every resident span the producer wrote, including program padding. */
    const unsigned spans[][2]={{0,1792},{1792,384},{2176,128},{2304,128},{2432,128},
        {PVR_RENDER_BATCH_BLEND_PDS0,128},{PVR_RENDER_BATCH_BLEND_PDS1,128},
        {PVR_RENDER_BATCH_BLEND_PDS2,128},{PVR_RENDER_BATCH_BLEND_PDS3,128},
        {4096,4096},{8192+0xf100,104},{8192+0x10040,4},{8192+0x10060,8}};
    for(unsigned i=0;i<sizeof(spans)/sizeof(spans[0]);++i)
        if(memcmp(payload+spans[i][0],reference+spans[i][0],spans[i][1])) {
            fprintf(stderr,"span %u at %u differs\n",i,spans[i][0]);abort();
        }
    /* The producer's descriptor is the default nearest clamp-to-edge state. */
    struct pvr_render_texture_input input={64,64,64,4,6};unsigned char descriptor[80];
    assert(pvr_render_batch_descriptor(descriptor,&input,1u|2u<<6|2u<<9));
    assert(!memcmp(descriptor,reference+8192+0x10100,80));
}

static uint32_t cull(unsigned mode)
{
    /* pvr_arch_cmd_buffer.c:7872-7895 with a counter-clockwise front face:
     * back culling removes clockwise triangles, front culling the others. */
    const VkFrontFace front=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if(!mode)return ROGUE_TA_CULLMODE_NO_CULLING;
    VkCullModeFlags vk=mode==1?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_FRONT_BIT;
    return ((front==VK_FRONT_FACE_COUNTER_CLOCKWISE)^(vk==VK_CULL_MODE_FRONT_BIT))?
        ROGUE_TA_CULLMODE_CULL_CW:ROGUE_TA_CULLMODE_CULL_CCW;
}
static void commands(void)
{
    struct pvr_render_program_image image;
    const unsigned shapes[][2]={{1,1},{64,64},{1280,720},{1920,1080}};
    for(unsigned shape=0;shape<4;++shape) {
        unsigned width=shapes[shape][0],height=shapes[shape][1];
        assert(pvr_render_texture_programs_build_profile(payload,sizeof(payload),width,height,4,&image));
        struct pvr_render_batch b[PVR_RENDER_BATCH_MAX];
        for(unsigned i=0;i<PVR_RENDER_BATCH_MAX;++i) {
            unsigned stencil=i&1,blend=i%5,compare=i&7,write=(i>>1)&1;
            b[i]=(struct pvr_render_batch){.first=3*i,.count=3*(i+1),
                .state=compare | (write?PVR_RENDER_BATCH_DEPTH_WRITE:0) | (i%3)<<4 | blend<<6 |
                    ((!blend && i==10)?PVR_RENDER_BATCH_NO_COLOR:0) |
                    (stencil?PVR_RENDER_BATCH_STENCIL | (7-compare)<<11 | (i&7)<<14 | ((i+3)&7)<<17 | ((i+5)&7)<<20:0),
                .stencil=stencil?((i*17u)&255) | ((255u-i)<<8) | ((i*3u)&255)<<16:0};
            assert(pvr_render_batch_valid(b+i,3*PVR_RENDER_BATCH_MAX*2));
        }
        for(unsigned count=1;count<=PVR_RENDER_BATCH_MAX;count+=count==1?7:8) {
            uint32_t frame=count;
            assert(pvr_render_batch_commands(payload,width,height,frame,b,3*PVR_RENDER_BATCH_MAX*2));
            const uint32_t *w=(const void *)(payload+8192+PVR_RENDER_BATCH_VDM_OFFSET);
            for(unsigned i=0;i<count;++i) {
                struct ROGUE_VDMCTRL_PPP_STATE0 s0;struct ROGUE_VDMCTRL_PPP_STATE1 s1;
                ROGUE_VDMCTRL_PPP_STATE0_unpack(w++,&s0);ROGUE_VDMCTRL_PPP_STATE1_unpack(w++,&s1);
                uint64_t block=PVR_RENDER_GENERAL_ADDRESS+PVR_RENDER_BATCH_PPP_OFFSET+i*PVR_RENDER_BATCH_PPP_STRIDE;
                assert(s0.block_type==ROGUE_VDMCTRL_BLOCK_TYPE_PPP_STATE_UPDATE && s0.word_count==PVR_RENDER_BATCH_PPP_WORDS);
                assert((s0.addrmsb.addr|s1.addrlsb.addr)==block);
                if(!i) {
                    /* Producer-recorded VDM state at the old stream base. */
                    assert(!memcmp(w,reference+8192+0xf000,PVR_RENDER_BATCH_VDM_STATE_BYTES));
                    w+=PVR_RENDER_BATCH_VDM_STATE_BYTES/4;
                }
                struct ROGUE_VDMCTRL_INDEX_LIST0 l0;struct ROGUE_VDMCTRL_INDEX_LIST1 l1;struct ROGUE_VDMCTRL_INDEX_LIST2 l2;
                ROGUE_VDMCTRL_INDEX_LIST0_unpack(w++,&l0);ROGUE_VDMCTRL_INDEX_LIST1_unpack(w++,&l1);ROGUE_VDMCTRL_INDEX_LIST2_unpack(w++,&l2);
                assert(l0.block_type==ROGUE_VDMCTRL_BLOCK_TYPE_INDEX_LIST && l0.index_addr_present && l0.index_count_present &&
                    !l0.index_instance_count_present && !l0.index_offset_present && !l0.start_present &&
                    !l0.indirect_addr_present && !l0.split_count_present && l0.index_size==ROGUE_VDMCTRL_INDEX_SIZE_B16 &&
                    l0.primitive_topology==ROGUE_VDMCTRL_PRIMITIVE_TOPOLOGY_TRI_LIST && !l0.patch_count &&
                    l0.degen_cull_enable==PVR_HAS_FEATURE(dev,vdm_degenerate_culling));
                assert((l0.index_base_addrmsb.addr|l1.index_base_addrlsb.addr)==PVR_RENDER_TEXTURE_INDEX_ADDRESS+b[i].first*2 &&
                    l2.index_count==b[i].count);
                /* Each block equals the template except the documented fields. */
                const uint32_t *ppp=(const void *)(payload+8192+PVR_RENDER_BATCH_PPP_OFFSET+i*PVR_RENDER_BATCH_PPP_STRIDE);
                const uint32_t *base=(const void *)(payload+8192+0xf100);
                for(unsigned k=0;k<PVR_RENDER_BATCH_PPP_WORDS;++k)
                    if(k!=1 && k!=2 && k!=3 && k!=5 && k!=24)assert(ppp[k]==base[k]);
                struct ROGUE_TA_STATE_ISPCTL c,cb;struct ROGUE_TA_STATE_ISPA a,ab;struct ROGUE_TA_STATE_ISPB sb;
                struct ROGUE_TA_STATE_PDS_SHADERBASE shader;struct ROGUE_TA_STATE_PPP_CTRL control,controlb;
                ROGUE_TA_STATE_ISPCTL_unpack(ppp+1,&c);ROGUE_TA_STATE_ISPCTL_unpack(base+1,&cb);
                ROGUE_TA_STATE_ISPA_unpack(ppp+2,&a);ROGUE_TA_STATE_ISPA_unpack(base+2,&ab);
                ROGUE_TA_STATE_ISPB_unpack(ppp+3,&sb);ROGUE_TA_STATE_PDS_SHADERBASE_unpack(ppp+5,&shader);
                ROGUE_TA_STATE_PPP_CTRL_unpack(ppp+24,&control);ROGUE_TA_STATE_PPP_CTRL_unpack(base+24,&controlb);
                uint32_t s=b[i].state;unsigned blend=(s>>6)&7;
                assert(c.tagwritedisable==!!(s&PVR_RENDER_BATCH_NO_COLOR));
                cb.tagwritedisable=c.tagwritedisable;assert(!memcmp(&c,&cb,sizeof(c)));
                assert(a.dcmpmode==pvr_ta_cmpmode((VkCompareOp)(s&7)) && a.dwritedisable==!(s&PVR_RENDER_BATCH_DEPTH_WRITE));
                assert(a.passtype==(blend?ROGUE_TA_PASSTYPE_TRANSLUCENT:ROGUE_TA_PASSTYPE_OPAQUE));
                assert(a.sref==((s&PVR_RENDER_BATCH_STENCIL)?(b[i].stencil&255):0));
                ab.dcmpmode=a.dcmpmode;ab.dwritedisable=a.dwritedisable;ab.passtype=a.passtype;ab.sref=a.sref;
                assert(!memcmp(&a,&ab,sizeof(a)));
                if(s&PVR_RENDER_BATCH_STENCIL) {
                    assert(sb.scmpmode==pvr_ta_cmpmode((VkCompareOp)((s>>11)&7)) &&
                        sb.sop1==pvr_ta_stencilop((VkStencilOp)((s>>14)&7)) &&
                        sb.sop2==pvr_ta_stencilop((VkStencilOp)((s>>17)&7)) &&
                        sb.sop3==pvr_ta_stencilop((VkStencilOp)((s>>20)&7)) &&
                        sb.scmpmask==((b[i].stencil>>8)&255) && sb.swmask==((b[i].stencil>>16)&255));
                } else assert(ppp[3]==base[3] && sb.scmpmode==ROGUE_TA_CMPMODE_ALWAYS && !sb.sop1 && !sb.sop2 && !sb.sop3 && !sb.swmask);
                const unsigned pds[]={PVR_RENDER_BATCH_OPAQUE_PDS,PVR_RENDER_BATCH_BLEND_PDS0,PVR_RENDER_BATCH_BLEND_PDS1,
                    PVR_RENDER_BATCH_BLEND_PDS2,PVR_RENDER_BATCH_BLEND_PDS3};
                assert(shader.addr.addr==pds[blend]);
                assert(control.cullmode==cull((s>>4)&3));controlb.cullmode=control.cullmode;
                assert(!memcmp(&control,&controlb,sizeof(control)));
                /* Region clip and viewport come from the session shape. */
                struct ROGUE_TA_REGION_CLIP0 x;struct ROGUE_TA_REGION_CLIP1 y;
                ROGUE_TA_REGION_CLIP0_unpack(ppp+11,&x);ROGUE_TA_REGION_CLIP1_unpack(ppp+12,&y);
                assert(x.right==(width+15)/16-1 && y.bottom==(height+15)/16-1 && x.mode==ROGUE_TA_REGION_CLIP_MODE_OUTSIDE);
                float vx,vy;memcpy(&vx,ppp+13,4);memcpy(&vy,ppp+15,4);assert(vx==width*0.5f && vy==height*0.5f);
            }
            struct ROGUE_VDMCTRL_STREAM_TERMINATE end;ROGUE_VDMCTRL_STREAM_TERMINATE_unpack(w++,&end);
            assert(end.block_type==ROGUE_VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE);
            assert((const unsigned char *)w-(payload+8192+PVR_RENDER_BATCH_VDM_OFFSET)<=
                PVR_RENDER_BATCH_PPP_OFFSET-PVR_RENDER_BATCH_VDM_OFFSET);
        }
    }
    /* The template's own fields match the producer's Mesa packing. */
    const uint32_t *ppp=(const void *)(payload+8192+0xf100);
    struct ROGUE_TA_STATE_VARYING0 v;ROGUE_TA_STATE_VARYING0_unpack(ppp+21,&v);
    assert(v.f32_linear==2 && !v.f32_npc && !v.f32_flat);
    /* Refusals leave the stream untouched. */
    unsigned char before[4096];memcpy(before,payload+8192+PVR_RENDER_BATCH_VDM_OFFSET,4096);
    struct pvr_render_batch bad={0,6,0,0};
    assert(!pvr_render_batch_commands(payload,64,64,0,&bad,6));
    assert(!pvr_render_batch_commands(payload,64,64,1,&bad,3));
    assert(!memcmp(before,payload+8192+PVR_RENDER_BATCH_VDM_OFFSET,4096));
}

static void descriptors(void)
{
    struct pvr_render_texture_input input={64,64,64,4,6};
    unsigned char plain[80],out[80];
    assert(pvr_render_texture_descriptor(plain,&input));
    for(uint32_t frame=1;frame<1u<<13;frame+=7) {
        uint32_t u=(frame>>6)&7,v=(frame>>9)&7;
        int valid=pvr_render_batch_frame_valid(frame);
        assert(valid==((frame&31) && (frame&31)<=16 && u<=4 && v<=4));
        if(!valid){memset(out,0x5a,80);assert(!pvr_render_batch_descriptor(out,&input,frame));
            for(unsigned i=0;i<80;++i)assert(out[i]==0x5a);continue;}
        assert(pvr_render_batch_descriptor(out,&input,frame));
        struct ROGUE_TEXSTATE_IMAGE_WORD0 image,image_plain;struct ROGUE_TEXSTATE_SAMPLER_WORD0 sampler,gather,expected;
        ROGUE_TEXSTATE_IMAGE_WORD0_unpack(out,&image);ROGUE_TEXSTATE_IMAGE_WORD0_unpack(plain,&image_plain);
        ROGUE_TEXSTATE_SAMPLER_WORD0_unpack(out+32,&sampler);ROGUE_TEXSTATE_SAMPLER_WORD0_unpack(out+64,&gather);
        ROGUE_TEXSTATE_SAMPLER_WORD0_unpack(plain+32,&expected);
        VkFilter filter=(frame&PVR_RENDER_BATCH_LINEAR)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        expected.magfilter=pvr_sampler_get_hw_filter_from_vk(dev,filter);
        expected.minfilter=pvr_sampler_get_hw_filter_from_vk(dev,filter);
        expected.addrmode_u=pvr_sampler_get_hw_addr_mode_from_vk((VkSamplerAddressMode)u);
        expected.addrmode_v=pvr_sampler_get_hw_addr_mode_from_vk((VkSamplerAddressMode)v);
        assert(!memcmp(&sampler,&expected,sizeof(expected)) && !expected.bordercolor_index);
        /* pvr_arch_sampler.c: gather copies word 0 with linear filters. */
        expected.minfilter=expected.magfilter=ROGUE_TEXSTATE_FILTER_LINEAR;expected.mipfilter=false;
        assert(!memcmp(&gather,&expected,sizeof(expected)));
        /* pvr_arch_tex_state.c sets gamma for four-component sRGB; the
         * overlapping two-component field shares bit 3. */
        image_plain.gamma=(frame&PVR_RENDER_BATCH_SRGB)?ROGUE_TEXSTATE_GAMMA_ON:ROGUE_TEXSTATE_GAMMA_OFF;
        image_plain.twocomp_gamma=(enum ROGUE_TEXSTATE_TWOCOMP_GAMMA)image_plain.gamma;
        assert(!memcmp(&image,&image_plain,sizeof(image)));
        uint32_t pck;memcpy(&pck,out+28,4);
        assert(pck==((frame&PVR_RENDER_BATCH_SRGB)?PVR_RENDER_BATCH_SRGB_PACK:320u));
        assert(!memcmp(out+8,plain+8,20) && !memcmp(out+40,plain+40,24) && !memcmp(out+72,plain+72,8));
    }
}

static void depth(void)
{
    struct pvr_render_program_image programs;struct pvr_render_job_image job;
    assert(pvr_render_texture_programs_build_profile(payload,sizeof(payload),64,64,4,&programs));
    const unsigned shapes[][2]={{1,1},{31,33},{64,64},{1280,720},{1919,1079},{1920,1080}};
    for(unsigned i=0;i<sizeof(shapes)/sizeof(shapes[0]);++i) {
        unsigned width=shapes[i][0],height=shapes[i][1];
        assert(pvr_render_job_build_frame_pr(&programs,0xc0a00100,0xc0a00580,0xc0a00584,7,&job));
        assert(pvr_render_batch_depth_job(width,height,&job));
        unsigned zx,zy;rogue_get_zls_tile_size_xy(dev,&zx,&zy);
        unsigned x=util_next_power_of_two(width),y=util_next_power_of_two(height);
        if(x<zx)x=zx;if(y<zy)y=zy;unsigned tx,ty;
        rogue_get_isp_num_tiles_xy(dev,1,x,y,&tx,&ty);
        struct ROGUE_CR_ISP_ZLSCTL zls;ROGUE_CR_ISP_ZLSCTL_unpack(job.fragment+136,&zls);
        assert(zls.zlsextent_x_z==tx-1 && zls.zlsextent_y_z==ty-1 && !zls.stencil_extent_enable);
        assert(zls.zloadformat==ROGUE_CR_ZLS_FORMAT_TYPE_F64Z && zls.zstoreformat==ROGUE_CR_ZLS_FORMAT_TYPE_F64Z);
        assert(zls.loadtwiddled && zls.storetwiddled && !zls.zloaden && !zls.sloaden && !zls.forcezload);
        assert(zls.zstoreen && zls.sstoreen && zls.forcezstore);
        struct ROGUE_CR_ISP_STENCIL_LOAD_BASE stencil;ROGUE_CR_ISP_STENCIL_LOAD_BASE_unpack(job.fragment+152,&stencil);
        assert(stencil.addr.addr==PVR_RENDER_DEPTH_ADDRESS && !stencil.enable);
        batch_job_reference(&job,x,y);
        struct pvr_render_texture_layout l;assert(pvr_render_texture_layout_profile(width,height,4,&l));
        assert(l.depth_bytes==x*y*8 && !(l.depth&4095) && l.draft==l.depth+l.depth_bytes+8192);
        assert(l.copy-l.draft>=PVR_RENDER_TEXTURE_GEOMETRY_BYTES+PVR_RENDER_TEXTURE_MAX_BYTES+PVR_RENDER_BATCH_MAX*PVR_RENDER_BATCH_BYTES);
        assert(pvr_render_texture_layout_profile(width,height,3,&l) && l.depth_bytes==PVR_RENDER_DEPTH_BYTES);
    }
}

static void put(float *v,float x,float y,float z,float w,float s,float t)
{ v[0]=x;v[1]=y;v[2]=z;v[3]=w;v[4]=s;v[5]=t; }
static void inputs(void)
{
    struct pvr_render_texture_input input={1,1,2,3,6};
    float vertices[18];uint16_t indices[6]={0,1,2,2,1,0};uint32_t frame=2;
    struct pvr_render_batch b[2]={{0,3,1|PVR_RENDER_BATCH_DEPTH_WRITE,0},
        {3,3,7|1u<<6|PVR_RENDER_BATCH_STENCIL|5u<<11|2u<<20,0xff0180}};
    put(vertices,-2,2,0,2,0,0);put(vertices+6,0.25f,-0.5f,0.5f,0.5f,-3,4);put(vertices+12,1,1,1,1,65536,-65536);
    assert(pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8));
    assert(!pvr_render_texture_input_valid_profile(4,&input,vertices,indices,8));
    assert(pvr_render_batch_table_bytes(4,frame)==32 && !pvr_render_batch_table_bytes(3,frame) && !pvr_render_batch_table_bytes(4,0));
    struct { unsigned at; float value; } bad[]={{0,-2.1f},{1,2.5f},{2,-0.25f},{2,2.5f},{3,0},{3,-1},{3,1.0f/131072},{3,131072},
        {4,65537},{5,-65537},{4,0.0f/0.0f},{3,1.0f/0.0f}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        float saved=vertices[bad[i].at];vertices[bad[i].at]=bad[i].value;
        assert(!pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8));vertices[bad[i].at]=saved;
    }
    vertices[2]=-0.0f;assert(pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8));vertices[2]=0;
    /* W bounds alone, with the position at the view-volume centre. */
    const float limits[][2]={{1.0f/65536,1},{65536,1},{1.0f/131072,0},{131072,0}};
    for(unsigned i=0;i<4;++i) {
        float saved[6];memcpy(saved,vertices+12,sizeof(saved));put(vertices+12,0,0,0,limits[i][0],0.5f,0.5f);
        assert(pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8)==(int)limits[i][1]);
        memcpy(vertices+12,saved,sizeof(saved));
    }
    indices[5]=3;assert(!pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8));indices[5]=0;
    assert(!pvr_render_batch_input_valid(&input,frame,vertices,indices,b,4));
    assert(!pvr_render_batch_input_valid(&input,0,vertices,indices,b,8));
    assert(!pvr_render_batch_input_valid(&input,17,vertices,indices,b,8));
    /* Each refused batch field. */
    const struct pvr_render_batch refused[]={{0,0,0,0},{0,4,0,0},{3,6,0,0},{7,3,0,0},{0,3,1u<<23,0},{0,3,3u<<4,0},
        {0,3,5u<<6,0},{0,3,1u<<6|PVR_RENDER_BATCH_NO_COLOR,0},{0,3,0,1},{0,3,1u<<11,0},{0,3,PVR_RENDER_BATCH_STENCIL,1u<<24}};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i) {
        assert(!pvr_render_batch_valid(refused+i,6));struct pvr_render_batch saved=b[1];b[1]=refused[i];
        assert(!pvr_render_batch_input_valid(&input,frame,vertices,indices,b,8));b[1]=saved;
    }
    struct pvr_render_batch whole={0,6,PVR_RENDER_BATCH_NO_COLOR|PVR_RENDER_BATCH_STENCIL|7u<<11|2u<<20,0xff00ff};
    assert(pvr_render_batch_valid(&whole,6));
    assert(pvr_render_texture_state_valid(4,frame) && !pvr_render_texture_state_valid(4,0) &&
        !pvr_render_texture_state_valid(4,1u<<13) && !pvr_render_texture_state_valid(4,5u<<6|1) &&
        pvr_render_texture_state_valid(4,16|4u<<6|4u<<9|PVR_RENDER_BATCH_LINEAR|PVR_RENDER_BATCH_SRGB));
    assert(!pvr_render_texture_state_valid(3,frame|16) && pvr_render_texture_state_valid(2,0));
}

int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(reference,1,sizeof(reference),f)==sizeof(reference));assert(!fclose(f));
    programs();commands();descriptors();depth();inputs();
    puts("PASS batch reference: exact resident programs, per-batch PPP/VDM, sampler modes, D32S8 state and input bounds");
    return 0;
}
