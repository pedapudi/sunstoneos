#include "display_scanout.h"
#include "a733_dsi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct display_scanout s;
static struct model {
    uint32_t de[0x2ac000/4], lcd[1024], top[16], host[256], phy[128], ccu[2];
    struct a733_dsi command;
    uint64_t now;
    unsigned calls, authenticated[18], panel_steps, stopped_reads, resource_steps, frame_steps;
    unsigned drain_steps, output_steps, panel_done, de_drained, output_quiet;
    unsigned deny_auth, change_resource, frame_bad_span, drain_bad_generation;
    unsigned pending_forever, fail_write, late_write, reenter_write, fail_drain;
} m;
struct snapshot {struct display_scanout controller;struct model model;};
static struct snapshot *ready_copy,*active_copy,*drain_copy;
static void save(struct snapshot **p)
{
    *p=malloc(sizeof(**p));assert(*p);memcpy(&(*p)->controller,&s,sizeof(s));memcpy(&(*p)->model,&m,sizeof(m));
}
static void restore(const struct snapshot *p)
{
    memcpy(&s,&p->controller,sizeof(s));memcpy(&m,&p->model,sizeof(m));
}
static int now(void *ctx,uint64_t *v){assert(ctx==&m);*v=m.now++;return 1;}
static int resources(void *ctx,const struct display_request *r,uint64_t deadline,struct display_resource_token *t)
{
    assert(ctx==&m && m.now<deadline);++m.calls;++m.resource_steps;
    if(!t->id)*t=(struct display_resource_token){.id=101,.generation=2,.owner=r->owner,.board_id=r->board_id,
        .panel_id=r->panel_id,.recovery_generation=3,.scopes=DISPLAY_REQUIRED_SCOPES,.reference_hz=24000000,
        .host_hz=150000000,.hs_hz=r->timing.dsi.hs_hz,.ls_hz=r->timing.dsi.ls_hz};
    else if(m.change_resource)t->generation++;
    return m.resource_steps>1;
}
static int frame(void *ctx,const struct display_request *r,const struct display_resource_token *rt,
                 uint64_t deadline,struct display_frame_token *t)
{
    assert(ctx==&m && rt->id==101 && m.now<deadline);++m.calls;++m.frame_steps;
    *t=(struct display_frame_token){.id=102,.generation=5,.owner=r->owner,.frame_id=r->frame.frame_id,
        .mapping_generation=6,.visibility_generation=7,.cpu_address=r->frame.cpu_address,
        .device_address=r->frame.device_address,.mapped_bytes=r->frame.mapped_bytes,
        .pitch=r->frame.pitch,.cache_state=DISPLAY_CACHE_IMMUTABLE_DEVICE_READ};
    if(m.frame_bad_span)t->mapped_bytes-=64;
    return 1;
}
static int authenticate(void *ctx,const struct display_request *r,const struct display_ownership *o,
                        enum display_phase phase,uint64_t deadline)
{
    assert(ctx==&m && m.now<deadline);++m.calls;++m.authenticated[phase];
    assert(o->resources.id==101 && o->resources.generation==2 && o->frame.id==102 && o->frame.generation==5);
    assert(o->frame.mapping_generation==6 && o->frame.visibility_generation==7);
    assert(o->frame.device_address==r->frame.device_address && o->frame.cpu_address==r->frame.cpu_address);
    if(phase==DISPLAY_START_LCD)assert(s.de.state==A733_DE_ARMED && s.dsi.state==A733_DSI_VIDEO_READY);
    if(phase==DISPLAY_START_DSI)assert(s.lcd.state==A733_TCON_ACTIVE && s.de.state==A733_DE_ARMED && m.panel_done);
    if(phase==DISPLAY_STOP_DE)assert(s.dsi.state==A733_DSI_VIDEO_STOPPED && s.lcd.state==A733_TCON_ACTIVE);
    if(phase==DISPLAY_STOP_LCD)assert(m.de_drained && o->de_drain.id==201 && o->de_drain.frame_generation==5);
    return !m.deny_auth;
}
static uint32_t *reg(uintptr_t address)
{
    assert(!(address&3));
    if(address>=A733_DE_BASE && address<A733_DE_BASE+sizeof(m.de))return &m.de[(address-A733_DE_BASE)/4];
    if(address>=A733_TCON_LCD1_BASE && address<A733_TCON_LCD1_BASE+sizeof(m.lcd))return &m.lcd[(address-A733_TCON_LCD1_BASE)/4];
    if(address>=A733_TCON_TOP_BASE && address<A733_TCON_TOP_BASE+sizeof(m.top))return &m.top[(address-A733_TCON_TOP_BASE)/4];
    if(address>=0x05508000 && address<0x05508000+sizeof(m.host))return &m.host[(address-0x05508000)/4];
    if(address>=0x05509000 && address<0x05509000+sizeof(m.phy))return &m.phy[(address-0x05509000)/4];
    assert(address==0x02003588 || address==0x0200358c);return &m.ccu[(address-0x02003588)/4];
}
static int read32(void *ctx,const struct display_resource_token *t,uintptr_t a,uint32_t *v)
{
    assert(ctx==&m && t->id==101);++m.calls;
    if(a==0x05508010 && (m.host[4]&1)){
        if(s.phase==DISPLAY_PREPARE_PANEL)m.host[4]&=~1u;
        if(s.phase==DISPLAY_STOP_DSI && (m.host[0x4c/4]&0x10000000) && ++m.stopped_reads==2)m.host[4]&=~1u;
    }
    *v=*reg(a);return 1;
}
static int write32(void *ctx,const struct display_resource_token *t,uintptr_t a,uint32_t v)
{
    assert(ctx==&m && t->id==101);++m.calls;
    if(a==A733_DE_BASE+0x8140 && (v&1))assert(s.lcd.state==A733_TCON_READY && s.dsi.state==A733_DSI_VIDEO_READY);
    if(a==A733_TCON_LCD1_BASE && (v&0x80000000))assert(s.de.state==A733_DE_ARMED);
    if(a==0x05508010 && (v&1) && s.phase==DISPLAY_START_DSI)assert(s.lcd.state==A733_TCON_ACTIVE);
    if(s.phase==DISPLAY_STOP_DE)assert(s.dsi.state==A733_DSI_VIDEO_STOPPED && s.lcd.state==A733_TCON_ACTIVE);
    if(s.phase==DISPLAY_STOP_LCD)assert(m.de_drained);
    *reg(a)=v;
    if(m.late_write)m.now=s.deadline;
    if(m.reenter_write)assert(display_scanout_poll(&s)==DISPLAY_SCANOUT_FAULT_RETAINED);
    return !m.fail_write;
}
static int barrier(void *ctx,const struct display_resource_token *t){assert(ctx==&m && t->id==101);++m.calls;return 1;}
static int lp_admit(void *ctx,unsigned controller,unsigned lanes,uint64_t lease)
{
    assert(ctx==&m && controller==1 && lanes==4 && lease==101 && s.phase==DISPLAY_PREPARE_PANEL);
    ++m.calls;return a733_dsi_phy_ready(&s.phy,controller,lanes,lease);
}
static int lp_read(void *ctx,uintptr_t a,uint32_t *v){return read32(ctx,&s.ownership.resources,a,v);}
static int lp_write(void *ctx,uintptr_t a,uint32_t v){return write32(ctx,&s.ownership.resources,a,v);}
static int lp_barrier(void *ctx){return barrier(ctx,&s.ownership.resources);}
static int panel(void *ctx,const struct display_request *r,const struct display_ownership *o,
                 const struct a733_dsi_phy *phy,uint64_t deadline)
{
    assert(ctx==&m && r->panel_id==9 && o->resources.id==101 && phy==&s.phy);++m.panel_steps;
    if(!m.command.initialized){
        struct a733_dsi_config c={.ops={lp_admit,lp_read,lp_write,lp_barrier,now},.context=&m,
            .lease=101,.controller=1,.lanes=4};
        unsigned char command=0; /* Model panel NOP; no production panel sequence. */
        assert(a733_dsi_init(&m.command,&c));assert(a733_dsi_submit(&m.command,0x05,0,&command,1,deadline));return 0;
    }
    enum a733_dsi_state q=a733_dsi_poll(&m.command);
    if(q==A733_DSI_RETAINED)return -1;
    if(q==A733_DSI_ENGINE_DONE){m.panel_done=1;return 1;}
    return 0;
}
static struct display_proof proof(const struct display_ownership *o,enum display_proof_kind kind)
{
    return (struct display_proof){.id=kind==DISPLAY_PROOF_DE_DRAIN?201:202,.generation=8,
        .resource_id=o->resources.id,.resource_generation=o->resources.generation,
        .frame_id=o->frame.id,.frame_generation=o->frame.generation,.completed_us=m.now,.kind=kind};
}
static int drain(void *ctx,const struct display_request *r,const struct display_ownership *o,
                 uint64_t deadline,struct display_proof *p)
{
    assert(ctx==&m && r->frame.frame_id==11 && m.now<deadline);++m.calls;++m.drain_steps;
    assert(s.dsi.state==A733_DSI_VIDEO_STOPPED && s.de.state==A733_DE_DISABLED_RETAINED && s.lcd.state==A733_TCON_ACTIVE);
    if(m.fail_drain)return -1;
    if(m.pending_forever || m.drain_steps<3)return 0;
    *p=proof(o,DISPLAY_PROOF_DE_DRAIN);if(m.drain_bad_generation)p->frame_generation++;
    m.de_drained=1;return 1;
}
static int quiet(void *ctx,const struct display_request *r,const struct display_ownership *o,
                 uint64_t deadline,struct display_proof *p)
{
    assert(ctx==&m && r->panel_id==9 && m.now<deadline);++m.calls;++m.output_steps;
    assert(m.de_drained && s.lcd.state==A733_TCON_STOPPED && s.dsi.state==A733_DSI_VIDEO_STOPPED);
    if(m.output_steps<2)return 0;
    *p=proof(o,DISPLAY_PROOF_OUTPUT_QUIET);m.output_quiet=1;return 1;
}
static void setup(void)
{
    memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.now=1;
    struct display_request r={.owner=4,.board_id=8,.panel_id=9,
        .timing={.dsi={.pixel_hz=80000000,.hs_hz=480000000,.ls_hz=80000000,.width=1280,.height=720,
            .hsync=40,.hback=100,.hfront=80,.vsync=5,.vback=20,.vfront=5,.lanes=4},.actual_dclk_hz=80000000},
        .frame={.cpu_address=0x40000000,.device_address=0x50000000,.mapped_bytes=5120*720,.frame_id=11,.pitch=5120}};
    struct display_provider p={now,resources,frame,authenticate,read32,write32,barrier,panel,drain,quiet};
    struct a733_dsi_pll_plan pll;assert(a733_dsi_pll_plan(480000000,80000000,&pll));
    m.ccu[0]=0x82000000;m.ccu[1]=0x10001;m.phy[0x104/4]=pll.factors|0x00700080;
    m.phy[0x108/4]=0x00601000;m.phy[0xe0/4]=1u<<26;m.top[3]=0x20;
    m.de[0x8000/4]=m.de[0x8004/4]=0x10010;m.de[0x8008/4]=0x11;
    assert(display_scanout_init(&s,&r,&p,&m));assert(!m.calls);
    assert(display_scanout_prepare(&s,100000));
}
static void tick(void)
{
    unsigned calls=m.calls;display_scanout_poll(&s);assert(m.calls-calls<=1);
}
static void drive(void)
{
    for(unsigned n=0;s.state==DISPLAY_SCANOUT_PENDING;n++){assert(n<4000);tick();}
    unsigned calls=m.calls;uint64_t n=m.now;
    assert(display_scanout_poll(&s)==s.state && m.calls==calls && m.now==n);
}
static void lifecycle(void)
{
    setup();drive();
    if(s.state!=DISPLAY_SCANOUT_READY)fprintf(stderr,"prepare failed phase%d err%d phy%d dsi%d lcd%d de%d\n",s.failed_phase,s.error,s.phy.error,s.dsi.error,s.lcd.error,s.de.error);
    assert(s.state==DISPLAY_SCANOUT_READY && m.panel_done && m.command.state==A733_DSI_ENGINE_DONE);
    save(&ready_copy);
    assert(display_scanout_request(&s,DISPLAY_SCANOUT_START,m.now+10000));drive();assert(s.state==DISPLAY_SCANOUT_ACTIVE);
    assert(display_scanout_request(&s,DISPLAY_SCANOUT_OBSERVE,m.now+10000));drive();assert(s.state==DISPLAY_SCANOUT_ACTIVE);
    save(&active_copy);
    assert(display_scanout_request(&s,DISPLAY_SCANOUT_STOP,m.now+10000));
    while(s.state==DISPLAY_SCANOUT_PENDING && s.phase!=DISPLAY_DRAIN_DE)tick();
    assert(s.phase==DISPLAY_DRAIN_DE && s.state==DISPLAY_SCANOUT_PENDING);save(&drain_copy);
    drive();assert(s.state==DISPLAY_SCANOUT_STOPPED_RETAINED && m.de_drained && m.output_quiet);
    assert(s.ownership.resources.id==101 && s.ownership.frame.id==102 && s.phy.state==A733_DSI_PHY_READY);
    assert(!display_scanout_prepare(&s,m.now+10000));assert(!display_scanout_request(&s,DISPLAY_SCANOUT_START,m.now+10000));
}
static void faults(void)
{
    setup();m.change_resource=1;drive();assert(s.error==DISPLAY_SCANOUT_IDENTITY);
    assert(s.ownership.resources.generation==2 && s.observed_resources.generation==3 && !s.phy.initialized);
    setup();m.frame_bad_span=1;drive();assert(s.error==DISPLAY_SCANOUT_IDENTITY && s.ownership.frame.id==102 && !s.phy.initialized);
    for(unsigned kind=0;kind<4;kind++){
        restore(ready_copy);assert(display_scanout_request(&s,DISPLAY_SCANOUT_START,m.now+10000));
        if(!kind)m.deny_auth=1;
        if(kind==1)m.fail_write=1;
        if(kind==2)m.late_write=1;
        if(kind==3)m.reenter_write=1;
        drive();assert(s.state==DISPLAY_SCANOUT_FAULT_RETAINED && s.failed_phase==DISPLAY_START_DE);
        assert(s.ownership.resources.id==101 && s.ownership.frame.id==102 && s.lcd.state==A733_TCON_READY);
    }
    restore(active_copy);assert(display_scanout_request(&s,DISPLAY_SCANOUT_STOP,m.now+10000));
    m.host[0x2f0/4]=2;drive();assert(s.state==DISPLAY_SCANOUT_FAULT_RETAINED && s.failed_phase==DISPLAY_STOP_DSI);
    assert(s.de.state==A733_DE_ARMED && s.lcd.state==A733_TCON_ACTIVE);
    for(unsigned kind=0;kind<3;kind++){
        restore(drain_copy);
        if(!kind)m.drain_bad_generation=1;
        if(kind==1)m.fail_drain=1;
        if(kind==2){m.pending_forever=1;s.deadline=m.now+30;}
        drive();assert(s.state==DISPLAY_SCANOUT_FAULT_RETAINED && s.failed_phase==DISPLAY_DRAIN_DE);
        assert(s.lcd.state==A733_TCON_ACTIVE && s.ownership.frame.id==102 && !m.output_quiet);
    }
}
int main(void)
{
    lifecycle();puts("actual PHY, LP command, DSI, LCD and DE lifecycle composition passed");
    faults();puts("token mutation/span, authority, uncertain write, timeout, reentry and independent-drain failures retained ownership");
    free(ready_copy);free(active_copy);free(drain_copy);return 0;
}
