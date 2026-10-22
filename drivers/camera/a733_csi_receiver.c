/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register definitions and sequences adapted from the A733 Linux camera driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2019 by Allwinnertech Co., Ltd. http://www.allwinnertech.com
 * Copyright (c) 2007-2017 Allwinnertech Co., Ltd.
 */
#include "a733_csi_receiver.h"
#include "a733_camera_common.h"
#include <stddef.h>
#include <stdint.h>
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
struct step { uint16_t off; uint8_t region, special; uint32_t mask, value; };
/* special: 1 caller settle, 2 caller deskew, 3 lane map, 4 port-0 debounce,
 * 5 W1C status (no read/verification), 6 direct write + verification,
 * 7 parser capture mode (SCAP for image, VCAP for existing video API),
 * 8 port YUV order, 9 parser YUV order, 10 output width, 11 output height. */
static const struct step configure[] = {
    {0x118,A733_CSI_PORT,5,0,0x1fff},
    {0x044,A733_CSI_PARSER,5,0,7},
    /* Local PHY: two data lanes, one clock, MIPI work mode. */
    {0x000,A733_CSI_PHY,0,0x000003f0,0x00000130},
    {0x000,A733_CSI_PHY,0,0x34071000,0x04041000},
    {0x008,A733_CSI_PHY,0,0x00001010,0x00001000},
    {0x00c,A733_CSI_PHY,0,0x01f00000,0x01300000},
    {0x020,A733_CSI_PHY,0,0x00ff003f,0},
    {0x024,A733_CSI_PHY,0,0x00ff003f,0},
    {0x028,A733_CSI_PHY,1,0x00ff030f,0x300},
    {0x02c,A733_CSI_PHY,0,0x00003f04,0x00001300},
    {0x02c,A733_CSI_PHY,4,0x00010000,0},
    {0x000,A733_CSI_PHY,0,1,1},
    {0x034,A733_CSI_PHY,0,1,1},
    {0x018,A733_CSI_PHY,2,0x01f00000,0},
    /* One progressive UYVY channel. Unused fields start from reset. */
    {0x110,A733_CSI_PORT,0,0x400e000f,0x000e000f},
    {0x000,A733_CSI_PORT,0,0x80030f00,0x00000200},
    {0x004,A733_CSI_PORT,3,UINT32_MAX,0},
    {0x008,A733_CSI_PORT,0,0xffff,0},
    {0x100,A733_CSI_PORT,8,0x00030041,0x00000001},
    {0x108,A733_CSI_PORT,0,UINT32_MAX,0x1e},
    /* The parser has no memory-bus master; downstream admission precedes it. */
    {0x000,A733_CSI_PARSER,0,0x8000,0x8000},
    {0x000,A733_CSI_PARSER,0,1,1},
    {0x000,A733_CSI_PARSER,0,1,0},
    {0x000,A733_CSI_PARSER,0,1,1},
    {0x000,A733_CSI_PARSER,0,2,2},
    {0x008,A733_CSI_PARSER,9,0xe0,0},
    {0x000,A733_CSI_PARSER,0,0x80000000,0x80000000},
    {0x024,A733_CSI_PARSER,6,UINT32_MAX,3},
    {0x028,A733_CSI_PARSER,10,0x1fff1fff,0},
    {0x02c,A733_CSI_PARSER,11,0x1fff1fff,0},
    {0x00c,A733_CSI_PARSER,0,0x3c3c3c3c,0},
    {0x00c,A733_CSI_PARSER,7,3,2},
    /* Port enable is last, after all local setup and downstream admission. */
    {0x000,A733_CSI_PORT,0,1,1}
};
static const struct step disable[] = {
    {0x000,A733_CSI_PORT,0,1,0},
    {0x000,A733_CSI_PARSER,0,0x80000000,0},
    {0x00c,A733_CSI_PARSER,0,0x03030303,0},
    {0x000,A733_CSI_PARSER,0,1,0},
    {0x000,A733_CSI_PARSER,0,0x8000,0},
    {0x000,A733_CSI_PHY,0,1,0},
    {0x034,A733_CSI_PHY,0,1,0}
};
static const struct step stop_video[] = {
    {0x00c,A733_CSI_PARSER,0,0x03030303,0}
};
/* The platform supplies reset defaults. These checks reject active local
 * engines, unexpected modes, trigger extensions and local interrupt enables.
 * Stable global power/reference settings are checked without modifying them.
 */
static const struct step precheck[] = {
    {0x000,A733_CSI_TOP,0,0x60000f0f,0x00000b0f},
    {0x000,A733_CSI_PHY,0,3,0},
    {0x014,A733_CSI_PHY,0,0xff,0},
    {0x000,A733_CSI_PORT,0,0x31,0},
    {0x00c,A733_CSI_PORT,0,3,0},
    {0x010,A733_CSI_PORT,0,0x800f0000,0},
    {0x100,A733_CSI_PORT,0,0x1be,0},
    {0x110,A733_CSI_PORT,0,0xb001ff00,0},
    {0x114,A733_CSI_PORT,0,0x1fff,0},
    {0x000,A733_CSI_PARSER,0,0x80018005,0},
    {0x00c,A733_CSI_PARSER,0,0x03030303,0},
    {0x040,A733_CSI_PARSER,0,7,0}
};
static int fault(struct a733_csi_receiver *s) { s->state=A733_CSI_FAULT; return -1; }
static int tick(struct a733_csi_receiver *s)
{
    return a733_camera_clock(s->ops.now_us,s->context,&s->now,s->deadline)?1:fault(s);
}
int a733_csi_init(struct a733_csi_receiver *s,const struct a733_csi_ops *ops,
                  void *context,const struct a733_csi_config *cfg)
{
    struct a733_camera_format f=A733_CAMERA_VGA;
    return a733_csi_init_format(s,ops,context,cfg,&f);
}
int a733_csi_init_format(struct a733_csi_receiver *s,const struct a733_csi_ops *ops,
                         void *context,const struct a733_csi_config *cfg,
                         const struct a733_camera_format *format)
{
    if (!s || !ops || !cfg || s->initialized || s->state!=A733_CSI_NEW ||
        !a733_camera_format_valid(format) ||
        cfg->port>1 || cfg->parser!=cfg->port || !cfg->settle ||
        !cfg->deskew || cfg->deskew>31 || cfg->top_hz!=324000000 ||
        a733_ranges_overlap_or_wrap(s,sizeof(*s),ops,sizeof(*ops)) ||
        a733_ranges_overlap_or_wrap(s,sizeof(*s),cfg,sizeof(*cfg)) ||
        a733_ranges_overlap_or_wrap(s,sizeof(*s),format,sizeof(*format)) ||
        (context && a733_ranges_overlap_or_wrap(s,sizeof(*s),context,1)) || !ops->now_us ||
        !ops->admit || !ops->read || !ops->write || !ops->producer_stop ||
        !ops->drain || !ops->release) return -1;
    *s=(struct a733_csi_receiver){.ops=*ops,.config=*cfg,.context=context,
                                  .format=*format,.initialized=1};
    return 1;
}
static int begin(struct a733_csi_receiver *s,enum a733_csi_state state,uint64_t d)
{
    if (!d || d<=s->now) return -1;
    s->state=state;s->deadline=d;s->index=0;s->phase=0;return 1;
}
int a733_csi_open(struct a733_csi_receiver *s,uint64_t d)
{
    if (!s || !s->initialized || s->state!=A733_CSI_NEW) return -1;
    return begin(s,A733_CSI_ADMIT,d);
}
int a733_csi_open_image(struct a733_csi_receiver *s,uint64_t d)
{
    int r=a733_csi_open(s,d);
    if(r==1)s->image_mode=1;
    return r;
}
int a733_csi_rearm_image(struct a733_csi_receiver *s,uint64_t d)
{
    if (!s || !s->initialized || s->state!=A733_CSI_ARMED || !s->image_mode) return -1;
    return begin(s,A733_CSI_REARM,d);
}
int a733_csi_sample(struct a733_csi_receiver *s,uint64_t d)
{
    if (!s || !s->initialized || s->state!=A733_CSI_ARMED) return -1;
    return begin(s,A733_CSI_SAMPLE,d);
}
int a733_csi_stop_video(struct a733_csi_receiver *s,uint64_t d)
{
    if (!s || !s->initialized || s->state!=A733_CSI_ARMED || s->image_mode) return -1;
    return begin(s,A733_CSI_VIDEO_STOP,d);
}
int a733_csi_close(struct a733_csi_receiver *s,uint64_t d)
{
    if (!s || !s->initialized || s->state!=A733_CSI_ARMED) return -1;
    return begin(s,A733_CSI_PRODUCER_STOP,d);
}
static int read_reg(struct a733_csi_receiver *s,unsigned region,uint16_t off,uint32_t *v)
{
    int r=s->ops.read(s->context,s->token,(enum a733_csi_region)region,off,v,s->deadline);
    if (r!=1 || tick(s)!=1) return fault(s);
    return 1;
}
static int write_reg(struct a733_csi_receiver *s,unsigned region,uint16_t off,uint32_t v)
{
    int r=s->ops.write(s->context,s->token,(enum a733_csi_region)region,off,v,s->deadline);
    if (r!=1 || tick(s)!=1) return fault(s);
    return 1;
}
static int program(struct a733_csi_receiver *s,const struct step *plan,size_t count)
{
    if (s->index==count) return 1;
    const struct step *e=&plan[s->index];
    uint32_t v,value=e->value;
    if(e->special==1)value|=(uint32_t)s->config.settle<<16;
    if(e->special==2)value|=(uint32_t)s->config.deskew<<20;
    if(e->special==3)value=s->config.port ? 0x54 : 0x10;
    if(e->special==7)value=s->image_mode?1:2;
    /* The source sequence codes are YUYV 0 and UYVY 2 in both blocks. */
    if(e->special==8)value|=(s->format.order==A733_CAMERA_UYVY?2u:0u)<<16;
    if(e->special==9)value=(s->format.order==A733_CAMERA_UYVY?2u:0u)<<6;
    if(e->special==10)value=(uint32_t)s->format.width<<16;
    if(e->special==11)value=(uint32_t)s->format.height<<16;
    /* The source enables debounce only for PHY0; PHY1 stays at reset. */
    if(e->special==4 && !s->config.port)value=0x10000;
    if(e->special==5) {
        if(write_reg(s,e->region,e->off,value)!=1)return -1;
        ++s->index;return 0;
    }
    if(!s->phase && e->special!=6) {
        if(read_reg(s,e->region,e->off,&v)!=1)return -1;
        s->saved=(v&~e->mask)|(value&e->mask);s->phase=1;return 0;
    }
    if(s->phase<2) {
        if(write_reg(s,e->region,e->off,e->special==6?value:s->saved)!=1)return -1;
        s->phase=2;return 0;
    }
    if(read_reg(s,e->region,e->off,&v)!=1)return -1;
    if((v&e->mask)!=(value&e->mask))return fault(s);
    s->phase=0;++s->index;return 0;
}
int a733_csi_poll(struct a733_csi_receiver *s)
{
    uint64_t token;
    uint32_t v;
    int r;
    if(!s || !s->initialized || s->state==A733_CSI_FAULT)return -1;
    if(s->state==A733_CSI_ARMED || s->state==A733_CSI_CLOSED)return 1;
    if(s->state==A733_CSI_NEW)return -1;
    if(tick(s)!=1)return -1;
    switch(s->state) {
    case A733_CSI_ADMIT:
        token=s->token;r=s->ops.admit(s->context,&s->config,s->deadline,&token);
        r=a733_camera_adopt(&s->token,token,r);
        if(r<0 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_CSI_PRECHECK;
        return 0;
    case A733_CSI_PRECHECK: {
        const struct step *e=&precheck[s->index];
        if(read_reg(s,e->region,e->off,&v)!=1)return -1;
        if((v&e->mask)!=e->value)return fault(s);
        if(++s->index==COUNT(precheck)) { s->index=0;s->state=A733_CSI_CONFIGURE; }
        return 0;
    }
    case A733_CSI_CONFIGURE:
        r=program(s,configure,COUNT(configure));
        if(r==1)s->state=A733_CSI_ARMED;
        return r;
    case A733_CSI_SAMPLE:
        if(s->index==0) {
            if(read_reg(s,A733_CSI_PORT,0x118,&v)!=1)return -1;
            s->mipi_events|=v&0x1fff;s->saved=v&0x1fff;
            if(v&0x1f80)return fault(s);
        } else if(s->index==1) {
            if(s->saved && write_reg(s,A733_CSI_PORT,0x118,s->saved)!=1)return -1;
        } else if(s->index==2) {
            if(read_reg(s,A733_CSI_PARSER,0x44,&v)!=1)return -1;
            s->parser_events|=v&7;s->saved=v&7;
            if(v&4)return fault(s);
        } else {
            if(s->saved && write_reg(s,A733_CSI_PARSER,0x44,s->saved)!=1)return -1;
            s->state=A733_CSI_ARMED;return 1;
        }
        ++s->index;return 0;
    case A733_CSI_REARM:
        /* Status reads, fence read, SCAP write, SCAP readback. */
        if(s->index<2) {
            if(read_reg(s,s->index?A733_CSI_PARSER:A733_CSI_PORT,s->index?0x44:0x118,&v)!=1)return -1;
            if(v&(s->index?7u:0x1fffu))return fault(s);
        } else if(s->index==2) {
            if(read_reg(s,A733_CSI_PARSER,0x0c,&v)!=1)return -1;
            if(v&0x03030303)return fault(s);
            s->saved=v|1u;
        } else if(s->index==3) {
            if(write_reg(s,A733_CSI_PARSER,0x0c,s->saved)!=1)return -1;
        } else {
            if(read_reg(s,A733_CSI_PARSER,0x0c,&v)!=1)return -1;
            if((v&0x03030303)!=1)return fault(s);
            s->state=A733_CSI_ARMED;return 1;
        }
        ++s->index;return 0;
    case A733_CSI_VIDEO_STOP:
        r=program(s,stop_video,COUNT(stop_video));
        if(r!=1)return r;
        s->state=A733_CSI_ARMED;return 1;
    case A733_CSI_PRODUCER_STOP:
        r=s->ops.producer_stop(s->context,s->token,s->deadline);
        if(r<0 || r>1 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_CSI_DISABLE;
        return 0;
    case A733_CSI_DISABLE:
        r=program(s,disable,COUNT(disable));
        if(r==1)s->state=A733_CSI_DRAIN;
        return r<0?-1:0;
    case A733_CSI_DRAIN:
        r=s->ops.drain(s->context,s->token,s->deadline);
        if(r<0 || r>1 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_CSI_RELEASE;
        return 0;
    case A733_CSI_RELEASE:
        r=s->ops.release(s->context,s->token,s->deadline);
        if(r==1)s->token=0;
        if(r<0 || r>1 || tick(s)!=1)return fault(s);
        if(r)s->state=A733_CSI_CLOSED;
        return r;
    default:return fault(s);
    }
}
