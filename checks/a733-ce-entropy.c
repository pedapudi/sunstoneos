#include "a733_ce_entropy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
struct reg {uintptr_t a;uint32_t v;};
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct device_memory_pool pool;
    struct device_memory_lease lease;
    struct a733_ce_entropy service;
    struct a733_ce_config config;
    _Alignas(4096) uint8_t pages[8192];
    struct reg regs[40];unsigned count;
    uint64_t now;
    unsigned scrubs,held,reads,writes,starts,pending,published,acquired,health_writes,authorizations;
    unsigned fail_health_write,finish_late,retain_busy,retained;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for (unsigned i=0;i<m->count;++i) if (m->regs[i].a==a) return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static int now(void *v,uint64_t *n) {struct model *m=v;*n=m->now;return 1;}
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *mem)
{
    struct model *m=v;assert(c->owner==5 && c->handoff==7 && c->health_profile==8);
    assert(t->id==m->token.id && mem==(void *)m->pages);
    assert(c->descriptor_physical==0x41000000 && c->output_physical==0x41000100);
    ++m->authorizations;return 1;
}
static int enter(void *v,uint64_t d)
{struct model *m=v;assert(d==m->service.deadline && !m->held);m->held=1;return 1;}
static void leave(void *v)
{
    struct model *m=v;assert(m->held);m->held=0;
    if (m->finish_late && m->acquired) {
        m->now=m->service.deadline;
        if (m->retain_busy) atomic_store(&m->registry.busy,1);
    }
}
static void retain(void *v) {struct model *m=v;assert(m->held);++m->retained;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct model *m=v;assert(m->held);++m->reads;*out=*reg(m,a);return 1;}
static uint64_t le(const uint8_t *p,unsigned n)
{uint64_t v=0;for (unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);return v;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held);++m->writes;
    if (a>=SEC+0x90 && a<=SEC+0x9c) {
        assert(!(*reg(m,SEC+0x9c)&0x30000) && (*reg(m,NS+0xd0)&3)==2);
        const uintptr_t order[]={SEC+0x90,SEC+0x94,SEC+0x98,SEC+0x9c,SEC+0x9c};
        assert(m->health_writes<5 && a==order[m->health_writes]);
        if (m->health_writes==3) assert(value==512);
        if (m->health_writes==4) assert(value==0x30200);
        ++m->health_writes;*reg(m,a)=value;
        return m->fail_health_write==m->health_writes ? -1:1;
    }
    assert(a>=NS && a<SEC);
    if (a==NS+12) {*reg(m,a)&=~value;m->pending=0;}
    else if (a==NS+0x10) {
        assert(value==0x400 && m->published==m->starts+1 && !m->pending);
        assert(*reg(m,NS)==0x41000000 && (*reg(m,NS+4)&255)==0);
        assert((*reg(m,NS+8)&15)==4);
        const uint8_t *d=m->pages;
        assert(le(d,4)==0x11002 && le(d+4,4)==0x203);
        assert(le(d+24,5)==0 && le(d+29,5)==0x41000100);
        assert(le(d+36,4)==0 && le(d+40,4)==32);
        for (unsigned i=44;i<256;++i) assert(!d[i]);
        ++m->starts;m->pending=1;
    } else *reg(m,a)=value;
    return 1;
}
static int sync_memory(void *v,struct a733_ce_memory *mem,enum a733_ce_sync op)
{
    struct model *m=v;struct device_memory_extent e;enum device_memory_state state;
    assert(m->held && mem==(void *)m->pages);
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_DEVICE);
    if (op==A733_CE_PUBLISH) {assert(!m->pending);++m->published;}
    else if (op==A733_CE_SCRUB) {
        /* The copied task result and hash state leave the retained page. */
        assert(m->acquired && !(*reg(m,NS+12)&0x30) && !(*reg(m,NS+8)&4));
        for (size_t i=offsetof(struct a733_ce_memory,output);i<offsetof(struct a733_ce_memory,cipher_output);++i) assert(!((uint8_t *)mem)[i]);
        ++m->scrubs;
    } else {assert(m->pending && (*reg(m,NS+12)&0x30)==0x10);++m->acquired;}
    return 1;
}
static const struct a733_ce_ops ops={now,authorize,enter,leave,retain,read_reg,write_reg,sync_memory};
static void setup(struct model *m,unsigned cold)
{
    memset(m,0,sizeof(*m));m->now=1;
    assert(a733_clock_leases_init(&m->registry,now,m));
    a733_clock_mask_t shared=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_RC16M;
    assert(a733_clock_acquire(&m->registry,5,shared,A733_CLOCK_CE,10000,&m->token)==A733_CLOCK_OK);
    assert(device_memory_pool_init(&m->pool,(uintptr_t)m->pages,0x41000000,sizeof(m->pages)));
    struct device_memory_extent e;
    assert(device_memory_acquire(&m->pool,5,4096,&m->lease,&e));
    m->config=(struct a733_ce_config){.owner=5,.handoff=7,.health_profile=8,.deadline_us=10000,
        .fabric={0x03000001,0x83000000,0x84000000},.fabric_hz={300000000,600000000,400000000},
        .health={9,325,0,0x30200},.configure_health=cold};
    /* Artificial model values: no production threshold or entropy assertion. */
    *reg(m,0x07090160)=0x10000003;*reg(m,CCU)=0xf8000000|79u<<8|79u<<16;
    *reg(m,CCU+0xa0)=0xfe000000|99u<<8|1u<<20;
    *reg(m,CCU+0x1908)=0x0a060a06;*reg(m,CCU+0x1988)=0x0a060000;
    *reg(m,CCU+0xac0)=0x81000000;*reg(m,CCU+0xac4)=0x30003;*reg(m,CCU+0x5e4)=4;
    for (unsigned i=0;i<3;++i) *reg(m,CCU+(unsigned[]){0x500,0x580,0x588}[i])=m->config.fabric[i];
    for (unsigned i=0;i<4;++i) *reg(m,SEC+0x90+4*i)=cold?0:m->config.health[i];
    *reg(m,NS+4)=0xa5000099;
    assert(a733_ce_entropy_bind(&m->service,&m->pool,m->lease,&m->config,&ops,m,&m->registry,&m->token));
    assert(!device_memory_release(&m->pool,m->lease));
}
static void complete(struct model *m)
{
    assert(m->pending);*reg(m,NS+12)=0x10;
    /* Deterministic DMA data are a register-model fixture, never random output. */
    for (unsigned i=0;i<32;++i) m->pages[256+i]=(uint8_t)(m->starts*32+i);
}
static void unchanged(const uint8_t *out,size_t n)
{for (size_t i=0;i<n;++i) assert(out[i]==0xa5);}
static void empty_staging(const struct model *m)
{for (size_t i=0;i<sizeof(m->service.staging);++i) assert(!m->service.staging[i]);}
static enum a733_ce_entropy_result pump(struct model *m,uint64_t g,uint8_t *out,size_t n,unsigned auto_complete)
{
    enum a733_ce_entropy_result r=A733_CE_ENTROPY_PENDING;
    for (unsigned i=0;i<80 && r==A733_CE_ENTROPY_PENDING;++i) {
        if (m->pending && auto_complete) complete(m);
        r=a733_ce_entropy_poll(&m->service,g,out,n);
        if (r!=A733_CE_ENTROPY_BYTES) unchanged(out,n);
    }
    return r;
}
static void until_start(struct model *m,uint64_t g,uint8_t *out,size_t n,unsigned target)
{
    for (unsigned i=0;i<80 && m->starts<target;++i) {
        if (m->pending) complete(m);
        assert(a733_ce_entropy_poll(&m->service,g,out,n)==A733_CE_ENTROPY_PENDING);unchanged(out,n);
    }
    assert(m->starts==target && m->pending);
}
static uint64_t request(struct model *m,size_t n,uint64_t deadline)
{
    uint64_t g=0;assert(a733_ce_entropy_request(&m->service,n,deadline,&g)==A733_CE_ENTROPY_PENDING);assert(g);return g;
}
static void terminal(struct model *m,uint64_t g,uint8_t *out,size_t n)
{
    unsigned reads=m->reads,writes=m->writes;struct device_memory_extent e;enum device_memory_state state;
    assert(a733_ce_entropy_poll(&m->service,g,out,n)==A733_CE_ENTROPY_RETAINED);
    assert(reads==m->reads && writes==m->writes);unchanged(out,n);empty_staging(m);
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_RETAINED);
    assert(!device_memory_release(&m->pool,m->lease));
}
int main(void)
{
    struct model m;uint8_t out[256];uint64_t g,old;memset(out,0xa5,sizeof(out));
    setup(&m,1);g=request(&m,65,10000);
    assert(pump(&m,g,out,65,1)==A733_CE_ENTROPY_BYTES);
    for (unsigned i=0;i<65;++i) assert(out[i]==(uint8_t)(32+i));
    assert(m.starts==3 && m.scrubs==3 && m.health_writes==5 && m.authorizations==1);empty_staging(&m);
    memset(out,0xa5,sizeof(out));assert(a733_ce_entropy_poll(&m.service,g,out,65)==A733_CE_ENTROPY_CONSUMED);unchanged(out,65);
    old=g;g=request(&m,33,20000);assert(g!=old);
    assert(a733_ce_entropy_poll(&m.service,old,out,33)==A733_CE_ENTROPY_INVALID);
    assert(pump(&m,g,out,33,1)==A733_CE_ENTROPY_BYTES);
    for (unsigned i=0;i<33;++i) assert(out[i]==(uint8_t)(128+i));
    assert(m.starts==5 && m.health_writes==5 && m.authorizations==1);empty_staging(&m);
    memset(out,0xa5,sizeof(out));
    for (unsigned i=1;i<=5;++i) {
        setup(&m,1);m.fail_health_write=i;g=request(&m,32,10000);
        assert(pump(&m,g,out,32,1)==A733_CE_ENTROPY_RETAINED);
        assert(!m.starts && m.health_writes==i);terminal(&m,g,out,32);
    }
    setup(&m,1);*reg(&m,SEC+0x9c)=0x30200;g=request(&m,32,10000);
    assert(pump(&m,g,out,32,1)==A733_CE_ENTROPY_RETAINED);assert(!m.writes);terminal(&m,g,out,32);
    setup(&m,0);g=request(&m,32,10000);assert(pump(&m,g,out,32,1)==A733_CE_ENTROPY_BYTES);assert(!m.health_writes);
    memset(out,0xa5,sizeof(out));
    /* Failed second tasks must erase accumulated private bytes, leaving caller output intact. */
    for (unsigned i=0;i<3;++i) {
        setup(&m,0);g=request(&m,65,10000);until_start(&m,g,out,65,2);assert(m.service.filled==32);complete(&m);
        if (!i) *reg(&m,SEC+0x90)=10;
        else *reg(&m,(i==1?NS:SEC)+0x18)=0x43u<<16;
        assert(a733_ce_entropy_poll(&m.service,g,out,65)==A733_CE_ENTROPY_RETAINED);terminal(&m,g,out,65);
    }
    /* Rearm refuses stale completion, errors, health changes and uncleared launch state. */
    const uintptr_t stale[]={NS+12,SEC+12,NS+0x18,SEC+0x18,SEC+0x90,NS+0x10,SEC+0x10,NS+8,SEC+8};
    for (unsigned i=0;i<sizeof(stale)/sizeof(stale[0]);++i) {
        setup(&m,0);g=request(&m,64,10000);until_start(&m,g,out,64,1);complete(&m);
        assert(a733_ce_entropy_poll(&m.service,g,out,64)==A733_CE_ENTROPY_PENDING);
        assert(m.service.filled==32 && m.service.need_request);*reg(&m,stale[i])=1;
        assert(a733_ce_entropy_poll(&m.service,g,out,64)==A733_CE_ENTROPY_RETAINED);assert(m.starts==1);terminal(&m,g,out,64);
    }
    /* Timeout and late publication with registry contention perform metadata-only retries. */
    for (unsigned late=0;late<2;++late) {
        setup(&m,0);g=request(&m,32,10000);until_start(&m,g,out,32,1);
        if (late) {complete(&m);m.finish_late=1;m.retain_busy=1;}
        else {m.now=10000;atomic_store(&m.registry.busy,1);}
        assert(a733_ce_entropy_poll(&m.service,g,out,32)==A733_CE_ENTROPY_BUSY);empty_staging(&m);unchanged(out,32);
        unsigned reads=m.reads,writes=m.writes;
        assert(a733_ce_entropy_poll(&m.service,g,out,32)==A733_CE_ENTROPY_BUSY);
        assert(reads==m.reads && writes==m.writes);
        atomic_store(&m.registry.busy,0);terminal(&m,g,out,32);
    }
    /* Refuse aliases and invalid sizes without modifying a live request. */
    setup(&m,0);g=0;
    assert(a733_ce_entropy_request(&m.service,0,10000,&g)==A733_CE_ENTROPY_INVALID);
    assert(a733_ce_entropy_request(&m.service,257,10000,&g)==A733_CE_ENTROPY_INVALID);
    assert(a733_ce_entropy_request(&m.service,32,10000,(uint64_t *)m.pages)==A733_CE_ENTROPY_INVALID);
    g=request(&m,32,10000);
    assert(a733_ce_entropy_poll(&m.service,g,m.pages,32)==A733_CE_ENTROPY_INVALID);
    assert(a733_ce_entropy_poll(&m.service,g,(uint8_t *)&m.service,32)==A733_CE_ENTROPY_INVALID);
    assert(a733_ce_entropy_poll(&m.service,g,(uint8_t *)&m.registry,32)==A733_CE_ENTROPY_INVALID);
    assert(!m.starts);
    setup(&m,0);m.service.next_generation=UINT64_MAX;g=request(&m,1,10000);assert(g==UINT64_MAX);
    assert(pump(&m,g,out,1,1)==A733_CE_ENTROPY_BYTES);
    assert(a733_ce_entropy_request(&m.service,1,20000,&g)==A733_CE_ENTROPY_INVALID);memset(out,0xa5,sizeof(out));
    /* Copied service, callback substitution and stale pool identity cannot operate hardware. */
    setup(&m,0);struct a733_ce_entropy copy=m.service;
    assert(a733_ce_entropy_request(&copy,32,10000,&g)==A733_CE_ENTROPY_RETAINED);assert(!m.writes);
    setup(&m,0);m.service.engine->controller.ops.read=0;
    assert(a733_ce_entropy_request(&m.service,32,10000,&g)==A733_CE_ENTROPY_RETAINED);assert(!m.writes);
    setup(&m,0);++m.pool.allocations[m.lease.slot].generation;
    assert(a733_ce_entropy_request(&m.service,32,10000,&g)==A733_CE_ENTROPY_RETAINED);assert(!m.writes);
    puts("PASS assessed-profile provisioning, actual pool-backed repeated CE requests, staged publication, binding refusal and retained faults; no entropy qualification");
}
