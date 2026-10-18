/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register programming adapted from the A733 HDMI driver.
 * Copyright (C) 2016 Allwinner.
 * RGB range conversion coefficients and write order from dw-hdmi.c:
 * Copyright (C) 2013-2015 Mentor Graphics Inc.
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc.
 * Copyright (C) 2010, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 * EDID field interpretation follows Linux drm_edid.c and drm_edid.h. */
#include "a733_hdmi.h"

static int overlap(const void *a,size_t an,const void *b,size_t bn){
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an);
}
int a733_hdmi_edid_accept(const uint8_t *e,size_t n,enum a733_hdmi_mode mode,
                         enum a733_hdmi_range range){
    static const uint8_t signature[]={0,255,255,255,255,255,255,0};
    struct a733_hdmi_timing t;unsigned hdmi=0,vic=0,quant=0;
    if(!e || n<128 || n>A733_HDMI_EDID_BYTES || (n&127) ||
       (range!=A733_HDMI_LIMITED && range!=A733_HDMI_FULL) ||
       a733_hdmi_timing(mode,&t)!=1)return -1;
    for(unsigned i=0;i<8;++i)if(e[i]!=signature[i])return -1;
    if(e[18]!=1 || (e[19]!=3 && e[19]!=4) || !(e[20]&0x80) ||
       (size_t)(e[126]+1)*128!=n)return -1;
    for(size_t b=0;b<n;b+=128){
        unsigned sum=0;for(unsigned j=0;j<128;++j)sum+=e[b+j];
        if(sum&255)return -1;
        if(!b || e[b]!=2)continue;
        if(e[b+1]<3)return -1;
        unsigned end=e[b+2];
        if(!end)continue;
        if(end<4 || end>127)return -1;
        for(unsigned p=4;p<end;){
            unsigned tag=e[b+p]>>5,len=e[b+p]&31,q=p+1;
            if(q+len>end)return -1;
            if(tag==2){for(unsigned j=0;j<len;++j)
                if((e[b+q+j]&0x7f)==(unsigned)mode)vic=1;}
            if(tag==3 && len>=5 && e[b+q]==3 && e[b+q+1]==12 && e[b+q+2]==0){
                hdmi=1;
                if(len>=7 && e[b+q+6] && (uint32_t)e[b+q+6]*5000000<t.pixel_hz)return -1;
            }
            /* CTA Video Capability Data Block: QS permits RGB range selection.
             * Hardware colorbar is full-range, so do not mislabel it limited. */
            if(tag==7 && len>=2 && e[b+q]==0 && (e[b+q+1]&0x40))quant=1;
            p=q+len;
        }
    }
    return hdmi && vic && (range==A733_HDMI_LIMITED || quant)?1:-1;
}
static int fail(struct a733_hdmi *s,enum a733_hdmi_error e){
    if(!s || s->self!=s)return -1;
    if(s->error==A733_HDMI_OK)s->error=e;
    s->state=A733_HDMI_FAULT_RETAINED;return a733_hdmi_fail(&s->io);
}
int a733_hdmi_init(struct a733_hdmi *s,const struct a733_hdmi_config *c){
    struct a733_hdmi_timing t;
    if(!s || !c || s->self || overlap(s,sizeof(*s),c,sizeof(*c)) ||
       (c->context && overlap(s,sizeof(*s),c->context,1)) ||
       !c->ops.now_us || !c->ops.acquire || !c->ops.validate || !c->ops.read8 ||
       !c->ops.write8 || !c->ops.read32 || !c->ops.write32 || !c->ops.barrier ||
       !c->ops.retain || !c->request.initial.id || !c->request.initial.generation ||
       (c->request.range!=A733_HDMI_LIMITED && c->request.range!=A733_HDMI_FULL) ||
       (c->request.source!=A733_HDMI_PATTERN&&c->request.source!=A733_HDMI_FRAMEBUFFER)||
       a733_hdmi_timing(c->request.mode,&t)!=1)return -1;
    /* No external PHY control/BIST; no live output or pad mux. Complete
     * provenance for the remaining fields is still mandatory at acquisition. */
    const uint32_t *w=c->request.initial.words;
    if((w[0]&0x80000300) || (w[1]&1) || (w[2]&0xe8000000) || (w[3]&0x100))return -1;
    s->self=s;s->io.self=&s->io;s->io.ops=c->ops;s->io.context=c->context;
    s->io.request=c->request;s->timing=t;
    if(a733_hdmi_phy_init(&s->phy,&s->io)!=1 || a733_tcon_tv_init(&s->tv,&s->io)!=1)
        return fail(s,A733_HDMI_IO);
    return 1;
}
static int deadline(struct a733_hdmi *s,uint64_t d){
    if(!s || s->self!=s || s->io.self!=&s->io || s->entered || s->io.fault || !d || d<=s->io.now)return -1;
    s->entered=1;s->io.deadline=d;
    int r=a733_hdmi_tick(&s->io);s->entered=0;
    if(r!=1 || s->io.fault)return fail(s,A733_HDMI_IO);
    return 1;
}
int a733_hdmi_prepare(struct a733_hdmi *s,uint64_t d){
    if(!s || s->state!=A733_HDMI_NEW || deadline(s,d)!=1)return -1;
    s->state=A733_HDMI_ACQUIRE;return 1;
}
int a733_hdmi_start(struct a733_hdmi *s,uint64_t d){
    if(!s || s->state!=A733_HDMI_PREPARED || deadline(s,d)!=1)return -1;
    s->index=0;s->state=A733_HDMI_STARTING;return 1;
}
int a733_hdmi_stop(struct a733_hdmi *s,uint64_t d){
    if(!s || (s->state!=A733_HDMI_PREPARED && s->state!=A733_HDMI_ACTIVE &&
       s->state!=A733_HDMI_STARTING) || deadline(s,d)!=1)return -1;
    s->index=0;s->state=A733_HDMI_STOPPING;s->stop_requested=1;return 1;
}
static int field(struct a733_hdmi *s,uint16_t a,uint8_t m,uint8_t v){
    struct a733_hdmi_step x={A733_HDMI_BASE+a,m,v,1};return a733_hdmi_field(&s->io,&x);
}
static int hpd(struct a733_hdmi *s){
    uint32_t x;if(a733_hdmi_read(&s->io,A733_HDMI_BASE+0x3004,1,&x)!=1)return -1;
    return (x&2)?1:0;
}
static int delay_set(struct a733_hdmi *s,uint64_t us){
    if(s->io.now>UINT64_MAX-us)return fail(s,A733_HDMI_IO);
    s->until=s->io.now+us;return 1;
}
#define B(o,m,v) {A733_HDMI_BASE+(o),(m),(v),1}
static const struct a733_hdmi_step ddc_setup[]={
    B(0x7e07,8,0),B(0x7e0d,0xff,0),B(0x7e0e,0xff,245),
    B(0x7e0b,0xff,0),B(0x7e0c,0xff,221), /* source ~50kHz with24MHz SFR */
    B(0x7e05,4,0),B(0x7e06,0x44,0),B(0x7e00,0x7f,0x50),
    B(0x7e08,0x7f,0x30),B(0x7e0a,0xff,0)
};
static int edid_read(struct a733_hdmi *s){
    struct a733_hdmi_io *b=&s->io;uint32_t x;int r;
    if(s->phase==0){
        r=hpd(s);if(r<0)return -1;if(!r)return fail(s,A733_HDMI_SINK);
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x105,1,3)!=1)return -1;
    }else if(s->phase==1){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x7e0a,1,s->position/256)!=1)return -1;
    }else if(s->phase==2){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x7e01,1,s->position&255)!=1)return -1;
    }else if(s->phase==3){
        if(b->now>UINT64_MAX-10000)return fail(s,A733_HDMI_IO);
        s->transaction_deadline=b->now+10000;
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x7e04,1,s->position>=256?2:1)!=1)return -1;
    }else if(s->phase==4){
        if(b->now>=s->transaction_deadline)return fail(s,A733_HDMI_DDC_ERROR);
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x105,1,&x)!=1)return -1;
        if(b->now>=s->transaction_deadline || (x&1))return fail(s,A733_HDMI_DDC_ERROR);
        if(!(x&2))return 0;
    }else if(s->phase==5){
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x7e03,1,&x)!=1)return -1;
        s->edid[s->position]=(uint8_t)x;
    }else if(s->phase==6){
        if(a733_hdmi_write(b,A733_HDMI_BASE+0x105,1,2)!=1)return -1;
        r=hpd(s);if(r<0)return -1;if(!r)return fail(s,A733_HDMI_SINK);
        ++s->position;s->phase=0;
        if(s->position==128){
            unsigned sum=0;for(unsigned i=0;i<128;++i)sum+=s->edid[i];
            if((sum&255) || s->edid[126]>3)return fail(s,A733_HDMI_EDID_INVALID);
            s->edid_size=(uint16_t)((s->edid[126]+1)*128);
        }
        if(s->position==s->edid_size){
            if(a733_hdmi_edid_accept(s->edid,s->edid_size,b->request.mode,b->request.range)!=1)
                return fail(s,A733_HDMI_EDID_INVALID);
            return 1;
        }return 0;
    }else return fail(s,A733_HDMI_IO);
    ++s->phase;return 0;
}
static const struct a733_hdmi_step video_fixed[]={
    B(0x1000,0xfb,0x78), /* positive HS/VS/DE, progressive HDMI, no HDCP */
    B(0x1011,0xff,12),B(0x1012,0xff,32),B(0x1013,0xff,1),
    B(0x1014,0xff,11),B(0x1015,0x3f,22),B(0x1016,0x3f,33),
    B(0x10e0,0xff,0x10), B(0x0801,0xff,0x40), /* 8-bit, repeat0 */
    B(0x0802,0x27,0x27),B(0x0803,3,0),B(0x0804,0x7f,0x47),
    B(0x4004,1,0), /* Range-specific CSC selection below. */
    B(0x0200,0x9f,1),B(0x0202,0xff,0),B(0x0203,0xff,0),
    B(0x0204,0xff,0),B(0x0205,0xff,0),B(0x0206,0xff,0),B(0x0207,0xff,0),
    B(0x0201,7,7),B(0x1018,7,6), /* phase1, SET_AVMUTE */
    B(0x1019,0xff,0),B(0x101a,0xff,0x20), /* RGB,16:9,no active format */
    B(0x101b,0xff,0),B(0x1017,0x0f,0), /* Default CTA limited RGB; full uses QS. */
    B(0x10b3,0x1f,0),B(0x10b4,0x0f,1),B(0x10b5,0xff,0x11),
    B(0x10e3,0xff,6), /* only AVI + GCP, no audio/HDR */
    B(0x10e1,1,0),B(0x1200,0x11,0),B(0x4003,1,0)
};
static int video_step(struct a733_hdmi *s){
    const struct a733_hdmi_timing *t=&s->timing;
    struct a733_hdmi_step x={A733_HDMI_BASE,0xff,0,1};
    unsigned i=s->index;
    if(i<sizeof(video_fixed)/sizeof(video_fixed[0])){
        x=video_fixed[i];
        if(x.address==A733_HDMI_BASE+0x4004)x.value=s->io.request.range==A733_HDMI_LIMITED;
        if(x.address==A733_HDMI_BASE+0x101b)x.value=s->io.request.range==A733_HDMI_FULL?8:0;
    }
    else {
        i-=sizeof(video_fixed)/sizeof(video_fixed[0]);
        /* Source-backed full RGB -> limited RGB. Hardware uses diagonal
         * 0x1b7c with scale1 and offset0x20, corresponding to219/255 and16.
         * Preserve source LSB-before-MSB ordering, column then A/B/C. */
        static const uint16_t coeff[3][4]={
            {0x1b7c,0,0,0x20},{0,0x1b7c,0,0x20},{0,0,0x1b7c,0x20}};
        if(i<26){
            if(i==0){x.address+=0x4100;x.mask=0xb3;x.value=0;}
            else if(i==25){x.address+=0x4101;x.mask=0xf3;x.value=0x41;}
            else {
                unsigned j=i-1,col=j/6,row=(j%6)/2,lsb=!(j&1);
                x.address+=0x4102+row*8+col*2+lsb;
                x.value=lsb?coeff[row][col]&255:coeff[row][col]>>8;
            }
            goto program;
        }
        i-=26;
        static const uint16_t offsets[]={0x1001,0x1002,0x1003,0x1004,0x1005,0x1006,
            0x1007,0x1008,0x1009,0x100a,0x100b,0x100c,0x100d,0x101c};
        if(i>=sizeof(offsets)/sizeof(offsets[0]))return fail(s,A733_HDMI_IO);
        x.address+=offsets[i];
        const uint16_t values[]={t->width&255,t->width>>8,(t->htotal-t->width)&255,
            (t->htotal-t->width)>>8,t->height&255,t->height>>8,t->vtotal-t->height,
            t->hfront&255,t->hfront>>8,t->hsync&255,t->hsync>>8,t->vfront,t->vsync,t->vic};
        x.value=values[i];
        if(i==1 || i==3 || i==5 || i==8)x.mask=0x1f;
        else if(i==10)x.mask=1;
        else if(i==12)x.mask=0x3f;
    }
program:
    if(a733_hdmi_field(&s->io,&x)!=1)return -1;
    if(++s->index==sizeof(video_fixed)/sizeof(video_fixed[0])+26+14){
        if(field(s,0x4001,0x7f,0x68)!=1)return -1;
        /* Source rewrites INVIDCONF after programming to latch input state. */
        if(field(s,0x1000,0xfb,0x78)!=1)return -1;
        return 1;
    }return 0;
}
static int stop_step(struct a733_hdmi *s){
    static const struct a733_hdmi_step steps[]={
        B(0x1018,3,2),B(0x3000,0x10,0x10),B(0x3000,8,0),
        B(0x4001,0x7f,0x7f),
        {A733_HDMI_TOP_BASE+0x20,0x10100000,0,4},
        {A733_HDMI_PHY_BASE+0x20,0x08000000,0,4}
    };
    if(s->index==3){
        if(a733_tcon_tv_stop(&s->tv)!=1)return -1;
        ++s->index;return 0;
    }
    unsigned i=s->index>3?s->index-1:s->index;
    if(i>=sizeof(steps)/sizeof(steps[0]))return fail(s,A733_HDMI_IO);
    if(a733_hdmi_field(&s->io,&steps[i])!=1)return -1;
    if(++s->index==7){
        if(a733_hdmi_tick(&s->io)!=1)return -1;
        s->state=A733_HDMI_STOPPED_RETAINED;return 1;
    }return 0;
}
static int poll_inner(struct a733_hdmi *s){
    struct a733_hdmi_io *b=&s->io;int r;uint32_t x;
    if(b->fault || s->state==A733_HDMI_FAULT_RETAINED)return a733_hdmi_fail(b);
    if(s->state==A733_HDMI_NEW)return -1;
    if(s->state==A733_HDMI_PREPARED || s->state==A733_HDMI_STOPPED_RETAINED)return 1;
    if(a733_hdmi_tick(b)!=1)return -1;
    switch(s->state){
    case A733_HDMI_ACQUIRE:
        r=b->ops.acquire(b->context,&b->request,b->deadline,&b->reservation);
        if(r<0 || r>1 || a733_hdmi_tick(b)!=1)return fail(s,A733_HDMI_AUTHORITY);
        if(!r)return 0;
        if(!b->reservation.clock.id || !b->reservation.clock.owner ||
           !(b->reservation.clock.exclusive&A733_HDMI_VO1_SCOPE) ||
           !(b->reservation.clock.shared&A733_CLOCK_OSC) ||
           !b->reservation.generation || !b->reservation.rail_owner ||
           !b->reservation.rail_generation || !b->reservation.system_owner ||
           !b->reservation.system_generation || !b->reservation.inactive_epoch ||
           (!b->reservation.recovery_epoch==!b->reservation.supervision_epoch) ||
           (b->request.source==A733_HDMI_FRAMEBUFFER &&
            b->reservation.framebuffer_epoch!=b->reservation.inactive_epoch) ||
           (b->request.source==A733_HDMI_PATTERN && b->reservation.framebuffer_epoch) ||
           (b->request.source!=A733_HDMI_PATTERN&&b->request.source!=A733_HDMI_FRAMEBUFFER) ||
           (b->reservation.supervision_epoch &&
            (b->request.mode!=A733_HDMI_720P60 || b->request.range!=A733_HDMI_LIMITED)))
            return fail(s,A733_HDMI_AUTHORITY);
        b->admitted=1;s->state=A733_HDMI_PREFLIGHT;return 0;
    case A733_HDMI_PREFLIGHT:
        r=a733_hdmi_phy_preflight(&s->phy);if(r<0)return -1;
        if(r)s->state=A733_HDMI_HPD;return 0;
    case A733_HDMI_HPD:
        r=hpd(s);if(r<0)return -1;if(!r)return 0;
        s->sink_seen=1;s->index=0;s->state=A733_HDMI_DDC_INIT;return 0;
    case A733_HDMI_DDC_INIT:
        if(!s->index){
            /* Source software-reset request; self-clearing, no fabricated readback. */
            if(a733_hdmi_write(b,A733_HDMI_BASE+0x7e09,1,0)!=1)return -1;
        }else if(a733_hdmi_field(b,&ddc_setup[s->index-1])!=1)return -1;
        if(++s->index==sizeof(ddc_setup)/sizeof(ddc_setup[0])+1){
            s->edid_size=128;s->state=A733_HDMI_EDID;s->index=0;}
        return 0;
    case A733_HDMI_EDID:
        r=edid_read(s);if(r<0)return -1;
        if(r){s->index=0;s->state=A733_HDMI_CONTROLLER;}return 0;
    case A733_HDMI_CONTROLLER:
        r=video_step(s);if(r<0)return -1;
        if(r){s->index=0;s->state=A733_HDMI_PHY;}return 0;
    case A733_HDMI_PHY:
        r=a733_hdmi_phy_poll(&s->phy);if(r<0)return -1;
        if(r)s->state=A733_HDMI_TV;return 0;
    case A733_HDMI_TV:
        r=a733_tcon_tv_prepare(&s->tv);if(r<0)return -1;
        if(r){
            if(hpd(s)!=1 || a733_hdmi_tick(b)!=1)return fail(s,A733_HDMI_SINK);
            s->state=A733_HDMI_PREPARED;return 1;}
        return 0;
    case A733_HDMI_STARTING:
        if(hpd(s)!=1)return fail(s,A733_HDMI_SINK);
        if(!s->index){
            if(a733_hdmi_read(b,A733_HDMI_PHY_BASE+0x40,4,&x)!=1 || !(x&1))
                return fail(s,A733_HDMI_IO);
            if(a733_hdmi_read(b,A733_HDMI_BASE+0x3004,1,&x)!=1 || (x&3)!=3)
                return fail(s,A733_HDMI_IO);
            if(a733_tcon_tv_start(&s->tv)!=1 || delay_set(s,50000)!=1)return -1;
            s->index=1;return 0;
        }
        if(b->now<s->until)return 0;
        if(a733_hdmi_read(b,A733_HDMI_PHY_BASE+0x40,4,&x)!=1 || !(x&1))
            return fail(s,A733_HDMI_IO);
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x3004,1,&x)!=1 || (x&3)!=3 ||
           a733_tcon_tv_health(&s->tv)!=1)return fail(s,A733_HDMI_UNDERFLOW);
        if(field(s,0x1018,3,1)!=1 || a733_hdmi_tick(b)!=1)return -1;
        s->state=A733_HDMI_ACTIVE;return 1;
    case A733_HDMI_ACTIVE:
        r=hpd(s);if(r<0)return -1;
        if(!r){s->error=A733_HDMI_SINK;s->index=0;s->state=A733_HDMI_STOPPING;return 0;}
        if(a733_hdmi_read(b,A733_HDMI_BASE+0x3004,1,&x)!=1)return -1;
        if(x&1){
            uint32_t top;
            if(a733_hdmi_read(b,A733_HDMI_PHY_BASE+0x40,4,&top)!=1)return -1;
            if(!(top&1))x&=~1u;
        }
        if(!(x&1) || a733_tcon_tv_health(&s->tv)!=1){
            if(b->fault)return -1;
            s->error=A733_HDMI_UNDERFLOW;s->index=0;s->state=A733_HDMI_STOPPING;return 0;}
        return 1;
    case A733_HDMI_STOPPING:return stop_step(s);
    default:return fail(s,A733_HDMI_IO);
    }
}
int a733_hdmi_poll(struct a733_hdmi *s){
    int r;
    if(!s || s->self!=s || s->io.self!=&s->io)return -1;
    if(s->entered)return fail(s,A733_HDMI_AUTHORITY);
    s->entered=1;r=poll_inner(s);
    if(r<0 && s->state!=A733_HDMI_NEW)r=fail(s,A733_HDMI_IO);
    s->entered=0;return r;
}
int a733_hdmi_service(struct a733_hdmi *s,uint64_t d){
    if(!s || s->state!=A733_HDMI_ACTIVE || deadline(s,d)!=1)return -1;
    return a733_hdmi_poll(s);
}
