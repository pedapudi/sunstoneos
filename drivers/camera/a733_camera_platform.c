/* SPDX-License-Identifier: GPL-2.0-only */
/* Register definitions and analog enable order adapted from the A733 driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2019 by Allwinnertech Co., Ltd.
 * Authors: Zheng Zequn <zequnzheng@allwinnertech.com>
 */
#include "a733_camera_platform.h"
#include "a733_camera_common.h"
#include <stddef.h>

#define TOP_MASK UINT32_C(0x67001f0f)
#define CAL_RW UINT32_C(0x1f7)
static const uint16_t observations[]={0,4,0x100,0x200,0x300,0x1000,0x1400,0x1800};
struct step { uint16_t off;uint32_t mask,value; };
static const struct step setup[]={
    {0,TOP_MASK,0xb00}, /* Internal reference, link0, PRBS0, held in reset. */
    {4,CAL_RW,0x102}, /* SET=16, SOFT=1, AUTO=0; retain RESETN unchanged. */
    {0,1,1}, /* Analog power reset released before digital reset. */
    {0,2,2},
    {0,4,4}, /* Reference voltage before LDO. */
    {0,8,8}
};
static int fault(struct a733_camera_platform *s){s->state=A733_CAMERA_PLATFORM_FAULT;return -1;}
static int tick(struct a733_camera_platform *s){
    return a733_camera_clock(s->ops.now_us,s->context,&s->now,s->deadline)?1:fault(s);
}
static int initialize(struct a733_camera_platform *s,
    const struct a733_camera_platform_ops *o,void *ctx,uint32_t proof){
    if(!s || !o || s->initialized || s->state!=A733_CAMERA_PLATFORM_NEW ||
       a733_ranges_overlap_or_wrap(s,sizeof(*s),o,sizeof(*o)) ||
       (ctx && a733_ranges_overlap_or_wrap(s,sizeof(*s),ctx,1)) ||
       !o->now_us || !o->admit || !o->read || !o->write)return -1;
    s->ops=*o;s->context=ctx;s->required_proof=proof;s->initialized=1;return 1;
}
int a733_camera_platform_init(struct a733_camera_platform *s,const struct a733_camera_platform_ops *o,void *ctx)
{return initialize(s,o,ctx,A733_CAMERA_REQUIRED_PROOF);}
int a733_camera_platform_supervised_init(struct a733_camera_platform *s,const struct a733_camera_platform_ops *o,void *ctx)
{return initialize(s,o,ctx,A733_CAMERA_SUPERVISED_PROOF);}
int a733_camera_platform_open(struct a733_camera_platform *s,uint64_t d){
    if(!s || !s->initialized || s->state!=A733_CAMERA_PLATFORM_NEW || !d || d<=s->now)return -1;
    s->deadline=d;s->state=A733_CAMERA_PLATFORM_ADMIT;return 1;
}
static int rd(struct a733_camera_platform *s,uint16_t off,uint32_t *v){
    if(s->ops.read(s->context,s->admission.token,off,v,s->deadline)!=1 || tick(s)!=1)return fault(s);
    return 1;
}
static int wr(struct a733_camera_platform *s,uint16_t off,uint32_t v){
    if(s->ops.write(s->context,s->admission.token,off,v,s->deadline)!=1 || tick(s)!=1)return fault(s);
    return 1;
}
static int program(struct a733_camera_platform *s){
    const struct step *p=&setup[s->index];uint32_t v,value=p->value;
    if(p->off==4)value|=s->observed[1]&4;
    if(s->phase==0){
        /* Calibration writes contain only R/W bits. RESULT and FLAG are read
         * only; the manual defines their reflection in software mode.
         */
        if(p->off==4){s->saved=value;s->phase=1;return 0;}
        if(rd(s,p->off,&v)!=1)return -1;
        s->saved=(v&~p->mask)|value;s->phase=1;return 0;
    }
    if(s->phase==1){if(wr(s,p->off,s->saved)!=1)return -1;s->phase=2;return 0;}
    if(rd(s,p->off,&v)!=1)return -1;
    if((v&p->mask)!=value)return fault(s);
    s->phase=0;
    if(++s->index==sizeof(setup)/sizeof(setup[0]))s->state=A733_CAMERA_PLATFORM_CALIBRATE;
    return 0;
}
int a733_camera_platform_poll(struct a733_camera_platform *s){
    uint32_t v;int r;
    if(!s || !s->initialized || s->state==A733_CAMERA_PLATFORM_FAULT)return -1;
    if(s->state==A733_CAMERA_PLATFORM_ACTIVE)return 1;
    if(s->state==A733_CAMERA_PLATFORM_NEW)return -1;
    if(tick(s)!=1)return -1;
    switch(s->state){
    case A733_CAMERA_PLATFORM_ADMIT:{
        struct a733_camera_admission a=s->admission;
        r=s->ops.admit(s->context,s->deadline,&a);
        if(s->admission.token && s->admission.token!=a.token)return fault(s);
        s->admission=a;
        if(r<0 || r>1 || (r && !a.token) || tick(s)!=1)return fault(s);
        if(!r)return 0;
        if(a.proof!=s->required_proof ||
           (a.origin!=A733_CAMERA_COLD_RESET && a.origin!=A733_CAMERA_RESOLVED_RESET_HANDOFF))return fault(s);
        s->state=A733_CAMERA_PLATFORM_CHECK;return 0;
    }
    case A733_CAMERA_PLATFORM_CHECK:
        if(rd(s,observations[s->index],&v)!=1)return -1;
        s->observed[s->index]=v;
        if((s->index==0 && (v&UINT32_C(0x6700100f))) ||
           (s->index>=2 && s->index<=4 && (v&UINT32_C(0x040001f1))) ||
           (s->index>=5 && (v&1)))return fault(s);
        if(++s->index==sizeof(observations)/sizeof(observations[0])){
            s->index=0;s->state=A733_CAMERA_PLATFORM_PROGRAM;
        }return 0;
    case A733_CAMERA_PLATFORM_PROGRAM:return program(s);
    case A733_CAMERA_PLATFORM_CALIBRATE:
        if(rd(s,4,&v)!=1)return -1;
        s->calibration=v;
        if((v&CAL_RW)!=(0x102u|(s->observed[1]&4)))return fault(s);
        if((v&UINT32_C(0x1f0008))!=UINT32_C(0x100008))return 0;
        s->state=A733_CAMERA_PLATFORM_VERIFY;return 0;
    case A733_CAMERA_PLATFORM_VERIFY:
        if(rd(s,0,&v)!=1)return -1;
        if((v&TOP_MASK)!=0xb0f)return fault(s);
        s->state=A733_CAMERA_PLATFORM_ACTIVE;return 1;
    default:return fault(s);
    }
}
