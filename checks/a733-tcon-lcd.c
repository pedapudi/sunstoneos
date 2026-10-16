/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_tcon_lcd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void tcon_reference(const struct a733_tcon_lcd_timing *,const uint32_t *,const uint32_t *,
                    const uint32_t *,const uint32_t *,unsigned);
struct model {
    uint32_t lcd[1024],top[16];
    uint64_t now;
    unsigned calls,fail_at,late_at,reenter_at,corrupt_at,regress_at;
    unsigned admitted,producer,dsi_ready,dsi_stopped,drained;
    struct a733_tcon_lcd *driver;
};
static struct a733_tcon_lcd_timing timing(unsigned burst,unsigned format)
{
    return (struct a733_tcon_lcd_timing){.dsi={.pixel_hz=burst?72000000:80000000,
        .hs_hz=480000000,.ls_hz=80000000,.width=1280,.height=720,
        .hsync=40,.hback=100,.hfront=80,.vsync=5,.vback=20,.vfront=5,
        .lanes=4,.mode=burst,.format=format},.actual_dclk_hz=80000000};
}
static int op(struct model *m)
{
    ++m->calls;
    if(m->calls==m->late_at)m->now=m->driver->deadline;
    if(m->calls==m->regress_at)m->now=0;
    if(m->calls==m->reenter_at)assert(a733_tcon_lcd_poll(m->driver)==A733_TCON_RETAINED);
    return m->calls!=m->fail_at;
}
static int admit(void *arg,const struct a733_tcon_lcd_config *c,enum a733_tcon_lcd_action a)
{
    struct model *m=arg;assert(c->lease==47 && c->timing.actual_dclk_hz==80000000);
    unsigned permitted=m->admitted;
    if(a==A733_TCON_START)permitted&=m->producer && m->dsi_ready;
    if(a==A733_TCON_STOP)permitted&=m->dsi_stopped && m->drained;
    return op(m) && permitted;
}
static uint32_t *reg(struct model *m,uintptr_t addr)
{
    assert(!(addr&3));
    if(addr>=A733_TCON_LCD1_BASE && addr<A733_TCON_LCD1_BASE+4096)return &m->lcd[(addr-A733_TCON_LCD1_BASE)/4];
    assert(addr>=A733_TCON_TOP_BASE && addr<A733_TCON_TOP_BASE+64);
    return &m->top[(addr-A733_TCON_TOP_BASE)/4];
}
static int read32(void *arg,uintptr_t addr,uint32_t *v)
{
    struct model *m=arg;*v=*reg(m,addr);int ok=op(m);
    if(m->calls==m->corrupt_at)*v^=0x80000000;
    return ok;
}
static int write32(void *arg,uintptr_t addr,uint32_t v)
{
    struct model *m=arg;
    assert(addr!=A733_TCON_LCD1_BASE+4 && addr!=A733_TCON_LCD1_BASE+0xfc);
    if(addr==A733_TCON_LCD1_BASE && (v&0x80000000))assert(m->producer && m->dsi_ready);
    if(addr==A733_TCON_LCD1_BASE+0x60)assert(!(v&0x00c00002));
    if(addr==A733_TCON_LCD1_BASE+0x164)assert(!(v&0xffff0000));
    if(addr==A733_TCON_LCD1_BASE+0x44 && m->driver->action==A733_TCON_STOP){
        assert(!(m->lcd[0]&0x80000000) && m->dsi_stopped && m->drained);
    }
    *reg(m,addr)=v; /* Include an applied write followed by uncertain failure. */
    return op(m);
}
static int barrier(void *arg){return op(arg);}
static int now(void *arg,uint64_t *v){struct model *m=arg;*v=m->now++;return 1;}
static void setup(struct model *m,struct a733_tcon_lcd *d,struct a733_tcon_lcd_timing t)
{
    memset(m,0,sizeof(*m));memset(d,0,sizeof(*d));
    m->driver=d;m->admitted=1;m->now=1;
    m->lcd[0x8c/4]=0x0fffffff;m->lcd[0x168/4]=0x00200000;m->lcd[0xfc/4]=0x20000000;
    m->lcd[0x48/4]=0xa000a000; /* Preserve reserved bits in a timing word. */
    m->top[1]=0x10;m->top[3]=0x31;m->top[8]=0x10000; /* Preserve LCD0/PHY0. */
    struct a733_tcon_lcd_config c={.ops={admit,read32,write32,barrier,now},
        .context=m,.lease=47,.timing=t};
    assert(a733_tcon_lcd_init(d,&c,100000));
}
static void drive(struct model *m,struct a733_tcon_lcd *d)
{
    for(unsigned n=0;d->state==A733_TCON_PENDING;++n){
        assert(n<1000);unsigned before=m->calls;
        a733_tcon_lcd_poll(d);assert(m->calls-before<=1);
    }
    unsigned before=m->calls;uint64_t t=m->now;
    assert(a733_tcon_lcd_poll(d)==d->state);
    assert(m->calls==before && m->now==t);
}
static void start(struct model *m,struct a733_tcon_lcd *d)
{
    m->producer=m->dsi_ready=1;
    assert(a733_tcon_lcd_request(d,A733_TCON_START,m->now+10000));drive(m,d);
}
static void images(void)
{
    for(unsigned variant=0;variant!=2;++variant)
    for(unsigned burst=0;burst!=2;++burst)
    for(unsigned format=0;format!=2;++format){
        struct a733_tcon_lcd_timing t=timing(burst,format);
        if(variant){t.dsi.width=640;t.dsi.height=480;t.dsi.hsync=8;t.dsi.hback=16;
            t.dsi.hfront=40;t.dsi.vback=4;t.dsi.vfront=2;}
        struct model m;struct a733_tcon_lcd d;setup(&m,&d,t);
        uint32_t before[1024],top[16];memcpy(before,m.lcd,sizeof(before));memcpy(top,m.top,sizeof(top));
        drive(&m,&d);assert(d.state==A733_TCON_READY && m.calls==61);
        tcon_reference(&t,before,top,m.lcd,m.top,0);
        assert(!(m.lcd[0]&0x80000000) && !(m.lcd[0x40/4]&0x80000000));
        start(&m,&d);assert(d.state==A733_TCON_ACTIVE);
        tcon_reference(&t,before,top,m.lcd,m.top,1);
        assert(a733_tcon_lcd_request(&d,A733_TCON_OBSERVE,m.now+10000));
        drive(&m,&d);assert(d.state==A733_TCON_ACTIVE && d.debug_status==0x20000000);
        m.dsi_stopped=m.drained=1;
        assert(a733_tcon_lcd_request(&d,A733_TCON_STOP,m.now+10000));
        drive(&m,&d);assert(d.state==A733_TCON_STOPPED);
        tcon_reference(&t,before,top,m.lcd,m.top,2);
        assert(!a733_tcon_lcd_request(&d,A733_TCON_START,m.now+10000));
    }
    struct a733_tcon_lcd_plan out,prior;memset(&out,0x5a,sizeof(out));prior=out;
    for(unsigned i=0;i!=8;++i){
        struct a733_tcon_lcd_timing t=timing(0,0);
        switch(i){
        case 0:t.actual_dclk_hz++;break;
        case 1:t.dsi.vback=3;break;
        case 2:t.dsi.vfront=1;break;
        case 3:t.dsi.height=1081;break;
        case 4:t.dsi.hsync=4;t.dsi.hback=5;t.dsi.hfront=20;break;
        case 5:t.dsi.hsync=1025;break;
        case 6:t.dsi.vfront=4000;break;
        default:t.dsi.lanes=3;break;
        }
        assert(!a733_tcon_lcd_plan(&t,&out));assert(!memcmp(&out,&prior,sizeof(out)));
    }
}
static void faults(void)
{
    struct model m;struct a733_tcon_lcd d;
    static const unsigned boundaries[]={1,3,18,19,20,21,58,60,61};
    for(unsigned kind=0;kind!=4;++kind)
    for(unsigned i=0;i!=sizeof(boundaries)/sizeof(*boundaries);++i){
        setup(&m,&d,timing(0,0));
        if(kind==0)m.fail_at=boundaries[i];
        if(kind==1)m.late_at=boundaries[i];
        if(kind==2)m.reenter_at=boundaries[i];
        if(kind==3)m.regress_at=boundaries[i];
        drive(&m,&d);assert(d.state==A733_TCON_RETAINED && d.config.lease==47);
    }
    for(unsigned action=A733_TCON_START;action<=A733_TCON_STOP;++action){
        unsigned count=action==A733_TCON_START?28:action==A733_TCON_OBSERVE?12:25;
        for(unsigned kind=0;kind!=2;++kind)
        for(unsigned n=1;n<=count;++n){
            setup(&m,&d,timing(1,1));drive(&m,&d);
            if(action!=A733_TCON_START){start(&m,&d);assert(d.state==A733_TCON_ACTIVE);}
            m.producer=m.dsi_ready=m.dsi_stopped=m.drained=1;
            if(kind==0)m.fail_at=m.calls+n;else m.late_at=m.calls+n;
            assert(a733_tcon_lcd_request(&d,action,m.now+10000));drive(&m,&d);
            assert(d.state==A733_TCON_RETAINED);
        }
    }
    const struct {uintptr_t address;uint32_t bit;} bad[]={
        {A733_TCON_LCD1_BASE,0x80000000},{A733_TCON_LCD1_BASE+4,0x200},
        {A733_TCON_LCD1_BASE+0x60,2},{A733_TCON_LCD1_BASE+0x3c,0x80000000},
        {A733_TCON_LCD1_BASE+0x100,0x80000000},{A733_TCON_LCD1_BASE+0x230,0x100},
        {A733_TCON_TOP_BASE+0x20,0x20000}};
    for(unsigned i=0;i!=sizeof(bad)/sizeof(*bad);++i){
        setup(&m,&d,timing(0,0));*reg(&m,bad[i].address)|=bad[i].bit;drive(&m,&d);
        assert(d.state==A733_TCON_RETAINED && !d.writes_attempted);
    }
    setup(&m,&d,timing(0,0));m.top[3]&=~0x20u;drive(&m,&d);
    assert(d.state==A733_TCON_RETAINED && !d.writes_attempted);
    setup(&m,&d,timing(0,0));m.corrupt_at=21;drive(&m,&d);assert(d.error==A733_TCON_VERIFY);
    setup(&m,&d,timing(0,0));drive(&m,&d);
    assert(a733_tcon_lcd_request(&d,A733_TCON_START,m.now+1000));drive(&m,&d);
    assert(d.error==A733_TCON_AUTHORITY && !(m.lcd[0]&0x80000000));
    for(unsigned i=0;i!=2;++i){
        setup(&m,&d,timing(0,0));drive(&m,&d);start(&m,&d);
        m.dsi_stopped=i;m.drained=!i;
        unsigned before=d.writes_attempted;
        assert(a733_tcon_lcd_request(&d,A733_TCON_STOP,m.now+1000));drive(&m,&d);
        assert(d.error==A733_TCON_AUTHORITY && before==d.writes_attempted);
    }
    for(unsigned i=0;i!=2;++i){
        setup(&m,&d,timing(0,0));drive(&m,&d);start(&m,&d);
        m.lcd[i?1:0xfc/4]|=i?0x200:0x80000000;
        assert(a733_tcon_lcd_request(&d,A733_TCON_OBSERVE,m.now+1000));drive(&m,&d);
        assert(d.error==A733_TCON_UNDERFLOW);
    }
    setup(&m,&d,timing(0,0));drive(&m,&d);start(&m,&d);
    m.lcd[0xfc/4]|=42u<<16;m.lcd[1]|=0x8002; /* Ordinary events are retained. */
    assert(a733_tcon_lcd_request(&d,A733_TCON_OBSERVE,m.now+1000));drive(&m,&d);
    assert(d.state==A733_TCON_ACTIVE && ((d.debug_status>>16)&4095)==42 && d.irq_status==0x8002);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"faults")){faults();puts("PASS: TCON lifecycle fault model after correcting simulated clock origin");return 0;}
    images();faults();
    puts("PASS: eight TCON source profiles through prepare/start/stop; manual phase0 and deferred-enable deltas; exact clock/field guards; shared-word preservation; producer/drain admission; bounded failures and raw status");
    return 0;
}
