/* SPDX-License-Identifier: MIT */
#include "pvr_render_texture.h"
#include "pvr_gpu_vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __pvr_address_type uint64_t
#define __pvr_get_address(a) (a)
#define __pvr_make_address(a) (a)
#include "csbgen/rogue/texstate.h"
#include "csbgen/rogue/pbestate.h"
#include "csbgen/rogue/ppp.h"
#include "csbgen/rogue/vdm.h"
static unsigned char payload[PVR_RENDER_PROGRAM_BYTES],reference[PVR_RENDER_PROGRAM_BYTES];
static void descriptor(uint32_t width,uint32_t height,uint32_t stride)
{
    unsigned char actual[80];struct pvr_render_texture_input input={width,height,stride,4,6};
    assert(pvr_render_texture_descriptor(actual,&input));
    struct ROGUE_TEXSTATE_IMAGE_WORD0 image0;struct ROGUE_TEXSTATE_STRIDE_IMAGE_WORD1 image1;
    ROGUE_TEXSTATE_IMAGE_WORD0_unpack(actual,&image0);ROGUE_TEXSTATE_STRIDE_IMAGE_WORD1_unpack(actual+8,&image1);
    assert(image0.width==width-1 && image0.height==height-1 && image0.textype==ROGUE_TEXSTATE_TEXTYPE_STRIDE &&
        image0.texformat==ROGUE_TEXSTATE_FORMAT_U8U8U8U8 && !image0.smpcnt && !image0.gamma);
    assert(image0.swiz0==0 && image0.swiz1==1 && image0.swiz2==2 && image0.swiz3==3);
    assert(image1.stride==stride-1 && image1.texaddr==PVR_RENDER_TEXTURE_IMAGE_ADDRESS &&
        image1.num_mip_levels==1 && !image1.mipmaps_present && image1.alpha_msb);
    uint32_t layer,pck;memcpy(&layer,actual+16,4);memcpy(&pck,actual+28,4);assert(layer==stride*height*4 && pck==320);
    struct ROGUE_TEXSTATE_SAMPLER_WORD0 sampler;
    ROGUE_TEXSTATE_SAMPLER_WORD0_unpack(actual+32,&sampler);
    assert(sampler.magfilter==ROGUE_TEXSTATE_FILTER_POINT && sampler.minfilter==ROGUE_TEXSTATE_FILTER_POINT &&
        sampler.addrmode_u==ROGUE_TEXSTATE_ADDRMODE_CLAMP_TO_EDGE && sampler.addrmode_v==ROGUE_TEXSTATE_ADDRMODE_CLAMP_TO_EDGE &&
        !sampler.mipfilter && !sampler.non_normalized_coords && !sampler.minlod && !sampler.maxlod);
}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(reference,1,sizeof(reference),f)==sizeof(reference));assert(!fclose(f));
    struct pvr_render_program_image image;
    assert(pvr_render_texture_programs_build(payload,sizeof(payload),64,64,&image));
    const unsigned spans[][2]={{0,4096},{4096+1792,72},{4096+2304,104},{4096+2560,232},
        {8192+0xf000,44},{8192+0xf100,104},{8192+0x10040,4},{8192+0x10060,8}};
    for(unsigned i=0;i<sizeof(spans)/sizeof(spans[0]);++i)assert(!memcmp(payload+spans[i][0],reference+spans[i][0],spans[i][1]));
    struct pvr_render_texture_input input={64,64,64,4,6};unsigned char state[80];
    assert(pvr_render_texture_descriptor(state,&input));assert(!memcmp(state,reference+8192+0x10100,80));
    const unsigned shapes[][2]={{1,1},{64,64},{1280,720},{1920,1080},{1919,1079}};
    for(unsigned i=0;i<sizeof(shapes)/sizeof(shapes[0]);++i) {
        unsigned width=shapes[i][0],height=shapes[i][1],stride,bytes;
        assert(pvr_render_texture_geometry(width,height,&stride,&bytes));descriptor(width,height,stride);
        assert(pvr_render_texture_programs_build(payload,sizeof(payload),width,height,&image));
        struct ROGUE_PBESTATE_REG_WORD0 pbe0;struct ROGUE_PBESTATE_REG_WORD1 pbe1;
        ROGUE_PBESTATE_REG_WORD0_unpack(image.pbe,&pbe0);ROGUE_PBESTATE_REG_WORD1_unpack(image.pbe+1,&pbe1);
        assert(pbe0.linestride==(stride-1)/ROGUE_PBESTATE_REG_WORD0_LINESTRIDE_UNIT_SIZE &&
            pbe1.maxclip_x==width-1 && pbe1.maxclip_y==height-1);
        const uint32_t *ppp=(const void *)(payload+8192+0xf100);
        struct ROGUE_TA_REGION_CLIP0 x;struct ROGUE_TA_REGION_CLIP1 y;
        ROGUE_TA_REGION_CLIP0_unpack(ppp+11,&x);ROGUE_TA_REGION_CLIP1_unpack(ppp+12,&y);
        assert(x.right==(width+15)/16-1 && y.bottom==(height+15)/16-1);
        float vx,vy;memcpy(&vx,ppp+13,4);memcpy(&vy,ppp+15,4);assert(vx==width*0.5f && vy==height*0.5f);
        struct pvr_render_texture_layout l;assert(pvr_render_texture_layout(width,height,&l));
        assert(l.output_bytes==bytes+128 && !(l.total&4095) && l.copy>=l.draft+PVR_RENDER_TEXTURE_GEOMETRY_BYTES+PVR_RENDER_TEXTURE_MAX_BYTES);
    }
    struct ROGUE_VDMCTRL_INDEX_LIST0 list;ROGUE_VDMCTRL_INDEX_LIST0_unpack(reference+8192+0xf01c,&list);
    assert(list.index_addr_present && list.index_size==ROGUE_VDMCTRL_INDEX_SIZE_B16 && list.index_count_present);
    struct ROGUE_VDMCTRL_INDEX_LIST1 address;ROGUE_VDMCTRL_INDEX_LIST1_unpack(reference+8192+0xf020,&address);
    assert((list.index_base_addrmsb|address.index_base_addrlsb)==PVR_RENDER_TEXTURE_INDEX_ADDRESS);
    uint32_t count;memcpy(&count,reference+8192+PVR_RENDER_TEXTURE_COUNT_OFFSET,4);assert(count==3);
    unsigned char *vertices=calloc(PVR_RENDER_TEXTURE_VERTEX_BYTES,1);assert(vertices);
    for(unsigned i=0;i<65536;++i){uint32_t one=0x3f800000;memcpy(vertices+i*24+12,&one,4);}
    uint16_t indices[]={0,1,65535};input=(struct pvr_render_texture_input){1,1,2,65536,3};
    assert(pvr_render_texture_input_valid(&input,vertices,indices,8));
    input.vertex_count=4;assert(!pvr_render_texture_input_valid(&input,vertices,indices,8));
    indices[2]=3;assert(pvr_render_texture_input_valid(&input,vertices,indices,8));
    uint32_t nan=0x7fc00000;memcpy(vertices+16,&nan,4);assert(!pvr_render_texture_input_valid(&input,vertices,indices,8));
    free(vertices);puts("PASS texture reference: exact pinned shader/PDS bytes, complete descriptors, indexed layout, PBE/PPP geometry and register/fetch bounds");return 0;
}
