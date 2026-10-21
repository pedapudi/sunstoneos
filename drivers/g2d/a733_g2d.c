/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Copyright (c) 2007-2019 Allwinnertech Co., Ltd.
 * Author: zhengxiaobin <zhengxiaobin@allwinnertech.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* Bounded FreeRTOS RCQ adaptation; source pins: apps/g2d/sources.json. */
#include "a733_g2d.h"
#include "g2d_mixer_type.h"
#include <string.h>
/* Fixed 2x RGB enlargement uses upstream coefficient set1 (step=1/2).
 * Only these 32 phases are required; vertical filtering uses the RGB linear set.
 * Source: pinned g2d_scal.c, g2d_vsu_calc_fir_coef/g2d_vsu_para_set. */
static const uint32_t scale2x_h[32]={
    0x00004000,0x000140ff,0x00033ffe,0x00043ffd,
    0x00063efc,0xff083dfc,0x000a3bfb,0xff0d39fb,
    0xff0f37fb,0xff1136fa,0xfe1433fb,0xfe1631fb,
    0xfd192ffb,0xfd1c2cfb,0xfd1f29fb,0xfc2127fc,
    0xfc2424fc,0xfc2721fc,0xfb291ffd,0xfb2c1cfd,
    0xfb2f19fd,0xfb3116fe,0xfb3314fe,0xfa3611ff,
    0xfb370fff,0xfb390dff,0xfb3b0a00,0xfc3d08ff,
    0xfc3e0600,0xfd3f0400,0xfe3f0300,0xff400100,
};
static const uint32_t scale2x_v[32]={
    0x00004000,0x00023e00,0x00043c00,0x00063a00,
    0x00083800,0x000a3600,0x000c3400,0x000e3200,
    0x00103000,0x00122e00,0x00142c00,0x00162a00,
    0x00182800,0x001a2600,0x001c2400,0x001e2200,
    0x00202000,0x00221e00,0x00241c00,0x00261a00,
    0x00281800,0x002a1600,0x002c1400,0x002e1200,
    0x00301000,0x00320e00,0x00340c00,0x00360a00,
    0x00380800,0x003a0600,0x003c0400,0x003e0200,
};
struct head { uint32_t low,len,dirty,offset; };
_Static_assert(sizeof(struct head)==16,"RCQ descriptor");
static void block(uint8_t *memory,unsigned h,unsigned at,unsigned size,unsigned offset)
{
    struct head *head=(void *)memory;
    head[h]=(struct head){A733_G2D_RCQ_IOVA+at,size,1,offset};
}
int a733_g2d_commands(void *memory,size_t capacity,const struct a733_g2d_session *s)
{
    if(!memory||(uintptr_t)memory%32||capacity!=4096||!a733_g2d_geometry_valid(s))return 0;
    memset(memory,0,capacity);uint8_t *p=memory;
    struct g2d_mixer_ovl_v_reg *v=(void *)(p+128);
    struct g2d_mixer_bld_reg *b=(void *)(p+384);
    struct g2d_mixer_ovl_u_reg *u=(void *)(p+256);
    struct g2d_mixer_write_back_reg *w=(void *)(p+832);
    unsigned size=(s->w-1)|((s->h-1)<<16);
    unsigned source_size=s->op==A733_G2D_SCALE2X?
        ((s->w/2-1)|((s->h/2-1)<<16)):size;
    v->ovl_attr.dwval=UINT32_C(0xff000001)|(s->op==1?16:0);
    v->ovl_mem.dwval=source_size;v->ovl_winsize.dwval=source_size;
    v->ovl_mem_pitch0=s->stride;
    v->ovl_mem_low_addr0=A733_G2D_SOURCE_IOVA+s->sy*s->stride+s->sx*4;
    v->ovl_fill_color=s->color;
    b->bld_en_ctrl.dwval=0x101;b->mem_size[0].dwval=size;
    b->out_size.dwval=size;b->bld_ctrl.dwval=0x00010001; /* COPY */
    b->rop_ctrl.dwval=0xf0;b->ch3_index0.dwval=0x41000;
    if(s->op==A733_G2D_SRCOVER_SOLID){
        /* Effective g2d_bsp_bld: V0/pipe0 source, UI2/pipe1 destination.
         * G2D_PIXEL_ALPHA and bpremul=0 preserve straight per-pixel alpha.
         * UI2 is a constant opaque color, so it issues no buffer reads. */
        _Static_assert(sizeof(*u)==32,"UI2 RCQ slot ends before VSU");
        u->ovl_attr.dwval=UINT32_C(0xff000011);
        u->ovl_mem.dwval=size;u->ovl_winsize.dwval=size;
        u->ovl_mem_pitch0=s->stride;u->ovl_fill_color=s->color;
        b->bld_en_ctrl.dwval=0x301;b->mem_size[1].dwval=size;
        b->bld_ctrl.dwval=0x03010301; /* SRCOVER; straight output. */
    }
    if(s->op==A733_G2D_SCALE2X){
        struct g2d_mixer_video_scaler_reg *vs=(void *)(p+1024);
        _Static_assert(1024+sizeof(*vs)<=4096,"VSU fits retained RCQ extent");
        /* Same RGB branch and filter bank as effective g2d_vsu_para_set.
         * Coarse downsampling stays disabled for this enlargement ratio. */
        vs->vs_ctrl.dwval=0x101;vs->out_size.dwval=size;
        vs->glb_alpha.dwval=255;
        vs->y_ch_size.dwval=source_size;vs->c_ch_size.dwval=source_size;
        vs->y_hor_step.dwval=vs->y_ver_step.dwval=0x80000;
        vs->c_hor_step.dwval=vs->c_ver_step.dwval=0x80000;
        memcpy(vs->vs_y_ch_hor_filter_coef,scale2x_h,sizeof(scale2x_h));
        memcpy(vs->vs_c_ch_hor_filter_coef,scale2x_h,sizeof(scale2x_h));
        memcpy(vs->vs_y_ch_ver_filter_coef,scale2x_v,sizeof(scale2x_v));
    }
    w->data_size.dwval=size;w->pitch0=s->stride;
    w->laddr0=A733_G2D_OUTPUT_IOVA+s->dy*s->stride+s->dx*4;
    block(p,0,128,sizeof(*v),G2D_V0);
    block(p,1,192,4,G2D_UI0);block(p,2,224,4,G2D_UI1);
    block(p,3,256,s->op==A733_G2D_SRCOVER_SOLID?sizeof(*u):4,G2D_UI2);block(p,4,s->op==A733_G2D_SCALE2X?1024:288,
        s->op==A733_G2D_SCALE2X?sizeof(struct g2d_mixer_video_scaler_reg):4,G2D_VSU);
    block(p,5,384,sizeof(*b),G2D_BLD);block(p,6,832,sizeof(*w),G2D_WB);
    /* Eighth header is the required even-count zero-dirty terminator. */
    return 1;
}
