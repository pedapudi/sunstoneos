/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_display_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void de_reference(const struct a733_de_config *, const uint32_t *, const uint32_t *, unsigned);
static struct model {
    uint32_t regs[0x2ac000/4], initial[0x8200/4];
    struct a733_de *driver;
    uint64_t now;
    unsigned calls, fail_at, late_at, regress_at, reenter_at, corrupt_at;
    unsigned admitted, inactive, dsi_stopped;
    /* Replay model: free-running FRAME_END every frame_period status reads,
     * optionally ignored W1C clears, and RCQ completion on submission. */
    unsigned replay, status_reads, frame_period, stuck_clear, polls, races4, races17;
} m;
static int op(void)
{
    ++m.calls;
    if(m.calls==m.late_at)m.now=m.driver->deadline;
    if(m.calls==m.regress_at)m.now=0;
    if(m.calls==m.reenter_at)assert(a733_de_poll(m.driver)==A733_DE_FAULT_RETAINED);
    return m.calls!=m.fail_at;
}
static int admit(void *ctx,const struct a733_de_config *c,enum a733_de_action a)
{
    assert(ctx==&m && c->lease==87 && c->surface.owner==97);
    unsigned permit=m.admitted;
    if(a==A733_DE_PREPARE || a==A733_DE_START)permit&=m.inactive;
    if(a==A733_DE_STOP)permit&=m.dsi_stopped;
    return op() && permit;
}
static uint32_t *reg(uintptr_t addr)
{
    assert(!(addr&3) && addr>=A733_DE_BASE && addr<A733_DE_BASE+sizeof(m.regs));
    return &m.regs[(addr-A733_DE_BASE)/4];
}
static int read32(void *ctx,uintptr_t addr,uint32_t *v)
{
    assert(ctx==&m);
    if(m.replay && addr==A733_DE_BASE+0x8144 && m.frame_period && ++m.status_reads%m.frame_period==0)
        *reg(addr)|=1;
    *v=*reg(addr);
    if(m.replay && addr==A733_DE_BASE+0x8144 && (*v&1)){m.races4+=m.driver->step==4;m.races17+=m.driver->step==17;}
    int ok=op();if(m.calls==m.corrupt_at)*v^=0x80000000;return ok;
}
static int write32(void *ctx,uintptr_t addr,uint32_t v)
{
    assert(ctx==&m);uint32_t off=addr-A733_DE_BASE;
    if(m.replay && m.driver->action==A733_DE_REPLAY){
        assert(off==0x8144 || off==0x8154 || off==0x8158 || off==0x815c);
        if(off==0x8144){if(!m.stuck_clear)*reg(addr)&=~(v&13u);}else *reg(addr)=v;
        return op();
    }
    assert(off!=0x8000 && off!=0x8004 && off!=0x8008 && off!=0x8144 && off!=0x8150);
    if(m.driver->action==A733_DE_PREPARE)assert(m.inactive && !m.regs[0x8140/4]);
    if(off==0x8140 && (v&1))assert(m.inactive);
    if(m.driver->action==A733_DE_STOP)assert(m.dsi_stopped && off==0x8140 && !(v&1));
    *reg(addr)=v;return op(); /* Failed callback may already have changed hardware. */
}
static int barrier(void *ctx){assert(ctx==&m);return op();}
static int rcq_submit(void *ctx)
{
    assert(ctx==&m && m.replay && m.driver->action==A733_DE_REPLAY);
    /* Model RCQ completion: the queue writes word8 of the selected header to
     * UCH3 layer0 low address and latches RCQ_FINISH. */
    uint64_t header=*reg(A733_DE_BASE+0x8154),base=m.driver->config.surface.device_address;
    unsigned slot=(unsigned)((header-base-2*m.driver->replay_frame_bytes)/4096);
    *reg(A733_DE_BASE+0x221010)=(uint32_t)(base+slot*m.driver->replay_frame_bytes);
    *reg(A733_DE_BASE+0x8144)|=4;return op();
}
static int now(void *ctx,uint64_t *v){assert(ctx==&m);*v=m.now++;return 1;}
static struct a733_tcon_lcd_timing timing(void)
{
    return (struct a733_tcon_lcd_timing){.dsi={.pixel_hz=80000000,.hs_hz=480000000,.ls_hz=80000000,
        .width=1280,.height=720,.hsync=40,.hback=100,.hfront=80,.vsync=5,.vback=20,.vfront=5,
        .lanes=4,.mode=0,.format=0},.actual_dclk_hz=80000000};
}
static void setup(struct a733_de *d,unsigned variant)
{
    memset(&m,0,sizeof(m));memset(d,0,sizeof(*d));m.now=1;m.driver=d;m.admitted=m.inactive=1;
    m.regs[0x8000/4]=m.regs[0x8004/4]=0x10010;m.regs[0x8008/4]=0x11;
    m.regs[0x8010/4]=0xa432;m.regs[0x802c/4]=0xabc02345;m.regs[0x8034/4]=0x07654320;
    m.regs[0x8080/4]=0xabcd1234;m.regs[0x814c/4]=0x80;
    memcpy(m.initial,m.regs,sizeof(m.initial));
    struct a733_de_config c={.ops={admit,read32,write32,barrier,now,0},.context=&m,.lease=87,
        .timing=timing(),.surface={.device_address=0x50000000,.mapped_bytes=5120*720,.owner=97,.pitch=5120}};
    if(variant==1){c.surface.device_address=UINT64_C(0x8012340000);c.surface.pitch=5184;c.surface.mapped_bytes=5184*720;}
    if(variant==2){c.timing.dsi.pixel_hz=72000000;c.timing.dsi.mode=1;c.timing.dsi.format=1;}
    if(variant==3){c.timing.dsi.width=640;c.timing.dsi.height=480;c.surface.pitch=4096;
        c.surface.mapped_bytes=4096*480;c.surface.device_address=(UINT64_C(1)<<40)-c.surface.mapped_bytes;}
    assert(a733_de_init(d,&c,100000));
}
static void drive(struct a733_de *d)
{
    for(unsigned n=0;d->state==A733_DE_PENDING;++n){
        assert(n<1000);unsigned calls=m.calls;a733_de_poll(d);assert(m.calls-calls<=1);++m.polls;
    }
    unsigned calls=m.calls;uint64_t time=m.now;
    assert(a733_de_poll(d)==d->state && m.calls==calls && m.now==time);
}
static void images(void)
{
    for(unsigned v=0;v<4;v++){
        struct a733_de d;setup(&d,v);drive(&d);assert(d.state==A733_DE_READY);
        /* Admission, 25 initial checks, route, 99 full-mask words written and
         * verified in two polls, four masked words in three polls, barrier. */
        assert(m.polls==1+25+1+99*2+4*3+1);
        de_reference(&d.config,m.initial,m.regs,0);
        assert(d.plan.last_pixel_address==d.config.surface.device_address+
            (uint64_t)(d.config.timing.dsi.height-1)*d.config.surface.pitch+4*d.config.timing.dsi.width-1);
        assert(a733_de_request(&d,A733_DE_START,m.now+10000));drive(&d);assert(d.state==A733_DE_ARMED);
        de_reference(&d.config,m.initial,m.regs,1);
        m.regs[0x8144/4]=0x0192000d;m.inactive=0;
        assert(a733_de_request(&d,A733_DE_OBSERVE,m.now+10000));drive(&d);
        assert(d.state==A733_DE_ARMED && d.raw_status==0x0192000d && m.regs[0x8144/4]==0x0192000d);
        m.regs[0x8144/4]=0;m.dsi_stopped=1;
        assert(a733_de_request(&d,A733_DE_STOP,m.now+10000));drive(&d);
        assert(d.state==A733_DE_DISABLED_RETAINED && d.config.surface.owner==97 && d.config.lease==87);
        de_reference(&d.config,m.initial,m.regs,2);
        assert(m.regs[0x221000/4]==0xff000403); /* Frame remains programmed and retained. */
        assert(!a733_de_request(&d,A733_DE_START,m.now+10000));
    }
    struct a733_de_plan p,before;memset(&p,0x5a,sizeof(p));before=p;
    for(unsigned i=0;i<9;i++){
        struct a733_tcon_lcd_timing t=timing();struct a733_de_surface s={0x50000000,5120*720,97,5120};
        switch(i){
        case 0:s.device_address=UINT64_C(1)<<40;break;
        case 1:s.device_address=(UINT64_C(1)<<40)-4;break;
        case 2:s.mapped_bytes--;break;
        case 3:s.pitch=5116;break;
        case 4:s.pitch=5121;break;
        case 5:s.device_address++;break;
        case 6:s.owner=0;break;
        case 7:t.actual_dclk_hz++;break;
        default:s.mapped_bytes=UINT64_MAX;break;
        }
        assert(!a733_de_plan(&t,&s,&p) && !memcmp(&p,&before,sizeof(p)));
    }
}
static void faults(void)
{
    struct a733_de d;
    setup(&d,0);m.admitted=0;drive(&d);assert(d.error==A733_DE_AUTHORITY && !d.writes_attempted);
    setup(&d,0);m.regs[0x2a2804/4]=1;drive(&d);assert(d.error==A733_DE_INITIAL_STATE && !d.writes_attempted);
    setup(&d,0);m.regs[0x8010/4]=(m.regs[0x8010/4]&~0xf00u)|0x100;drive(&d);assert(d.error==A733_DE_INITIAL_STATE && !d.writes_attempted);
    setup(&d,0);m.regs[0x224000/4]=1;drive(&d);assert(d.error==A733_DE_INITIAL_STATE && !d.writes_attempted);
    setup(&d,0);m.regs[0x8150/4]=1;drive(&d);assert(d.error==A733_DE_INITIAL_STATE && !d.writes_attempted);
    /* First uncertain write, readback mismatch, post-write deadline, clock
     * regression and reentry are distinct terminal fault mechanisms. */
    for(unsigned kind=0;kind<5;kind++){
        setup(&d,0);
        while(d.step!=3)a733_de_poll(&d); /* First image word is written without pre-read. */
        unsigned next=m.calls+1;
        if(kind==0)m.fail_at=next;
        if(kind==1)m.corrupt_at=next+1;
        if(kind==2)m.late_at=next;
        if(kind==3)m.regress_at=next;
        if(kind==4)m.reenter_at=next;
        drive(&d);assert(d.state==A733_DE_FAULT_RETAINED && d.writes_attempted==1);
        assert(d.config.surface.owner==97 && d.config.lease==87 && m.regs[0x221000/4]==0xff000403);
    }
    setup(&d,0);drive(&d);m.inactive=0;
    assert(a733_de_request(&d,A733_DE_START,m.now+10000));drive(&d);assert(d.error==A733_DE_AUTHORITY);
    setup(&d,0);drive(&d);assert(a733_de_request(&d,A733_DE_START,m.now+10000));drive(&d);
    assert(a733_de_request(&d,A733_DE_STOP,m.now+10000));drive(&d);
    assert(d.error==A733_DE_AUTHORITY && m.regs[0x8140/4]==1);
}
static void replay_setup(struct a733_de *d)
{
    setup(d,0);memset(d,0,sizeof(*d));m.regs[0x8010/4]=0xa132;
    uint64_t f=2560*720;
    struct a733_de_config c={.ops={admit,read32,write32,barrier,now,rcq_submit},.context=&m,.lease=87,
        .surface={.device_address=0x50000000,.mapped_bytes=2*f+8192,.owner=97,.pitch=2560},
        .output=A733_DE_TV0_RGB565};
    assert(a733_de_init(d,&c,100000));drive(d);assert(d->state==A733_DE_READY);
    assert(a733_de_request(d,A733_DE_START,m.now+10000));drive(d);assert(d->state==A733_DE_ARMED);
    m.inactive=0;m.replay=1;
}
static void replays(void)
{
    struct a733_de d;uint64_t f=2560*720;
    /* A frame-end on every Nth status read lands at the step4 or step17
     * readbacks for some phases. Each must complete with a frame-end that
     * followed RCQ completion and never fault. */
    unsigned races4=0,races17=0;
    for(unsigned period=2;period<8;period++)for(unsigned phase=0;phase<period;phase++)for(unsigned slot=0;slot<2;slot++){
        replay_setup(&d);m.frame_period=period;m.status_reads=phase;
        assert(a733_de_replay_request(&d,slot,f,m.now+10000));drive(&d);
        assert(d.state==A733_DE_ARMED && d.error==A733_DE_OK && (d.raw_status&1));
        assert(d.replay_rcq_done && d.replay_frame_end>d.replay_rcq_done);
        assert(m.regs[0x221010/4]==(uint32_t)(0x50000000+slot*f));
        races4+=m.races4;races17+=m.races17;
    }
    assert(races4 && races17);
    /* FRAME_END set at every readback, as if a frame boundary followed each
     * clear: the step17 retry never accepts the readback-set bit as the
     * awaited frame-end and ends at the deadline. */
    replay_setup(&d);m.frame_period=1;
    assert(a733_de_replay_request(&d,0,f,m.now+1000));
    drive(&d);assert(d.state==A733_DE_FAULT_RETAINED && d.error==A733_DE_TIME && !d.replay_frame_end);
    /* A clear that never takes effect is bounded by the deadline as well. */
    replay_setup(&d);m.stuck_clear=1;m.regs[0x8144/4]=1;
    assert(a733_de_replay_request(&d,1,f,m.now+1000));
    drive(&d);assert(d.state==A733_DE_FAULT_RETAINED && d.error==A733_DE_TIME && !d.replay_frame_end);
    /* A stale RCQ_FINISH that survives the step3 clear still faults at step4. */
    replay_setup(&d);m.stuck_clear=1;m.regs[0x8144/4]=4;
    assert(a733_de_replay_request(&d,0,f,m.now+1000));
    drive(&d);assert(d.state==A733_DE_FAULT_RETAINED && d.error==A733_DE_VERIFY && !d.replay_rcq_done);
}
int main(void)
{
    images();puts("four full source image/lifecycle comparisons and address/timing rejection cases passed");
    faults();puts("ownership, active RCQ, bypass, uncertain write, readback, deadline and reentry cases passed");
    replays();puts("replay with free-running frame-end, persistent frame-end, ignored clear and stale RCQ_FINISH cases passed");
    return 0;
}
