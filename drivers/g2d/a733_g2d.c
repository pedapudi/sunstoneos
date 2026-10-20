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
    struct g2d_mixer_write_back_reg *w=(void *)(p+832);
    unsigned size=(s->w-1)|((s->h-1)<<16);
    v->ovl_attr.dwval=UINT32_C(0xff000001)|(s->op==1?16:0);
    v->ovl_mem.dwval=size;v->ovl_winsize.dwval=size;
    v->ovl_mem_pitch0=s->stride;
    v->ovl_mem_low_addr0=A733_G2D_SOURCE_IOVA+s->sy*s->stride+s->sx*4;
    v->ovl_fill_color=s->color;
    b->bld_en_ctrl.dwval=0x101;b->mem_size[0].dwval=size;
    b->out_size.dwval=size;b->bld_ctrl.dwval=0x00010001; /* COPY */
    b->rop_ctrl.dwval=0xf0;b->ch3_index0.dwval=0x41000;
    w->data_size.dwval=size;w->pitch0=s->stride;
    w->laddr0=A733_G2D_OUTPUT_IOVA+s->dy*s->stride+s->dx*4;
    block(p,0,128,sizeof(*v),G2D_V0);
    block(p,1,192,4,G2D_UI0);block(p,2,224,4,G2D_UI1);
    block(p,3,256,4,G2D_UI2);block(p,4,288,4,G2D_VSU);
    block(p,5,384,sizeof(*b),G2D_BLD);block(p,6,832,sizeof(*w),G2D_WB);
    /* Eighth header is the required even-count zero-dirty terminator. */
    return 1;
}
