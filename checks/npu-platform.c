#include "a733_npu_platform.h"
#include "npu-platform-reference.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
#define READY A733_NPU_PLATFORM_READY
#define BUSY A733_NPU_PLATFORM_BUSY
#define PENDING A733_NPU_PLATFORM_PENDING
#define RETAINED A733_NPU_PLATFORM_RETAINED
struct reg {uintptr_t address;uint32_t value;};
struct model {
    struct a733_npu_platform p;
    struct a733_clock_leases clocks;
    struct a733_npu_platform_config config;
    struct reg regs[64],writes[32];
    unsigned count,nwrite,reads,lock,lock_busy,calls,acquires,partial,corrupt,refuse;
    unsigned no_ack,fail_write,late_final,late_unlock,fail_unlock,regress,acquire_failed;
    uint64_t now,floor;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for(unsigned i=0;i<m->count;++i)if(m->regs[i].address==a)return &m->regs[i].value;
    assert(m->count<64);m->regs[m->count].address=a;return &m->regs[m->count++].value;
}
static int now(void *v,uint64_t *out)
{
    struct model *m=v;++m->calls;
    assert(!m->acquire_failed);
    if(m->now<m->floor)return 0;
    m->floor=m->now;*out=m->now;return 1;
}
static enum a733_npu_platform_result acquire(void *v,const struct a733_npu_platform_config *c,
    a733_clock_mask_t shared,struct a733_npu_platform_resources *r)
{
    struct model *m=v;++m->calls;++m->acquires;assert(!m->lock);
    if(memcmp(c,&m->config,sizeof(*c)) || m->refuse){m->acquire_failed=1;return RETAINED;}
    if(!r->clock.id) {
        enum a733_clock_result q=a733_clock_acquire(&m->clocks,c->owner,shared,A733_CLOCK_NPU,c->deadline_us,&r->clock);
        if(q==A733_CLOCK_BUSY)return BUSY;
        if(q!=A733_CLOCK_OK)return RETAINED;
    }
    if(m->partial && m->acquires==1)return PENDING;
    r->pck=31;
    if(m->corrupt)++r->clock.id;
    if(m->partial==2){m->acquire_failed=1;return RETAINED;}
    r->rail=45;r->supply_uv=m->partial==3 ? 810000 : SOURCE_UV;
    if(m->regress) {m->now+=10;uint64_t t;assert(now(m,&t));m->now-=5;}
    return READY;
}
static int enter(void *v,uint64_t d)
{
    struct model *m=v;++m->calls;assert(!m->lock && m->now<d);
    if(m->lock_busy)return 0;
    m->lock=1;return 1;
}
static int leave(void *v)
{
    struct model *m=v;++m->calls;assert(m->lock);m->lock=0;
    if(m->late_unlock && m->p.stage==5)m->now=m->config.deadline_us;
    return m->fail_unlock ? 0 : 1;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;++m->calls;++m->reads;assert(m->lock);
    *out=*reg(m,a);
    if(a==PCK+8 && (*reg(m,PCK)&15)==8 && !m->no_ack)*out=*reg(m,a)=8;
    if(m->late_final && a==CCU+0xb00 && m->p.settle_until && m->now>=m->p.settle_until)
        m->now=m->config.deadline_us;
    return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;++m->calls;assert(m->lock && m->nwrite<32);
    assert(a!=CCU+0x2a0 && a!=CCU+0x5c0 && a!=CCU+0x5e0 && a!=0x070101ac);
    *reg(m,a)=value;m->writes[m->nwrite++]=(struct reg){a,value};
    return m->fail_write!=m->nwrite;
}
static const struct a733_npu_platform_operations ops={acquire,enter,leave,now,read32,write32};
static void setup(struct model *m,unsigned parent_div)
{
    memset(m,0,sizeof(*m));m->now=100;
    m->config=(struct a733_npu_platform_config){.fabric={0x03000001,0x82000001,0x84000000},
        .rate_hz={300000000,400000000,400000000},.owner=7,.handoff=99,.deadline_us=10000};
    assert(a733_clock_leases_init(&m->clocks,now,m));
    *reg(m,0x07090160)=0xc000;
    *reg(m,CCU)=0xf8000000|(99u<<8)|(99u<<16);
    *reg(m,CCU+0xa0)=0xfe000000|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    *reg(m,CCU+0x2a0)=0xf8000000|(81u<<8)|((parent_div-1)<<20);
    *reg(m,CCU+0x1908)=0x0fff0fff;
    *reg(m,CCU+0x1988)=0x0fff0000;
    *reg(m,CCU+0x1920)=0x10001;
    *reg(m,CCU+0x19a0)=0x10000;
    *reg(m,CCU+0x500)=m->config.fabric[0];
    *reg(m,CCU+0x580)=m->config.fabric[1];
    *reg(m,CCU+0x588)=m->config.fabric[2];
    *reg(m,CCU+0x5c0)=0x123456ff;
    *reg(m,CCU+0x5e0)=0x41055800;
    *reg(m,0x070101ac)=0xaa000001;
    *reg(m,CCU+0xb00)=0x00200010;
    *reg(m,CCU+0xb04)=0xa0080040;
    *reg(m,PCK)=0x80000000;
    assert(a733_npu_platform_init(&m->p,&m->config,&ops,m));
}
static enum a733_npu_platform_result run(struct model *m)
{
    enum a733_npu_platform_result r=PENDING;
    for(unsigned i=0;i<150 && r!=READY && r!=RETAINED;++i) {
        r=a733_npu_platform_poll(&m->p);m->now+=100;
    }
    assert(r==READY || r==RETAINED);return r;
}
static void retained(struct model *m)
{
    assert(!a733_npu_platform_ready(&m->p));
    unsigned calls=m->calls;
    for(unsigned i=0;i<3;++i)assert(a733_npu_platform_poll(&m->p)==RETAINED);
    assert(m->calls==calls);
    if(m->p.resources.clock.id)assert(a733_clock_token_status(&m->clocks,&m->p.resources.clock)==A733_CLOCK_OK);
}
static void success(void)
{
    for(unsigned d=1;d<=4;d*=2) {
        struct model m;setup(&m,d);
        assert(run(&m)==READY && a733_npu_platform_ready(&m.p));
        assert(m.p.parent_hz==1968000000u/d && m.p.divisor==4/d);
        assert(m.nwrite==sizeof(source_addresses)/sizeof(*source_addresses));
        for(unsigned i=0;i<m.nwrite;++i)assert(m.writes[i].address==source_addresses[i]);
        uint32_t reset=0xa0080040;
        for(unsigned i=0;i<3;++i){reset|=source_reset_bits[i];assert(m.writes[i].value==reset);}
        assert(m.writes[3].value==(0x00200000|(4/d-1)));
        assert(m.writes[4].value==(reset|1));
        assert(m.writes[5].value==(0x80200000|(4/d-1)));
        for(unsigned i=0;i<5;++i)assert(m.writes[i+6].value==source_pck_values[i]);
        assert(m.writes[11].value==0x80000008);
        assert(m.p.last_now>=m.p.settle_until && m.now<10000);
        assert(*reg(&m,CCU+0x5c0)==0x123456ff && *reg(&m,CCU+0x5e0)==0x41055800);
        assert(*reg(&m,0x070101ac)==0xaa000001);
        assert(a733_clock_token_status(&m.clocks,&m.p.resources.clock)==A733_CLOCK_OK);
        unsigned calls=m.calls;assert(a733_npu_platform_poll(&m.p)==READY && calls==m.calls);
    }
}
static void scopes(void)
{
    struct model m;setup(&m,2);struct a733_clock_token t={0},out={0};
    a733_clock_mask_t shared;assert(a733_npu_platform_scopes(&m.config,&shared));
    assert(shared==(A733_CLOCK_PLL_NPU|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_PERI0));
    assert(a733_clock_acquire(&m.clocks,8,A733_CLOCK_NPU,0,10000,&t)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&m.clocks,8,A733_CLOCK_PLL_NPU,0,10000,&t)==A733_CLOCK_OK);
    assert(t.shared==(A733_CLOCK_PLL_NPU|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&m.clocks,9,0,A733_CLOCK_REF,10000,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&m.clocks,9,0,A733_CLOCK_PLL_NPU,10000,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&m.clocks,&t,10000)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m.clocks,8,0,A733_CLOCK_NPU,10000,&t)==A733_CLOCK_OK);
    assert(a733_npu_platform_poll(&m.p)==BUSY && !m.reads && !m.nwrite);
    assert(a733_clock_release(&m.clocks,&t,10000)==A733_CLOCK_OK);
    assert(a733_npu_platform_poll(&m.p)==PENDING);
    assert(a733_clock_acquire(&m.clocks,9,0,A733_CLOCK_FABRIC,10000,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&m.clocks,9,0,A733_CLOCK_PERI0,10000,&out)==A733_CLOCK_BUSY);
    a733_clock_mask_t unchanged=123;m.config.fabric[2]=0x85000000;
    assert(!a733_npu_platform_scopes(&m.config,&unchanged) && unchanged==123);
}
static void refusal(void)
{
    const struct {uintptr_t address;uint32_t bit;} cases[]={
        {CCU+0xb00,0x80000000},{CCU+0xb04,0x10000},{PCK,1},{PCK+8,2},
        {CCU+0x2a0,0x10000000},{CCU+0x2a8,0x80000000},{CCU+0x1920,1},
        {CCU+0x588,1},{CCU+0x5e0,0x40000},{0x070101ac,1},{0x07090160,0x4000}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        struct model m;setup(&m,2);*reg(&m,cases[i].address)^=cases[i].bit;
        assert(run(&m)==RETAINED && !m.nwrite);retained(&m);
    }
    struct model m;setup(&m,2);*reg(&m,CCU+0x2a0)=0xf8000000|(83u<<8)|(1u<<20);
    assert(run(&m)==RETAINED && !m.nwrite);retained(&m); /* 1008 MHz cannot produce 492. */
    setup(&m,2);m.partial=3;assert(run(&m)==RETAINED && !m.reads);retained(&m);
    setup(&m,2);m.refuse=1;assert(run(&m)==RETAINED && !m.reads && !m.p.resources.clock.id);retained(&m);
}
static void ownership_and_time(void)
{
    struct model m;setup(&m,2);m.partial=1;
    assert(a733_npu_platform_poll(&m.p)==PENDING && m.p.resources.clock.id && !m.reads);
    uint64_t id=m.p.resources.clock.id;
    assert(run(&m)==READY && m.p.resources.clock.id==id && m.acquires==2);
    setup(&m,2);m.partial=2;assert(run(&m)==RETAINED && m.p.resources.clock.id && m.p.resources.pck && !m.p.resources.rail && !m.reads);retained(&m);
    setup(&m,2);m.partial=1;assert(a733_npu_platform_poll(&m.p)==PENDING);m.corrupt=1;
    assert(run(&m)==RETAINED && m.p.resources.clock.id+1==m.p.observed.clock.id && !m.reads);retained(&m);
    setup(&m,2);m.lock_busy=1;assert(a733_npu_platform_poll(&m.p)==PENDING);
    assert(a733_npu_platform_poll(&m.p)==BUSY && !m.reads);m.lock_busy=0;assert(run(&m)==READY);
    setup(&m,2);m.fail_write=4;assert(run(&m)==RETAINED && m.nwrite==4 && m.p.wrote);retained(&m);
    setup(&m,2);m.no_ack=1;assert(run(&m)==RETAINED && m.nwrite==12);retained(&m);
    setup(&m,2);m.late_final=1;assert(run(&m)==RETAINED && m.nwrite==12);retained(&m);
    setup(&m,2);m.late_unlock=1;assert(run(&m)==RETAINED && m.nwrite==12);retained(&m);
    setup(&m,2);m.fail_unlock=1;assert(run(&m)==RETAINED && !m.nwrite);retained(&m);
    setup(&m,2);m.regress=1;assert(run(&m)==RETAINED && !m.reads);retained(&m);
}
static void gate_profile(struct model *m,unsigned route)
{
    setup(m,4);
    static const uint32_t selected[]={0x81000000,0x84000000,0x83000000,0x82000000};
    static const uint32_t rates[]={600000000,400000000,480000000,800000000};
    static const uint32_t masks[]={0xa00,0x806,0x180,0x400};
    m->config.fabric[0]=0;m->config.rate_hz[0]=24000000;
    m->config.fabric[1]=0x80000000;m->config.rate_hz[1]=24000000;
    m->config.fabric[2]=0x80000000;m->config.rate_hz[2]=24000000;
    unsigned index=route==3 ? 1 : 2;
    m->config.fabric[index]=selected[route];m->config.rate_hz[index]=rates[route];
    *reg(m,CCU+0x500)=m->config.fabric[0];*reg(m,CCU+0x580)=m->config.fabric[1];
    *reg(m,CCU+0x588)=m->config.fabric[2];
    *reg(m,CCU+0x1908)=masks[route]|(masks[route]<<16);
    *reg(m,CCU+0x1988)=masks[route]<<16;
    memset(&m->p,0,sizeof(m->p));assert(a733_npu_platform_init(&m->p,&m->config,&ops,m));
}
static void gate_admission(void)
{
    static const uint32_t masks[]={0xa00,0x806,0x180,0x400};
    unsigned refusals=0;
    for(unsigned route=0;route<4;++route) {
        struct model m;gate_profile(&m,route);
        assert(a733_npu_platform_poll(&m.p)==PENDING);
        assert(a733_npu_platform_poll(&m.p)==PENDING && !m.nwrite && m.p.parent_hz==492000000);
        for(unsigned bit=0;bit<12;++bit)if(masks[route]&(1u<<bit)) {
            for(unsigned kind=0;kind<3;++kind) {
                gate_profile(&m,route);
                *reg(&m,CCU+(kind==2 ? 0x1988 : 0x1908))&=~(1u<<(bit+(kind ? 16 : 0)));
                assert(a733_npu_platform_poll(&m.p)==PENDING);
                assert(a733_npu_platform_poll(&m.p)==RETAINED && !m.nwrite);retained(&m);++refusals;
            }
        }
    }
    for(unsigned kind=0;kind<3;++kind) {
        struct model m;gate_profile(&m,0);
        *reg(&m,CCU+(kind==2 ? 0x19a0 : 0x1920))&=~(1u<<(kind ? 16 : 0));
        assert(a733_npu_platform_poll(&m.p)==PENDING);
        assert(a733_npu_platform_poll(&m.p)==RETAINED && !m.nwrite);retained(&m);++refusals;
    }
    assert(refusals==27);
    puts("NPU/PERI0 forced-on parent admission: four routes pass; missing No-Auto, software gate or effective gate retains before writes.");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"gates")){gate_admission();return 0;}
    if(argc==2 && !strcmp(argv[1],"admission")) {
        struct model m;setup(&m,2);m.partial=2;
        assert(run(&m)==RETAINED && m.p.resources.clock.id && m.p.resources.pck && !m.reads);retained(&m);
        setup(&m,2);m.refuse=1;
        assert(run(&m)==RETAINED && !m.reads && !m.p.resources.clock.id);retained(&m);
        const struct {uintptr_t address;uint32_t bit;} added[]={
            {CCU+0x2a0,2},{CCU+0xa0,2},{CCU+0xb04,0x80000},{CCU+0x1b1c,1}
        };
        for(unsigned i=0;i<sizeof(added)/sizeof(*added);++i) {
            setup(&m,2);*reg(&m,added[i].address)^=added[i].bit;
            assert(run(&m)==RETAINED && !m.nwrite);retained(&m);
        }
        puts("Primary-reference admission refusals and retained acquisition passed.");return 0;
    }
    assert(SOURCE_HZ==A733_NPU_INITIAL_HZ && SOURCE_UV==A733_NPU_INITIAL_UV);
    success();scopes();refusal();ownership_and_time();
    puts("NPU startup source order, preserved fields, leases, retained failures and deadlines passed.");
    return 0;
}
