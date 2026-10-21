/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register definitions and sequences adapted from the A733 Linux camera driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2007-2017 Allwinnertech Co., Ltd.
 * Authors: Zhao Wei <zhaowei@allwinnertech.com>
 */
#include "a733_camera_dma.h"
#include "a733_camera_common.h"
#include <stddef.h>
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define PLANE_BYTES UINT64_C(307200)
#define BUS_LIMIT (UINT64_C(1)<<34)
#define EVENTS UINT32_C(0x2fe0e7)
#define NORMAL UINT32_C(0xa3)
struct step { uint16_t off; uint8_t kind; uint32_t mask, value; };
/* kind 1: Y address, 2: UV address, 3: write-one-to-clear. */
static const struct step setup[] = {
    {0x000,0,0x70007301,1}, /* TOP enabled, auto length, no VE handshake. */
    {0x004,0,1,0}, /* Online, channel 0 only. */
    {0x200,0,0x183,0}, /* No compression, FIFO addresses or frame drop. */
    {0x204,0,0x002f3ffc,0x00040800}, /* Progressive NV16, FIELD_EITHER. */
    {0x210,0,0x3fff3fff,640u<<16},
    {0x214,0,0x3fff3fff,480u<<16},
    {0x238,0,0x3fff3fff,(640u<<16)|640u},
    {0x23c,0,0x3fff3fff,(480u<<16)|1280u},
    {0x220,1,UINT32_MAX,0},
    {0x228,2,UINT32_MAX,0},
    {0x230,0,UINT32_MAX,0}, /* Third plane unused by NV16. */
    {0x254,3,0,EVENTS},
    {0x200,0,1,1}
};
static const struct step disable[] = {
    {0x200,0,1,0}, {0x000,0,1,0}
};
static int fail(struct a733_camera_dma *s) { s->state=A733_CAM_DMA_FAULT;return -1; }
static int tick(struct a733_camera_dma *s)
{
    return a733_camera_clock(s->ops.now_us,s->context,&s->now,s->deadline)?1:fail(s);
}
static int plane_valid(const struct a733_camera_plane *p)
{
    return p->bus_address && !(p->bus_address&63) && p->mapped_bytes>=PLANE_BYTES &&
        p->bus_address<BUS_LIMIT && p->mapped_bytes<=BUS_LIMIT-p->bus_address;
}
int a733_camera_dma_init(struct a733_camera_dma *s,
    const struct a733_camera_dma_ops *o,void *ctx,const struct a733_camera_dma_config *c)
{
    if(!s || !o || !c || s->initialized || s->state!=A733_CAM_DMA_NEW ||
       c->block>5 || !c->frame_id || !plane_valid(&c->y) || !plane_valid(&c->uv) ||
       (c->y.bus_address<c->uv.bus_address+c->uv.mapped_bytes &&
        c->uv.bus_address<c->y.bus_address+c->y.mapped_bytes) ||
       a733_camera_overlap(s,sizeof(*s),o,sizeof(*o)) ||
       a733_camera_overlap(s,sizeof(*s),c,sizeof(*c)) ||
       (ctx && a733_camera_overlap(s,sizeof(*s),ctx,1)) || !o->now_us || !o->admit ||
       !o->read || !o->write || !o->start_one || !o->producer_stop || !o->drain ||
       !o->sync_cpu || !o->release)return -1;
    *s=(struct a733_camera_dma){.ops=*o,.config=*c,.context=ctx,.initialized=1};
    return 1;
}
static int begin(struct a733_camera_dma *s,enum a733_camera_dma_state state,uint64_t d)
{
    if(!d || d<=s->now)return -1;
    s->state=state;s->deadline=d;s->index=0;s->phase=0;return 1;
}
int a733_camera_dma_open(struct a733_camera_dma *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_CAM_DMA_NEW)return -1;
    if(begin(s,A733_CAM_DMA_ADMIT,d)!=1)return -1;
    s->owner=A733_CAMERA_RESERVED;return 1;
}
int a733_camera_dma_capture(struct a733_camera_dma *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_CAM_DMA_ARMED)return -1;
    return begin(s,A733_CAM_DMA_START,d);
}
int a733_camera_dma_close(struct a733_camera_dma *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_CAM_DMA_READY)return -1;
    return begin(s,A733_CAM_DMA_RELEASE,d);
}
int a733_camera_dma_readout(struct a733_camera_dma *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_CAM_DMA_STOP || s->retained_readout ||
       s->owner!=A733_CAMERA_DEVICE || !(s->events&1) || !d || d<=s->now)return -1;
    s->retained_readout=1;s->deadline=d;s->index=0;s->phase=0;return 1;
}
int a733_camera_dma_rearm(struct a733_camera_dma *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_CAM_DMA_READABLE || !s->retained_readout ||
       s->owner!=A733_CAMERA_DEVICE || !(s->saved&1) || (s->saved&~NORMAL))return -1;
    return begin(s,A733_CAM_DMA_REARM,d);
}
static int read_reg(struct a733_camera_dma *s,uint16_t off,uint32_t *v)
{
    if(s->ops.read(s->context,s->token,off,v,s->deadline)!=1 || tick(s)!=1)return fail(s);
    return 1;
}
static int write_reg(struct a733_camera_dma *s,uint16_t off,uint32_t v)
{
    if(s->ops.write(s->context,s->token,off,v,s->deadline)!=1 || tick(s)!=1)return fail(s);
    return 1;
}
static int program(struct a733_camera_dma *s,const struct step *p,size_t n)
{
    if(s->index==n)return 1;
    const struct step *e=&p[s->index];
    uint32_t v,value=e->value;
    if(e->kind==1)value=(uint32_t)(s->config.y.bus_address>>2);
    if(e->kind==2)value=(uint32_t)(s->config.uv.bus_address>>2);
    if(e->kind==3) {
        if(!s->phase) {
            if(write_reg(s,e->off,value)!=1)return -1;
            s->phase=1;return 0;
        }
        if(read_reg(s,e->off,&v)!=1)return -1;
        if(v)return fail(s);
        s->phase=0;++s->index;return 0;
    }
    if(!s->phase) {
        if(read_reg(s,e->off,&v)!=1)return -1;
        s->saved=(v&~e->mask)|(value&e->mask);s->phase=1;return 0;
    }
    if(s->phase==1) {
        if(write_reg(s,e->off,s->saved)!=1)return -1;
        s->phase=2;return 0;
    }
    if(read_reg(s,e->off,&v)!=1)return -1;
    if((v&e->mask)!=(value&e->mask))return fail(s);
    s->phase=0;++s->index;return 0;
}
static int callback(struct a733_camera_dma *s,int (*fn)(void *,uint64_t,uint64_t),
                    enum a733_camera_dma_state next)
{
    int r=fn(s->context,s->token,s->deadline);
    if(r<0 || r>1 || tick(s)!=1)return fail(s);
    if(r) { s->state=next;s->index=0;s->phase=0; }
    return 0;
}
int a733_camera_dma_poll(struct a733_camera_dma *s)
{
    uint64_t token;uint32_t v;int r;
    if(!s || !s->initialized || s->state==A733_CAM_DMA_FAULT)return -1;
    if(s->state==A733_CAM_DMA_ARMED || s->state==A733_CAM_DMA_READY || s->state==A733_CAM_DMA_READABLE ||
       s->state==A733_CAM_DMA_CLOSED)return 1;
    if(s->state==A733_CAM_DMA_NEW)return -1;
    if(tick(s)!=1)return -1;
    switch(s->state) {
    case A733_CAM_DMA_ADMIT:
        token=s->token;r=s->ops.admit(s->context,&s->config,s->deadline,&token);
        r=a733_camera_adopt(&s->token,token,r);
        if(r<0 || tick(s)!=1)return fail(s);
        if(r) { s->owner=A733_CAMERA_DEVICE;s->state=A733_CAM_DMA_CHECK; }
        return 0;
    case A733_CAM_DMA_CHECK:
        /* Check all four channels on multichannel blocks, one on blocks 4/5.
         * Nothing may be running or have interrupts enabled before takeover.
         */
        if(s->index==0) {
            if(read_reg(s,0,&v)!=1)return -1;
            if(v&1)return fail(s);
        } else if(s->index==1) {
            if(read_reg(s,0x100,&v)!=1)return -1;
            if(v)return fail(s);
        } else {
            unsigned k=s->index-2;
            uint16_t off=(uint16_t)(0x200+(k/2)*0x200+(k%2?0x50:0));
            if(read_reg(s,off,&v)!=1)return -1;
            if(v&(k%2?UINT32_MAX:0x183))return fail(s);
        }
        if(++s->index==(s->config.block<4?10:4)) {
            s->index=0;s->state=A733_CAM_DMA_CONFIGURE;
        }
        return 0;
    case A733_CAM_DMA_CONFIGURE:
        r=program(s,setup,COUNT(setup));
        if(r==1)s->state=A733_CAM_DMA_ARMED;
        return r;
    case A733_CAM_DMA_START:
        return callback(s,s->ops.start_one,A733_CAM_DMA_WAIT);
    case A733_CAM_DMA_WAIT:
        if(read_reg(s,0x254,&v)!=1)return -1;
        s->events|=v;
        if(v&~NORMAL)return fail(s);
        if(v&1)s->state=A733_CAM_DMA_STOP;
        return 0;
    case A733_CAM_DMA_STOP:
        return callback(s,s->ops.producer_stop,s->retained_readout?A733_CAM_DMA_FINAL_STATUS:A733_CAM_DMA_DISABLE);
    case A733_CAM_DMA_DISABLE:
        r=program(s,disable,COUNT(disable));
        if(r==1)s->state=A733_CAM_DMA_DRAIN;
        return r<0?-1:0;
    case A733_CAM_DMA_DRAIN:
        return callback(s,s->ops.drain,A733_CAM_DMA_FINAL_STATUS);
    case A733_CAM_DMA_FINAL_STATUS:
        if(s->index==0) {
            if(read_reg(s,0x254,&v)!=1)return -1;
            s->events|=v;
            if((v&~NORMAL) || !(v&1))return fail(s);
            s->saved=v;
        } else if(s->index==1) {
            if(read_reg(s,0x24c,&v)!=1)return -1;
            if(v&1)return fail(s);
        } else {
            if(!s->retained_readout && write_reg(s,0x254,s->saved)!=1)return -1;
            s->state=A733_CAM_DMA_SYNC;
        }
        ++s->index;return 0;
    case A733_CAM_DMA_SYNC:
        r=callback(s,s->ops.sync_cpu,s->retained_readout?A733_CAM_DMA_READABLE:A733_CAM_DMA_READY);
        if(s->state==A733_CAM_DMA_READABLE)return 1;
        if(s->state==A733_CAM_DMA_READY) { s->owner=A733_CAMERA_CALLER;return 1; }
        return r;
    case A733_CAM_DMA_REARM:
        if(s->index==0) {
            /* Nothing may have changed since the final status observation. */
            if(read_reg(s,0x254,&v)!=1)return -1;
            if(v!=s->saved)return fail(s);
        } else if(s->index==1) {
            if(write_reg(s,0x254,s->saved)!=1)return -1;
        } else if(s->index==2) {
            if(read_reg(s,0x254,&v)!=1)return -1;
            if(v)return fail(s);
        } else if(s->index==3) {
            if(read_reg(s,0x24c,&v)!=1)return -1;
            if(v&1)return fail(s);
        } else if(s->index==4) {
            if(read_reg(s,0x000,&v)!=1)return -1;
            if((v&setup[0].mask)!=setup[0].value)return fail(s);
        } else if(s->index==5) {
            if(read_reg(s,0x200,&v)!=1)return -1;
            if((v&0x183)!=1)return fail(s);
        } else {
            token=s->token;r=s->ops.admit(s->context,&s->config,s->deadline,&token);
            if(token!=s->token || r<0 || r>1 || tick(s)!=1)return fail(s);
            if(!r)return 0;
            s->events=0;s->saved=0;s->retained_readout=0;
            s->state=A733_CAM_DMA_ARMED;return 1;
        }
        ++s->index;return 0;
    case A733_CAM_DMA_RELEASE:
        r=s->ops.release(s->context,s->token,s->deadline);
        if(r==1)s->token=0;
        if(r<0 || r>1 || tick(s)!=1)return fail(s);
        if(r)s->state=A733_CAM_DMA_CLOSED;
        return r;
    default:return fail(s);
    }
}
