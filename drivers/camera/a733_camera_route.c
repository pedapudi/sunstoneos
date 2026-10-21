/* SPDX-License-Identifier: GPL-2.0-only */
/* Register and parameter definitions adapted from the A733 camera drivers.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2007-2021 Allwinnertech Co., Ltd.
 * Authors: Zheng ZeQun <zequnzheng@allwinnertech.com>
 */
#include "a733_camera_route.h"
#include "a733_camera_common.h"
#include <stddef.h>
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
struct step { uint16_t off;uint8_t region,kind;uint32_t mask,value; };
/* kind 1 parser selection; other kinds are the shared VIPP step kinds. */
static const struct step top[]={
    {0x040,A733_ROUTE_TOP,1,0xf,0},
    {0x0a0,A733_ROUTE_TOP,0,0xf,4}
};
/* TOP parser/ISP selection, then the shared VIPP0 enable order. */
static struct step configure(unsigned i){
    if(i<COUNT(top))return top[i];
    const struct a733_vipp_step *e=&a733_vipp_enable[i-COUNT(top)];
    return (struct step){e->off,A733_ROUTE_VIPP,e->kind,e->mask,e->value};
}
#define CONFIGURE_STEPS (COUNT(top)+A733_VIPP_ENABLE_STEPS)
static const struct step disable[]={
    {0x040,A733_ROUTE_VIPP,0,5,0},
    {0x000,A733_ROUTE_VIPP,0,3,0}
};
static int fault(struct a733_camera_route *s){s->state=A733_ROUTE_FAULT;return -1;}
static int tick(struct a733_camera_route *s){
    return a733_camera_clock(s->ops.now_us,s->context,&s->now,s->deadline)?1:fault(s);
}
int a733_camera_route_init(struct a733_camera_route *s,const struct a733_route_ops *o,
                          void *ctx,const struct a733_route_config *c){
    if(!s || !o || !c || s->initialized || s->state!=A733_ROUTE_NEW || c->parser>1 ||
       !c->parameter_address || (c->parameter_address&63) || c->mapped_bytes<1024 ||
       c->parameter_address>UINT32_MAX || c->mapped_bytes>(UINT64_C(1)<<32)-c->parameter_address ||
       a733_camera_overlap(s,sizeof(*s),o,sizeof(*o)) ||
       a733_camera_overlap(s,sizeof(*s),c,sizeof(*c)) ||
       (ctx && a733_camera_overlap(s,sizeof(*s),ctx,1)) || !o->now_us || !o->admit ||
       !o->publish || !o->read || !o->write || !o->producer_stop || !o->drain ||
       !o->release)return -1;
    s->ops=*o;s->config=*c;s->context=ctx;s->initialized=1;
    a733_vipp_unity_parameters(s->parameters);
    return 1;
}
static int begin(struct a733_camera_route *s,enum a733_route_state state,uint64_t d){
    if(!d || d<=s->now)return -1;
    s->state=state;s->deadline=d;s->index=0;s->phase=0;return 1;
}
int a733_camera_route_open(struct a733_camera_route *s,uint64_t d){
    if(!s || !s->initialized || s->state!=A733_ROUTE_NEW)return -1;
    return begin(s,A733_ROUTE_ADMIT,d);
}
int a733_camera_route_finish(struct a733_camera_route *s,uint64_t d){
    if(!s || !s->initialized || s->state!=A733_ROUTE_ARMED)return -1;
    return begin(s,A733_ROUTE_STOP,d);
}
static int rd(struct a733_camera_route *s,unsigned reg,uint16_t off,uint32_t *v){
    if(s->ops.read(s->context,s->token,(enum a733_route_region)reg,off,v,s->deadline)!=1 || tick(s)!=1)return fault(s);
    return 1;
}
static int wr(struct a733_camera_route *s,unsigned reg,uint16_t off,uint32_t v){
    if(s->ops.write(s->context,s->token,(enum a733_route_region)reg,off,v,s->deadline)!=1 || tick(s)!=1)return fault(s);
    return 1;
}
static int program(struct a733_camera_route *s,const struct step *e){
    uint32_t v,value=e->value,mask=e->mask;
    if(e->kind==1)value=(uint32_t)s->config.parser*4;
    if(e->kind==A733_VIPP_ADDRESS)value=(uint32_t)(s->config.parameter_address>>2);
    if(e->kind==A733_VIPP_W1C){
        if(!s->phase){if(wr(s,e->region,e->off,value)!=1)return -1;s->phase=1;return 0;}
        if(rd(s,e->region,e->off,&v)!=1)return -1;
        if(v)return fault(s);
    }else{
        if(!s->phase){
            if(rd(s,e->region,e->off,&v)!=1)return -1;
            s->saved=(v&~mask)|(value&mask);s->phase=1;return 0;
        }
        if(s->phase==1){if(wr(s,e->region,e->off,s->saved)!=1)return -1;s->phase=2;return 0;}
        if(rd(s,e->region,e->off,&v)!=1)return -1;
        if(e->kind==A733_VIPP_READY)mask=A733_VIPP_READY_VERIFY;
        /* Capture disable may complete at the current frame boundary. Keep
         * polling its readback until clear without repeating the write.
         */
        if(s->state==A733_ROUTE_DISABLE && s->index==0 && (v&1))return 0;
        if((v&mask)!=(value&mask))return fault(s);
    }
    s->phase=0;++s->index;return 0;
}
static int call(struct a733_camera_route *s,int (*fn)(void *,uint64_t,uint64_t),enum a733_route_state next){
    int r=fn(s->context,s->token,s->deadline);
    if(r<0 || r>1 || tick(s)!=1)return fault(s);
    if(r){s->state=next;s->index=0;s->phase=0;}return 0;
}
int a733_camera_route_poll(struct a733_camera_route *s){
    uint64_t token;uint32_t v;int r;
    if(!s || !s->initialized || s->state==A733_ROUTE_FAULT)return -1;
    if(s->state==A733_ROUTE_ARMED || s->state==A733_ROUTE_CLOSED)return 1;
    if(s->state==A733_ROUTE_NEW)return -1;
    if(tick(s)!=1)return -1;
    switch(s->state){
    case A733_ROUTE_ADMIT:
        token=s->token;r=s->ops.admit(s->context,&s->config,s->deadline,&token);
        r=a733_camera_adopt(&s->token,token,r);
        if(r<0 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_ROUTE_CHECK;
        return 0;
    case A733_ROUTE_CHECK:
        if(s->index==0){
            if(rd(s,A733_ROUTE_VIPP,0,&v)!=1)return -1;
            if(v&0x53)return fault(s);
        }else if(s->index<3){
            if(rd(s,A733_ROUTE_VIPP,(uint16_t)(0x20+4*(s->index-1)),&v)!=1)return -1;
            if(v)return fault(s);
        }else if(s->index<7){
            if(rd(s,A733_ROUTE_VIPP,(uint16_t)(0x40+0x40*(s->index-3)),&v)!=1)return -1;
            if(v&0x15)return fault(s);
        }else{
            if(rd(s,A733_ROUTE_TOP,4,&v)!=1)return -1;
            if(v&0x11)return fault(s);
        }
        if(++s->index==8){s->state=A733_ROUTE_PUBLISH;s->index=0;}
        return 0;
    case A733_ROUTE_PUBLISH:
        r=s->ops.publish(s->context,s->token,s->parameters,s->deadline);
        if(r<0 || r>1 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_ROUTE_CONFIGURE;
        return 0;
    case A733_ROUTE_CONFIGURE: {
        if(s->index==CONFIGURE_STEPS){s->state=A733_ROUTE_ARMED;return 1;}
        struct step e=configure(s->index);
        return program(s,&e);
    }
    case A733_ROUTE_STOP:return call(s,s->ops.producer_stop,A733_ROUTE_DISABLE);
    case A733_ROUTE_DISABLE:
        if(s->index==COUNT(disable)){s->state=A733_ROUTE_DRAIN;return 0;}
        return program(s,&disable[s->index]);
    case A733_ROUTE_STATUS:
        if(rd(s,A733_ROUTE_VIPP,0x30,&v)!=1)return -1;
        s->events|=v;
        /* Required parameter load; all other raw status bits are faults.
         * Read raw status even with IRQs masked; the source helper masks it.
         */
        if(v!=0x100)return fault(s);
        s->state=A733_ROUTE_RELEASE;return 0;
    case A733_ROUTE_DRAIN:return call(s,s->ops.drain,A733_ROUTE_STATUS);
    case A733_ROUTE_RELEASE:
        r=s->ops.release(s->context,s->token,s->deadline);
        if(r==1)s->token=0;
        if(r<0 || r>1 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_ROUTE_CLOSED;return r;
    default:return fault(s);
    }
}
