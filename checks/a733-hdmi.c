/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_hdmi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void check_tcon_tv(const uint32_t *,const uint32_t *,unsigned);
struct event { uintptr_t address;uint32_t value;unsigned width;uint64_t time; };
struct model {
    uint8_t dw[0x8000],edid[512];
    uint32_t phy[32],top[16],tv[256],sys[2];
    uint16_t internal[256];
    struct event trace[8192];
    struct a733_hdmi_request expected;
    struct a733_hdmi_reservation grant;
    struct a733_hdmi *owner;
    uint64_t time;
    uint64_t top_lock_observed,svsret_low,tv_enable_time;
    unsigned events,reads,writes,validates,now_calls,retains;
    unsigned hpd,deny,retain_busy,expire_unmute,bad_phy_read,top_unlock,uncertain_unmute;
};
struct system {struct model m;struct a733_hdmi s;struct a733_hdmi_config c;};
static struct system live,ddc_checkpoint,pll_checkpoint,phy_checkpoint,start_checkpoint;
static struct a733_hdmi copied;
static unsigned ddc_saved,pll_saved,phy_saved,start_saved;
static int now(void *p,uint64_t *v){struct model *m=p;++m->now_calls;*v=m->time;return 1;}
static int acquire(void *p,const struct a733_hdmi_request *r,uint64_t d,
                   struct a733_hdmi_reservation *g){
    struct model *m=p;(void)d;assert(!memcmp(r,&m->expected,sizeof(*r)));
    *g=m->grant;return m->deny?-1:1;
}
static int validate(void *p,const struct a733_hdmi_request *r,
                    const struct a733_hdmi_reservation *g,uint64_t d){
    struct model *m=p;++m->validates;
    return !m->deny && m->time<d && !memcmp(r,&m->expected,sizeof(*r)) &&
        !memcmp(g,&m->grant,sizeof(*g));
}
static uint32_t *word(struct model *m,uintptr_t a){
    assert(!(a&3));
    if(a>=A733_HDMI_PHY_BASE && a<A733_HDMI_PHY_BASE+sizeof(m->phy))return &m->phy[(a-A733_HDMI_PHY_BASE)/4];
    if(a>=A733_HDMI_TOP_BASE && a<A733_HDMI_TOP_BASE+sizeof(m->top))return &m->top[(a-A733_HDMI_TOP_BASE)/4];
    if(a>=A733_HDMI_TV_BASE && a<A733_HDMI_TV_BASE+sizeof(m->tv))return &m->tv[(a-A733_HDMI_TV_BASE)/4];
    assert(a==0x03000160 || a==0x03000164);return &m->sys[(a-0x03000160)/4];
}
static int read32(void *p,uintptr_t a,uint32_t *v){
    struct model *m=p;++m->reads;
    if(a==A733_HDMI_PHY_BASE+0x40){
        *v=!m->top_unlock && (m->phy[8]&0xe0000000)==0xe0000000?1:0;
        if(*v && !m->top_lock_observed)m->top_lock_observed=m->time;
        return 1;}
    *v=*word(m,a);return 1;
}
static int read8(void *p,uintptr_t a,uint8_t *v){
    struct model *m=p;++m->reads;assert(a>=A733_HDMI_BASE && a<A733_HDMI_BASE+sizeof(m->dw));
    unsigned o=(unsigned)(a-A733_HDMI_BASE);
    if(o==0x3004){*v=(uint8_t)((m->hpd?2:0)|((m->dw[0x3000]&8)?1:0));return 1;}
    *v=m->dw[o];if(o==0x3024 && m->bad_phy_read)*v^=1;return 1;
}
static void event(struct model *m,uintptr_t a,uint32_t v,unsigned w){
    assert(m->events<sizeof(m->trace)/sizeof(m->trace[0]));
    m->trace[m->events++]=(struct event){a,v,w,m->time};++m->writes;
}
static int write32(void *p,uintptr_t a,uint32_t v){
    struct model *m=p;event(m,a,v,4);
    if(a==A733_HDMI_PHY_BASE+0x20 && (v&0x08000000)){
        assert(m->top_lock_observed && m->time-m->top_lock_observed>=20);}
    if(a==A733_HDMI_TV_BASE+0x90 && (v&0x80000000))m->tv_enable_time=m->time;
    *word(m,a)=v;return 1;
}
static int write8(void *p,uintptr_t a,uint8_t v){
    struct model *m=p;assert(a>=A733_HDMI_BASE && a<A733_HDMI_BASE+sizeof(m->dw));
    unsigned o=(unsigned)(a-A733_HDMI_BASE);event(m,a,v,1);
    if(o==0x105 || o==0x108){m->dw[o]&=(uint8_t)~v;return 1;}
    if(o==0x3000){
        if(!(v&0x20))m->svsret_low=m->time;
        else if(!(m->dw[o]&0x20))assert(m->svsret_low && m->time-m->svsret_low>=5);
    }
    if(o==0x1018 && (v&3)==1)
        assert(m->tv_enable_time && m->time-m->tv_enable_time>=50000);
    m->dw[o]=v;
    if(o==0x3026){
        assert(m->dw[0x3020]==0x69 && m->dw[0x3034]==0x10 && !(m->dw[0x4005]&1));
        unsigned reg=m->dw[0x3021];
        if(v==0x10)m->internal[reg]=(uint16_t)((m->dw[0x3022]<<8)|m->dw[0x3023]);
        else {assert(v==1);m->dw[0x3024]=(uint8_t)(m->internal[reg]>>8);m->dw[0x3025]=(uint8_t)m->internal[reg];}
        m->dw[0x108]=2;
    }
    if(o==0x7e04){
        assert(m->dw[0x7e00]==0x50 && m->dw[0x7e08]==0x30);
        unsigned off=(v==2?256u*m->dw[0x7e0a]:0)+m->dw[0x7e01];
        assert((v==1 || v==2) && off<sizeof(m->edid));
        m->dw[0x7e03]=m->edid[off];m->dw[0x105]=2;
    }
    if(o==0x1018 && (v&3)==1 && m->expire_unmute)m->time=m->owner->io.deadline;
    if(o==0x1018 && (v&3)==1 && m->uncertain_unmute)return -1;
    return 1;
}
static int barrier(void *p){(void)p;return 1;}
static int retain(void *p,const struct a733_hdmi_reservation *g){
    struct model *m=p;++m->retains;(void)g;return m->retain_busy?0:1;
}
static void checksum(uint8_t *p){unsigned s=0;for(unsigned i=0;i<127;++i)s+=p[i];p[127]=(uint8_t)(0u-s);}
static void setup(enum a733_hdmi_mode mode,enum a733_hdmi_range range){
    memset(&live,0,sizeof(live));struct model *m=&live.m;
    m->time=100;m->hpd=1;m->owner=&live.s;
    struct a733_hdmi_initial initial={.id=7,.generation=8,
        .words={8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040},
        .resistor_control=0xa5a50000,.resistor_manual=0xabcdef12};
    static const unsigned off[]={0,4,0x20,0x24,0x28,0x2c,0x30,0x34};
    for(unsigned i=0;i<8;++i)m->phy[off[i]/4]=initial.words[i];
    m->sys[0]=initial.resistor_control;m->sys[1]=initial.resistor_manual;
    m->tv[0]=0x80000000;m->tv[0xa4/4]=0x5a5a8000;
    m->top[0]=0x80000000;m->top[8]=0x00200000;
    m->grant=(struct a733_hdmi_reservation){
        .clock={1,2,A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_POWER_DCDC2,A733_HDMI_VO1_SCOPE},
        .generation=3,.rail_owner=4,.rail_generation=5,.system_owner=6,.system_generation=7,
        .inactive_epoch=8,.recovery_epoch=9,.phy_reference_hz=24000000,.sfr_hz=24000000};
    live.c=(struct a733_hdmi_config){
        .ops={now,acquire,validate,read8,write8,read32,write32,barrier,retain},
        .context=m,.request={.initial=initial,.mode=mode,.range=range,.phy_reference_hz=24000000}};
    m->expected=live.c.request;
    static const uint8_t sig[]={0,255,255,255,255,255,255,0};
    memcpy(m->edid,sig,8);m->edid[18]=1;m->edid[19]=4;m->edid[20]=0x80;m->edid[126]=3;
    checksum(m->edid);
    /* Force actual segment1 reads. Earlier blocks remain complete/checksummed. */
    m->edid[128]=0x70;checksum(m->edid+128);
    m->edid[256]=2;m->edid[257]=3;checksum(m->edid+256);
    uint8_t *e=m->edid+384;e[0]=2;e[1]=3;e[2]=range==A733_HDMI_FULL?15:12;
    e[4]=0x41;e[5]=(uint8_t)mode;e[6]=0x65;e[7]=3;e[8]=12;e[9]=0;e[10]=0x10;e[11]=0;
    if(range==A733_HDMI_FULL){e[12]=0xe2;e[13]=0;e[14]=0x40;}checksum(e);
    assert(a733_hdmi_init(&live.s,&live.c)==1);
}
static void capture(void){
    struct a733_hdmi *s=&live.s;
    if(!ddc_saved && s->state==A733_HDMI_EDID && s->phase==4){ddc_checkpoint=live;ddc_saved=1;}
    if(!pll_saved && s->state==A733_HDMI_PHY && s->phy.stage==4){pll_checkpoint=live;pll_saved=1;}
    if(!phy_saved && s->state==A733_HDMI_PHY && s->phy.stage==12 && s->phy.phase==11){phy_checkpoint=live;phy_saved=1;}
    if(!start_saved && s->state==A733_HDMI_STARTING && s->index==1 && live.m.time>=s->until){start_checkpoint=live;start_saved=1;}
}
static void run_to(enum a733_hdmi_state state,int save){
    for(unsigned n=0;n<20000;++n){
        if(save)capture();
        int r=a733_hdmi_poll(&live.s);assert(r>=0);
        if(live.s.state==state)return;
        live.m.time+=10;
    }
    assert(!"bounded model completion");
}
static void range_check(void){
    struct model *m=&live.m;
    assert((m->dw[0x4101]&0xf3)==0x41);
    unsigned a=(m->dw[0x4102]<<8)|m->dw[0x4103];
    unsigned bias=(m->dw[0x4108]<<8)|m->dw[0x4109];
    assert(a==0x1b7c && bias==0x20);
    /* Independent endpoints of the source matrix, scale1, offset half-unit.
     * Hardware sub-LSB rounding is not claimed by this integer model. */
    assert((a*0)/8192+bias/2==16);
    assert((a*255)/8192+bias/2==235);
    if(live.s.io.request.range==A733_HDMI_LIMITED){
        assert((m->dw[0x4004]&1)==1 && !(m->dw[0x101b]&12));
    }else assert(!(m->dw[0x4004]&1) && (m->dw[0x101b]&12)==8);
}
static void print_trace(unsigned mode,unsigned range){
    printf("{\"mode\":%u,\"range\":%u,\"writes\":[",mode,range);
    for(unsigned i=0;i<live.m.events;++i){
        struct event *e=&live.m.trace[i];
        printf("%s[%llu,%u,%u,%llu]",i?",":"",(unsigned long long)e->address,e->width,e->value,
            (unsigned long long)e->time);
    }
    printf("]}\n");
}
static void fault_no_io(void){
    assert(live.s.state==A733_HDMI_FAULT_RETAINED && live.s.io.fault);
    unsigned reads=live.m.reads,writes=live.m.writes,clocks=live.m.now_calls;
    live.m.retain_busy=0;
    assert(a733_hdmi_poll(&live.s)==-1 && live.s.io.retained);
    assert(live.m.reads==reads && live.m.writes==writes && live.m.now_calls==clocks);
}
int main(void){
    setup(A733_HDMI_720P60,A733_HDMI_LIMITED);
    assert(a733_hdmi_prepare(&live.s,1000000)==1);
    run_to(A733_HDMI_PREPARED,1);
    check_tcon_tv(live.m.tv,live.m.top,4);range_check();
    assert(live.m.sys[0]==0xa5a51000 && live.m.sys[1]==0x00cdef12);
    assert(!(live.m.tv[0x90/4]&0x80000000) && (live.m.dw[0x1018]&3)==2);
    assert(a733_hdmi_start(&live.s,2000000)==1);run_to(A733_HDMI_ACTIVE,1);
    assert(a733_hdmi_service(&live.s,3000000)==1);
    print_trace(4,0);
    /* Sampled disconnect enters bounded stop; ownership and rails remain held. */
    live.m.hpd=0;assert(a733_hdmi_service(&live.s,3000000)==0);
    run_to(A733_HDMI_STOPPED_RETAINED,0);
    assert(!(live.m.tv[0x90/4]&0x80000000) && !(live.m.phy[8]&0x08000000));
    assert(live.s.io.reservation.clock.id==1 && live.s.error==A733_HDMI_SINK);
    assert(ddc_saved && pll_saved && phy_saved && start_saved);

    setup(A733_HDMI_1080P60,A733_HDMI_FULL);
    assert(a733_hdmi_prepare(&live.s,1000000)==1);run_to(A733_HDMI_PREPARED,0);
    check_tcon_tv(live.m.tv,live.m.top,16);range_check();
    assert(a733_hdmi_start(&live.s,2000000)==1);run_to(A733_HDMI_ACTIVE,0);
    print_trace(16,1);
    assert(a733_hdmi_stop(&live.s,3000000)==1);run_to(A733_HDMI_STOPPED_RETAINED,0);

    /* Restore captured instances only at their original addresses. No copied
     * driver is executed; these checkpoints avoid repeated nominal preparation. */
    live=start_checkpoint;live.m.expire_unmute=1;live.m.retain_busy=1;
    assert(a733_hdmi_poll(&live.s)==-1);
    assert(live.s.state!=A733_HDMI_ACTIVE && !live.s.io.retained);fault_no_io();
    live=start_checkpoint;live.m.uncertain_unmute=1;
    assert(a733_hdmi_poll(&live.s)==-1 && live.s.io.reservation.clock.id==1);fault_no_io();
    live=start_checkpoint;live.m.top_unlock=1;
    assert(a733_hdmi_poll(&live.s)==-1 && (live.m.dw[0x1018]&3)==2);fault_no_io();
    live=ddc_checkpoint;live.m.dw[0x105]=3;
    assert(a733_hdmi_poll(&live.s)==-1 && live.s.error==A733_HDMI_DDC_ERROR);fault_no_io();
    live=ddc_checkpoint;live.m.time=live.s.transaction_deadline;
    assert(a733_hdmi_poll(&live.s)==-1);fault_no_io();
    live=pll_checkpoint;live.m.time=live.s.phy.operation_deadline;
    assert(a733_hdmi_poll(&live.s)==-1);fault_no_io();
    live=phy_checkpoint;live.m.bad_phy_read=1;
    assert(a733_hdmi_poll(&live.s)==0); /* read high byte; next poll compares */
    assert(a733_hdmi_poll(&live.s)==-1);fault_no_io();
    live=ddc_checkpoint;unsigned old=live.m.writes;
    ++live.s.io.reservation.clock.id;
    assert(a733_hdmi_poll(&live.s)==-1 && live.m.writes==old);fault_no_io();
    live=ddc_checkpoint;copied=live.s;old=live.m.reads;
    assert(a733_hdmi_poll(&copied)==-1 && live.m.reads==old);
    live=ddc_checkpoint;live.m.time=live.s.io.now-1;
    assert(a733_hdmi_poll(&live.s)==-1);fault_no_io();
    live=ddc_checkpoint;
    assert(a733_hdmi_edid_accept(live.m.edid,512,A733_HDMI_720P60,A733_HDMI_LIMITED)==1);
    assert(a733_hdmi_edid_accept(live.m.edid,512,A733_HDMI_720P60,A733_HDMI_FULL)==-1);
    live.m.edid[384+4]=0x5f;checksum(live.m.edid+384);
    assert(a733_hdmi_edid_accept(live.m.edid,512,A733_HDMI_720P60,A733_HDMI_LIMITED)==-1);
    live=ddc_checkpoint;live.m.edid[127]^=1;
    assert(a733_hdmi_edid_accept(live.m.edid,512,A733_HDMI_720P60,A733_HDMI_LIMITED)==-1);
    fprintf(stderr,"composed HDMI/PHY/TV0 and focused retained-failure checks passed\n");
    return 0;
}
