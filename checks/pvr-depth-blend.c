/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pvr_render_texture.h"
#include "pvr_render_job.h"
#include "common/device_info/bxm-4-64.h"
#include "hwdef/rogue_hw_utils.h"
#include "util/u_math.h"
#include <stdlib.h>
#include <stdio.h>
#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(a) (a).addr
#define __pvr_make_address(a) PVR_DEV_ADDR(a)
#include "csbgen/rogue/cr.h"
#include "csbgen/rogue/ppp.h"
void depth_job_reference(const struct pvr_render_job_image *,unsigned,unsigned);
static unsigned char payload[PVR_RENDER_PROGRAM_BYTES],reference[PVR_RENDER_PROGRAM_BYTES];
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(reference,1,sizeof(reference),f)==sizeof(reference));assert(!fclose(f));
    struct pvr_render_program_image programs;
    assert(pvr_render_texture_programs_build_profile(payload,sizeof(payload),1920,1080,3,&programs));
    assert(!memcmp(payload+4096+3072,reference+4096+3072,376));
    assert(!memcmp(payload+2560,reference+2560,36));
    assert(3072+376<=4096 && 2560+36<=4096);
    const uint32_t *ppp=(const void *)(payload+8192+0xf100);
    for(unsigned state=0;state<16;++state) {
        uint32_t before[26];memcpy(before,ppp,sizeof(before));
        if(!(state&3) && (state&4)) {
            assert(!pvr_render_texture_state_apply(payload,state));assert(!memcmp(before,ppp,sizeof(before)));continue;
        }
        assert(pvr_render_texture_state_apply(payload,state));
        struct ROGUE_TA_STATE_ISPA a;ROGUE_TA_STATE_ISPA_unpack(ppp+2,&a);
        const unsigned compare[]={ROGUE_TA_CMPMODE_ALWAYS,ROGUE_TA_CMPMODE_LESS,
            ROGUE_TA_CMPMODE_LESS_OR_EQUAL,ROGUE_TA_CMPMODE_ALWAYS};
        assert(a.dcmpmode==compare[state&3] && a.dwritedisable==!(state&PVR_RENDER_DEPTH_WRITE));
        assert(a.passtype==((state&PVR_RENDER_SOURCE_OVER)?ROGUE_TA_PASSTYPE_TRANSLUCENT:ROGUE_TA_PASSTYPE_OPAQUE));
        struct ROGUE_TA_STATE_PDS_SHADERBASE shader;ROGUE_TA_STATE_PDS_SHADERBASE_unpack(ppp+5,&shader);
        assert(shader.addr.addr==((state&PVR_RENDER_SOURCE_OVER)?2560u:2304u));
    }
    assert(!pvr_render_texture_state_apply(payload,16));
    struct pvr_render_job_image job;
    assert(!pvr_render_job_build_frame_pr(&programs,0xc0a00100,0xc0a00580,0xc0a00584,1,NULL));
    assert(!pvr_render_job_build_frame_pr(&programs,0xc0a00100,0xc0a00580,0xc0a00584,1,(void *)&programs));
    const unsigned shapes[][2]={{1,1},{31,33},{64,64},{1280,720},{1919,1079},{1920,1080}};
    for(unsigned i=0;i<sizeof(shapes)/sizeof(shapes[0]);++i) {
        unsigned width=shapes[i][0],height=shapes[i][1];
        assert(pvr_render_job_build_frame_pr(&programs,0xc0a00100,0xc0a00580,0xc0a00584,7,&job));
        assert(pvr_render_texture_depth_job(width,height,&job));
        unsigned zx,zy;const struct pvr_device_info *dev=&pvr_device_info_36_56_104_183;
        rogue_get_zls_tile_size_xy(dev,&zx,&zy);
        unsigned x=util_next_power_of_two(width),y=util_next_power_of_two(height);
        if(x<zx)x=zx;if(y<zy)y=zy;unsigned tx,ty;
        rogue_get_isp_num_tiles_xy(dev,1,x,y,&tx,&ty);
        struct ROGUE_CR_ISP_ZLSCTL zls;ROGUE_CR_ISP_ZLSCTL_unpack(job.fragment+136,&zls);
        assert(zls.zlsextent_x_z==tx-1 && zls.zlsextent_y_z==ty-1);
        assert(zls.zloadformat==ROGUE_CR_ZLS_FORMAT_TYPE_F32Z && zls.zstoreformat==ROGUE_CR_ZLS_FORMAT_TYPE_F32Z);
        assert(zls.loadtwiddled && zls.storetwiddled && !zls.zloaden && !zls.forcezload && zls.zstoreen && zls.forcezstore);
        assert(!zls.sloaden && !zls.sstoreen);depth_job_reference(&job,x,y);
        struct pvr_render_texture_layout l;assert(pvr_render_texture_layout_profile(width,height,3,&l));
        assert(l.draft==l.depth+PVR_RENDER_DEPTH_BYTES+8192 && !(l.depth&4095));
    }
    struct pvr_render_texture_input input={1,1,2,3,3};
    float vertices[18]={0,0,0.5f,1,0,0, 0,0,0.25f,1,0,0, 0,0,1,1,0,0};uint16_t indices[]={0,1,2};
    assert(pvr_render_texture_input_valid_profile(3,&input,vertices,indices,8));
    assert(!pvr_render_texture_input_valid_profile(2,&input,vertices,indices,8));
    vertices[2]=-0.1f;assert(!pvr_render_texture_input_valid_profile(3,&input,vertices,indices,8));
    puts("PASS depth/blend: exact resident compiler programs, PPP state, firmware depth/UPDATE layout, padded extents, resource and input bounds");
}
