/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Timing arithmetic adapted from the A733 display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016 Allwinner.
 * Writable fields follow A733 User Manual V1.00 section15.2.10; removed
 * legacy fields in the published C bitfield structures are not written. */
#include "a733_tcon_tv.h"

int a733_tcon_tv_init(struct a733_tcon_tv *t,struct a733_hdmi_io *b){
    if(!t || t->self || !b || b->self!=b ||
       a733_hdmi_timing(b->request.mode,&t->timing)!=1)return -1;
    t->self=t;t->io=b;return 1;
}
static int valid(struct a733_tcon_tv *t){
    return t && t->self==t && t->io && t->io->self==t->io && !t->io->fault;
}
int a733_tcon_tv_prepare(struct a733_tcon_tv *t){
    struct a733_hdmi_step s;const struct a733_hdmi_timing *v;
    uint32_t delay;
    if(!valid(t) || t->started || t->stopped)return -1;
    if(t->prepared)return 1;
    v=&t->timing;delay=v->vtotal-v->height-5;if(delay>31)delay=31;
    s=(struct a733_hdmi_step){A733_HDMI_TV_BASE,0,0,4};
    switch(t->index){
    case 0:s.address+=0x90;s.mask=0x800ffff0;s.value=delay<<4;break;
    case 1:s.mask=0x31;break; /* strict, one pixel/clock; no legacy global enable */
    case 2:s.address+=0x40;s.mask=7;s.value=t->io->request.source==A733_HDMI_FRAMEBUFFER?0u:1u;break; /* DE or color-check source */
    case 3:s.address+=0x98;s.mask=0x8001ffff;s.value=2u*v->vtotal;break;
    case 4:s.address+=0x9c;s.mask=0x7fff7fff;s.value=((uint32_t)(v->width-1)<<16)|(v->height-1);break;
    case 5:s.address+=0xa0;s.mask=0xffff7fff;s.value=((uint32_t)(v->htotal-1)<<16)|(v->hsync+v->hback-1);break;
    case 6:s.address+=0xa4;s.mask=0x7fff;s.value=v->vsync+v->vback-1;break;
    case 7:s.address+=0xa8;s.mask=0x0fff0fff;s.value=((uint32_t)(v->hsync-1)<<16)|(v->vsync-1);break;
    case 8:s.address+=0x88;s.mask=0x0f000000;s.value=0x07000000;break;
    case 9:s.address+=0x8c;s.mask=0x0f000000;s.value=0x0f000000;break;
    case 10:s.address+=0x300;s.mask=0x80000000;break; /* no 3D fill */
    case 11:s.address+=0x330;s.mask=0xffffffff;break;
    case 12:s.address+=0x334;s.mask=0xffff;break;
    case 13:s.address+=0x338;s.mask=0xffffffff;break;
    case 14:s.address+=0x33c;s.mask=0xffff;break;
    case 15:s.address+=0x340;s.mask=7;s.value=1;break; /* eight-bit colorbar */
    case 16:s.address=A733_HDMI_TOP_BASE;s.mask=0xa;break;
    case 17:s.address=A733_HDMI_TOP_BASE+0x20;s.mask=0x10100000;s.value=0x10100000;break;
    default:return a733_hdmi_fail(t->io);
    }
    if(a733_hdmi_field(t->io,&s)!=1)return -1;
    if(++t->index==18){
        if(a733_hdmi_tick(t->io)!=1)return -1;
        t->prepared=1;return 1;}
    return 0;
}
int a733_tcon_tv_start(struct a733_tcon_tv *t){
    struct a733_hdmi_step s={A733_HDMI_TV_BASE+0x90,0x80000000,0x80000000,4};
    if(!valid(t) || !t->prepared || t->started || t->stopped)return -1;
    if(a733_hdmi_field(t->io,&s)!=1 || a733_hdmi_tick(t->io)!=1)return -1;
    t->started=1;return 1;
}
int a733_tcon_tv_stop(struct a733_tcon_tv *t){
    struct a733_hdmi_step s={A733_HDMI_TV_BASE+0x90,0x80000000,0,4};
    if(!valid(t) || !t->prepared || t->stopped)return -1;
    if(a733_hdmi_field(t->io,&s)!=1 || a733_hdmi_tick(t->io)!=1)return -1;
    t->stopped=1;t->started=0;return 1;
}
int a733_tcon_tv_health(struct a733_tcon_tv *t){
    uint32_t x;
    if(!valid(t) || !t->started || t->stopped)return -1;
    if(a733_hdmi_read(t->io,A733_HDMI_TV_BASE+0xfc,4,&x)!=1)return -1;
    /* Underflow and lost-data warning are R/W0C; preserve them as evidence. */
    return (x&0x60000000)?0:1;
}
