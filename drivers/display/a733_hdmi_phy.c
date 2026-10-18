/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register definitions and sequences adapted from the A733 HDMI driver.
 * Copyright (C) 2016 Allwinner. */
#include "a733_hdmi_phy.h"

int a733_hdmi_timing(enum a733_hdmi_mode m,struct a733_hdmi_timing *t){
    if(!t)return -1;
    if(m==A733_HDMI_720P60)*t=(struct a733_hdmi_timing){
        74250000,1280,720,1650,750,110,40,220,5,5,20,4};
    else if(m==A733_HDMI_1080P60)*t=(struct a733_hdmi_timing){
        148500000,1920,1080,2200,1125,88,44,148,4,5,36,16};
    else return -1;
    return 1;
}
int a733_hdmi_fail(struct a733_hdmi_io *b){
    if(!b || b->self!=b)return -1;
    b->fault=1;
    if(!b->retained && b->ops.retain &&
       b->ops.retain(b->context,&b->reservation)==1)b->retained=1;
    return -1;
}
int a733_hdmi_tick(struct a733_hdmi_io *b){
    uint64_t n;
    if(!b || b->self!=b || b->fault)return -1;
    if(b->ops.now_us(b->context,&n)!=1 || n<b->now || n>=b->deadline)
        return a733_hdmi_fail(b);
    if(b->fault)return -1; /* callback reentry may have faulted the owner */
    b->now=n;return 1;
}
static int access_ok(struct a733_hdmi_io *b){
    if(a733_hdmi_tick(b)!=1)return -1;
    if(!b->admitted || b->ops.validate(b->context,&b->request,
       &b->reservation,b->deadline)!=1)return a733_hdmi_fail(b);
    return a733_hdmi_tick(b);
}
int a733_hdmi_read(struct a733_hdmi_io *b,uintptr_t a,unsigned w,uint32_t *v){
    uint8_t x=0;int r;
    if(!v || (w!=1 && w!=4) || (w==4 && (a&3)))return a733_hdmi_fail(b);
    if(access_ok(b)!=1)return -1;
    if(w==1){r=b->ops.read8(b->context,a,&x);*v=x;}
    else r=b->ops.read32(b->context,a,v);
    if(r!=1)return a733_hdmi_fail(b);
    return a733_hdmi_tick(b);
}
int a733_hdmi_write(struct a733_hdmi_io *b,uintptr_t a,unsigned w,uint32_t v){
    int r;
    if((w!=1 && w!=4) || (w==4 && (a&3)) || (w==1 && v>255))
        return a733_hdmi_fail(b);
    if(access_ok(b)!=1)return -1;
    ++b->writes;
    r=w==1?b->ops.write8(b->context,a,(uint8_t)v):b->ops.write32(b->context,a,v);
    if(r!=1 || a733_hdmi_tick(b)!=1)return a733_hdmi_fail(b);
    if(b->ops.barrier(b->context)!=1)return a733_hdmi_fail(b);
    return a733_hdmi_tick(b);
}
int a733_hdmi_field(struct a733_hdmi_io *b,const struct a733_hdmi_step *s){
    uint32_t x,y;
    if(!s || (s->value&~s->mask))return a733_hdmi_fail(b);
    if(a733_hdmi_read(b,s->address,s->width,&x)!=1)return -1;
    y=(x&~s->mask)|s->value;
    if(a733_hdmi_write(b,s->address,s->width,y)!=1 ||
       a733_hdmi_read(b,s->address,s->width,&x)!=1)return -1;
    return (x&s->mask)==s->value?1:a733_hdmi_fail(b);
}
#define B(o,m,v) {A733_HDMI_BASE+(o),(m),(v),1}
#define P(o,m,v) {A733_HDMI_PHY_BASE+(o),(m),(v),4}
static const uint16_t initial_offsets[]={0,4,0x20,0x24,0x28,0x2c,0x30,0x34};
/* Preserve TOP source ordering and its untouched charge-pump/test fields. */
static const struct a733_hdmi_step preflight_steps[]={
    B(0x1ff,3,3), B(0x1018,3,2), B(0x4001,0x7f,0x7f),
    B(0x4005,1,1), B(0x3000,0xdf,0x16), B(0x3006,0xff,0xf3),
    P(0,0x17,0x17), P(0x20,0x08000000,0), P(4,1,0),P(0x24,0x100,0),
    {UINT64_C(0x03000160),0x1000,0x1000,4},
    {UINT64_C(0x03000164),0xff000000,0,4}
};
static const struct a733_hdmi_step pll_steps[]={
    P(0x20,2,0),P(0x20,0x20,0),P(0x20,0xc0,0),
    P(0x20,0xff00,0x6200),P(0x20,0x7f0000,0), /* mode-specific P0 */
    P(0x28,0xffffffff,0x00035000),P(0x2c,0xffffffff,0x80000000),
    P(0x30,0xffffffff,0x30000000),P(0x20,0x80000000,0x80000000),
    P(0x20,0x40000000,0x40000000),P(0x24,0x80,0x80),
    P(0x20,0x20000000,0x20000000)
};
static int field(struct a733_hdmi_io *b,uintptr_t a,unsigned w,uint32_t m,uint32_t v){
    struct a733_hdmi_step s={a,m,v,(uint8_t)w};return a733_hdmi_field(b,&s);
}
int a733_hdmi_phy_init(struct a733_hdmi_phy *p,struct a733_hdmi_io *b){
    if(!p || p->self || !b || b->self!=b)return -1;
    p->self=p;p->io=b;return 1;
}
int a733_hdmi_phy_preflight(struct a733_hdmi_phy *p){
    uint32_t x;struct a733_hdmi_io *b;
    if(!p || p->self!=p || !(b=p->io) || b->self!=b || b->fault)return -1;
    if(p->preflight)return 1;
    if(p->stage==0){
        uintptr_t a;uint32_t expected;
        if(p->index<8){a=A733_HDMI_PHY_BASE+initial_offsets[p->index];
            expected=b->request.initial.words[p->index];}
        else {a=p->index==8?UINT64_C(0x03000160):UINT64_C(0x03000164);
            expected=p->index==8?b->request.initial.resistor_control:
                b->request.initial.resistor_manual;}
        if(a733_hdmi_read(b,a,4,&x)!=1)return -1;
        if(x!=expected)return a733_hdmi_fail(b);
        if(++p->index==10){p->index=0;p->stage=1;}return 0;
    }
    if(p->stage==1){
        uintptr_t a=p->index==0?A733_HDMI_TV_BASE+0x90:A733_HDMI_TOP_BASE+0x20;
        if(a733_hdmi_read(b,a,4,&x)!=1)return -1;
        if(x&(p->index==0?UINT32_C(0x80000000):UINT32_C(0x10100000)))
            return a733_hdmi_fail(b);
        if(++p->index==2){p->index=0;p->stage=16;}return 0;
    }
    if(p->stage==16){
        if(a733_hdmi_read(b,A733_HDMI_PHY_BASE+0x40,4,&x)!=1)return -1;
        if(x&1)return a733_hdmi_fail(b);
        p->stage=2;return 0;
    }
    if(p->stage!=2)return a733_hdmi_fail(b);
    if(a733_hdmi_field(b,&preflight_steps[p->index])!=1)return -1;
    if(++p->index==sizeof(preflight_steps)/sizeof(preflight_steps[0])){
        p->index=0;p->stage=3;p->preflight=1;return 1;}
    return 0;
}
static uint16_t phy_value(struct a733_hdmi_phy *p){
    static const uint16_t mpll720[]={0x0002,0x1142,0x0214};
    static const uint16_t mpll1080[]={0x0001,0x2080,0x020a};
    static const uint16_t drive[]={7,0x8160,0x8188};
    if(p->index>=3)return drive[p->index-3];
    return p->io->request.mode==A733_HDMI_720P60?mpll720[p->index]:mpll1080[p->index];
}
/* Six internal 16-bit register writes, each followed by a real read transaction.
 * A fresh W1C status clear precedes each command; error wins over DONE. */
static int phy_transaction(struct a733_hdmi_phy *p){
    static const uint8_t addresses[]={6,0x10,0x11,0x19,0x0e,9};
    struct a733_hdmi_io *b=p->io;uint32_t x;unsigned q=p->phase;
    if(a733_hdmi_tick(b)!=1)return -1;
    if(q==0 || q==7){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x108,1,3)!=1)return -1;
    }else if(q==1){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x3021,1,addresses[p->index])!=1)return -1;
    }else if(q==2 || q==3){
        uint16_t v=phy_value(p);
        if(a733_hdmi_write(b,A733_HDMI_BASE+(q==2?0x3022:0x3023),1,
            q==2?v>>8:v&255)!=1)return -1;
    }else if(q==4 || q==8){
        if(b->now>UINT64_MAX-5000)return a733_hdmi_fail(b);
        p->operation_deadline=b->now+5000;
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x3026,1,q==4?0x10:1)!=1)return -1;
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(b->now>UINT64_MAX-10)return a733_hdmi_fail(b);
        p->until=b->now+10;
    }else if(q==5 || q==9){
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(b->now<p->until)return 0;
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x108,1,&x)!=1)return -1;
        if(b->now>=p->operation_deadline || (x&1))return a733_hdmi_fail(b);
        if(!(x&2)){p->until=p->operation_deadline-b->now<10?
            p->operation_deadline:b->now+10;return 0;}
    }else if(q==6 || q==10){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x108,1,2)!=1)return -1;
    }else if(q==11){
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x3024,1,&x)!=1)return -1;
        p->value=(uint16_t)(x<<8);
    }else if(q==12){
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x3025,1,&x)!=1)return -1;
        if((p->value|x)!=phy_value(p))return a733_hdmi_fail(b);
        p->phase=0;return ++p->index==6?1:0;
    }else return a733_hdmi_fail(b);
    ++p->phase;return 0;
}
int a733_hdmi_phy_poll(struct a733_hdmi_phy *p){
    struct a733_hdmi_io *b;uint32_t x;int r;
    if(!p || p->self!=p || !(b=p->io) || b->self!=b || b->fault || !p->preflight)return -1;
    if(p->ready)return 1;
    if(a733_hdmi_tick(b)!=1)return -1;
    switch(p->stage){
    case 3:{
        struct a733_hdmi_step s=pll_steps[p->index];
        if(p->index==4)s.value=b->request.mode==A733_HDMI_720P60?0x1f0000:0x0f0000;
        if(a733_hdmi_field(b,&s)!=1)return -1;
        if(++p->index==sizeof(pll_steps)/sizeof(pll_steps[0])){
            if(b->now>UINT64_MAX-20000)return a733_hdmi_fail(b);
            p->operation_deadline=b->now+20000;p->stage=4;p->index=0;}
        return 0;
    }
    case 4:
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(a733_hdmi_read(b,A733_HDMI_PHY_BASE+0x40,4,&x)!=1)return -1;
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(!(x&1)){p->locks=0;return 0;}
        if(++p->locks<2)return 0;
        if(b->now>UINT64_MAX-20)return a733_hdmi_fail(b);
        p->until=b->now+20;p->stage=5;return 0;
    case 5:
        if(b->now<p->until)return 0;
        if(field(b,A733_HDMI_PHY_BASE+0x20,4,0x08000000,0x08000000)!=1)return -1;
        p->stage=6;return 0;
    case 6:
        if(field(b,A733_HDMI_BASE+0x3000,1,0x20,0)!=1)return -1;
        if(b->now>UINT64_MAX-5)return a733_hdmi_fail(b);
        p->until=b->now+5;p->stage=7;return 0;
    case 7:
        if(b->now<p->until)return 0;
        if(field(b,A733_HDMI_BASE+0x3000,1,0x20,0x20)!=1)return -1;
        p->stage=8;return 0;
    case 8:
        if(field(b,A733_HDMI_BASE+0x4005,1,1,0)!=1)return -1;
        p->stage=9;return 0;
    case 9:
        if(field(b,A733_HDMI_BASE+0x3027,1,4,0)!=1 ||
           field(b,A733_HDMI_BASE+0x3028,1,0x44,0)!=1)return -1;
        p->stage=10;return 0;
    case 10:
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x3034,1,0x10)!=1)return -1;
        p->stage=11;return 0;
    case 11:
        if(field(b,A733_HDMI_BASE+0x3020,1,0x7f,0x69)!=1)return -1;
        p->stage=12;return 0;
    case 12:
        r=phy_transaction(p);if(r<0)return -1;
        if(r){p->stage=13;p->index=0;}return 0;
    case 13:
        if(field(b,A733_HDMI_BASE+0x3000,1,0x10,0)!=1)return -1;
        p->stage=14;return 0;
    case 14:
        if(field(b,A733_HDMI_BASE+0x3000,1,8,8)!=1)return -1;
        if(b->now>UINT64_MAX-2500)return a733_hdmi_fail(b);
        p->operation_deadline=b->now+2500;p->until=b->now+5;
        p->locks=0;p->stage=15;return 0;
    case 15:
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(b->now<p->until)return 0;
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x3004,1,&x)!=1)return -1;
        if(b->now>=p->operation_deadline)return a733_hdmi_fail(b);
        if(!(x&1)){p->locks=0;p->until=p->operation_deadline-b->now<5?
            p->operation_deadline:b->now+5;return 0;}
        if(++p->locks<2){p->until=p->operation_deadline-b->now<5?
            p->operation_deadline:b->now+5;return 0;}
        if(a733_hdmi_tick(b)!=1)return -1;
        p->ready=1;return 1;
    default:return a733_hdmi_fail(b);
    }
}
