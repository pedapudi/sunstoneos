#include "a733_ce.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
struct reg { uintptr_t a;uint32_t v; };
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct a733_ce controller;
    struct a733_ce_config config;
    struct a733_ce_memory memory;
    struct reg regs[40];unsigned count;
    uint64_t now;
    unsigned reads,writes,syncs,starts,enters,leaves,retained,held,published,acquired;
    unsigned inject_write,inject_read,sync_failure,authorize_pending,lock_busy;
    unsigned finish_late,rollback,health_late,retain_busy;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for (unsigned i=0;i<m->count;++i) if (m->regs[i].a==a) return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static int now(void *v,uint64_t *n)
{struct model *m=v;*n=m->rollback ? 0:m->now;return 1;}
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *mem)
{
    struct model *m=v;
    assert(c->handoff==7 && c->health_profile==8 && t->id==m->token.id && mem==&m->memory);
    return !m->authorize_pending;
}
static int enter(void *v,uint64_t deadline)
{
    struct model *m=v;assert(deadline==m->config.deadline_us);++m->enters;
    if (m->lock_busy) return 0;
    assert(!m->held);m->held=1;return 1;
}
static void leave(void *v)
{
    struct model *m=v;assert(m->held);m->held=0;++m->leaves;
    if (m->finish_late && m->acquired) {
        m->now=m->config.deadline_us;
        if (m->retain_busy) atomic_store(&m->registry.busy,1);
    }
}
static void retain(void *v)
{struct model *m=v;assert(m->held);++m->retained;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;assert(m->held);++m->reads;
    if (m->inject_read==m->reads) return -1;
    *out=*reg(m,a);return 1;
}
static uint64_t le(const uint8_t *p,unsigned n)
{uint64_t v=0;for (unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);return v;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held);++m->writes;
    assert(a>=NS && a<SEC); /* No CCU, secure health or immutable-key writes. */
    if (a==NS+12) *reg(m,a)&=~value;
    else if (a==NS+0x10) {
        assert(value==0x400 && m->published && !m->starts);
        assert(*reg(m,NS)==(uint32_t)m->config.descriptor_physical);
        assert((*reg(m,NS+4)&255)==m->config.descriptor_physical>>32);
        assert((*reg(m,NS+8)&15)==4);
        /* Decode as the hardware descriptor, independently of runtime writes. */
        const uint8_t *d=m->memory.descriptor;
        assert(le(d,4)==0x11002 && le(d+4,4)==0x203);
        assert(le(d+8,5)==0 && le(d+13,5)==0 && le(d+18,5)==0);
        assert(le(d+24,5)==0 && le(d+29,5)==m->config.output_physical);
        assert(le(d+36,4)==0 && le(d+40,4)==32);
        for (unsigned i=44;i<256;++i) assert(d[i]==0);
        ++m->starts;
    } else *reg(m,a)=value;
    /* An uncertain write may already have changed hardware. */
    return m->inject_write==m->writes ? -1:1;
}
static int sync_memory(void *v,struct a733_ce_memory *mem,enum a733_ce_sync op)
{
    struct model *m=v;assert(m->held && mem==&m->memory);++m->syncs;
    if (op==A733_CE_PUBLISH) {assert(!m->starts);m->published=1;}
    else {
        assert(m->starts==1 && (*reg(m,NS+12)&0x30)==0x10);
        m->acquired=1;
        /* Deterministic bytes are a DMA model fixture, never entropy. */
        for (unsigned i=0;i<32;++i) mem->output[i]=(uint8_t)(17+i);
    }
    return m->sync_failure==m->syncs ? -1:1;
}
static const struct a733_ce_ops ops={now,authorize,enter,leave,retain,read_reg,write_reg,sync_memory};
static void setup(struct model *m,unsigned high)
{
    memset(m,0,sizeof(*m));m->now=1;
    assert(a733_clock_leases_init(&m->registry,now,m));
    a733_clock_mask_t shared=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_RC16M;
    assert(a733_clock_acquire(&m->registry,5,shared,A733_CLOCK_CE,10000,&m->token)==A733_CLOCK_OK);
    m->config=(struct a733_ce_config){.owner=5,.handoff=7,.health_profile=8,.deadline_us=10000,
        .descriptor_physical=high?UINT64_C(0xff00000100):0x40000100,
        .output_physical=high?UINT64_C(0xfe00000040):0x40000240,
        .fabric={0x03000001,0x83000000,0x84000000},.fabric_hz={300000000,600000000,400000000},
        .health={9,325,0,0x30200}};
    /* Health values are artificial contract fixtures, not physical thresholds. */
    *reg(m,0x07090160)=1;
    *reg(m,CCU)=0xf8000000|79u<<8|79u<<16;
    *reg(m,CCU+0xa0)=0xfe000000|99u<<8|1u<<20;
    *reg(m,CCU+0xa8)=0;*reg(m,CCU+0xac)=0;
    *reg(m,CCU+0x1908)=0x0a060a06;*reg(m,CCU+0x1988)=0x0a060000;
    *reg(m,CCU+0xac0)=0x81000000;*reg(m,CCU+0xac4)=0x30003;
    *reg(m,CCU+0x5e4)=4;
    for (unsigned i=0;i<3;++i) *reg(m,CCU+(unsigned[]){0x500,0x580,0x588}[i])=m->config.fabric[i];
    for (unsigned i=0;i<4;++i) *reg(m,SEC+0x90+4*i)=m->config.health[i];
    *reg(m,NS+4)=0xa5000099; /* Stale upper address, preserve reserved bits. */
    *reg(m,NS+8)=0x10000000; /* Preserve reserved bits during IRQ changes. */
    assert(a733_ce_init(&m->controller,&m->config,&ops,m,&m->registry,&m->token,&m->memory));
}
static void unchanged(const uint8_t *out)
{for (unsigned i=0;i<32;++i) assert(out[i]==0xa5);}
static void submit(struct model *m,uint8_t *out)
{
    for (unsigned i=0;i<3;++i) {assert(a733_ce_poll(&m->controller,out)==A733_CE_PENDING);unchanged(out);}
    assert(m->starts==1 && !m->held);
}
static void terminal(struct model *m,uint8_t *out)
{
    unsigned reads=m->reads,writes=m->writes,syncs=m->syncs,enters=m->enters;
    assert(a733_ce_poll(&m->controller,out)==A733_CE_RETAINED);
    assert(reads==m->reads && writes==m->writes && syncs==m->syncs && enters==m->enters);
    unchanged(out);
}
int main(void)
{
    struct model m;uint8_t out[32];memset(out,0xa5,sizeof(out));
    for (unsigned high=0;high<2;++high) {
        setup(&m,high);submit(&m,out);
        assert(a733_ce_poll(&m.controller,out)==A733_CE_PENDING);unchanged(out);
        *reg(&m,NS+12)=0x10;
        assert(a733_ce_poll(&m.controller,out)==A733_CE_RANDOM);
        for (unsigned i=0;i<32;++i) assert(out[i]==17+i);
        assert((*reg(&m,NS+8))==0x10000000 && (*reg(&m,NS+4)&0xffffff00)==0xa5000000);
        unsigned reads=m.reads,writes=m.writes;
        assert(a733_ce_poll(&m.controller,out)==A733_CE_CONSUMED);
        assert(reads==m.reads && writes==m.writes);
        memset(out,0xa5,sizeof(out));
    }
    /* Hardware failure status and errors in either security bank. */
    for (unsigned k=0;k<7;++k) {
        setup(&m,0);submit(&m,out);*reg(&m,NS+12)=0x10;
        if (k==0) *reg(&m,NS+12)=0x20;
        else if (k==1) *reg(&m,NS+12)=0x30;
        else if (k==2) *reg(&m,SEC+12)=1;
        else if (k==3) *reg(&m,NS+0x18)=0x43u<<16;
        else if (k==4) *reg(&m,SEC+0x18)=0x4cu<<16;
        else if (k==5) *reg(&m,SEC+0x18)=0x4fu<<16;
        else *reg(&m,NS+0x18)=0xff;
        assert(a733_ce_poll(&m.controller,out)==A733_CE_RETAINED);assert(!m.acquired);terminal(&m,out);
    }
    /* Refuse inherited use, disabled health and unstable source controls. */
    const uintptr_t invalid_regs[]={NS+8,SEC+8,NS+12,SEC+12,NS+0x18,SEC+0x18,
        NS+0x10,SEC+0x10,NS+0xd0,SEC+0x9c,CCU+0xac0,CCU+0xac4,CCU+0x5e4,
        CCU+0x1908,CCU+0x1988,0x07090160};
    for (unsigned i=0;i<sizeof(invalid_regs)/sizeof(invalid_regs[0]);++i) {
        setup(&m,0);uintptr_t a=invalid_regs[i];
        *reg(&m,a)=a==NS+0xd0?1:(a>=NS && a<SEC+0x90?1:0);
        assert(a733_ce_poll(&m.controller,out)==A733_CE_PENDING);
        assert(a733_ce_poll(&m.controller,out)==A733_CE_RETAINED);
        assert(!m.starts && !m.published);terminal(&m,out);
    }
    /* Deadline, time reversal and a health-profile change during DMA. */
    for (unsigned i=0;i<3;++i) {
        setup(&m,0);submit(&m,out);
        if (i==0) m.now=m.config.deadline_us;
        if (i==1) m.rollback=1;
        if (i==2) {*reg(&m,NS+12)=0x10;*reg(&m,SEC+0x90)=10;}
        assert(a733_ce_poll(&m.controller,out)==A733_CE_RETAINED);terminal(&m,out);
    }
    /* Every writable command boundary can fail after taking effect. */
    for (unsigned i=1;i<=7;++i) {
        setup(&m,0);m.inject_write=i;
        enum a733_ce_result result=A733_CE_PENDING;
        for (unsigned poll=0;poll<6 && result==A733_CE_PENDING;++poll) {
            if (m.starts) *reg(&m,NS+12)=0x10;
            result=a733_ce_poll(&m.controller,out);
        }
        assert(result==A733_CE_RETAINED);terminal(&m,out);
    }
    for (unsigned i=1;i<=2;++i) {
        setup(&m,0);m.sync_failure=i;
        enum a733_ce_result result=A733_CE_PENDING;
        for (unsigned poll=0;poll<5 && result==A733_CE_PENDING;++poll) {
            if (m.starts) *reg(&m,NS+12)=0x10;
            result=a733_ce_poll(&m.controller,out);
        }
        assert(result==A733_CE_RETAINED);terminal(&m,out);
    }
    setup(&m,0);m.inject_read=1;assert(a733_ce_poll(&m.controller,out)==A733_CE_PENDING);
    assert(a733_ce_poll(&m.controller,out)==A733_CE_RETAINED);terminal(&m,out);
    setup(&m,0);m.authorize_pending=1;assert(a733_ce_poll(&m.controller,out)==A733_CE_PENDING);assert(!m.reads);
    m.authorize_pending=0;assert(a733_ce_poll(&m.controller,out)==A733_CE_PENDING);
    m.lock_busy=1;assert(a733_ce_poll(&m.controller,out)==A733_CE_BUSY);assert(!m.reads);
    /* Final expiry with registry contention must never publish completion. */
    setup(&m,0);submit(&m,out);*reg(&m,NS+12)=0x10;m.finish_late=1;m.retain_busy=1;
    assert(a733_ce_poll(&m.controller,out)==A733_CE_BUSY);unchanged(out);
    unsigned reads=m.reads,writes=m.writes,syncs=m.syncs;
    assert(a733_ce_poll(&m.controller,out)==A733_CE_BUSY);
    assert(reads==m.reads && writes==m.writes && syncs==m.syncs);
    atomic_store(&m.registry.busy,0);assert(a733_ce_poll(&m.controller,out)==A733_CE_RETAINED);terminal(&m,out);
    /* Full 40-bit spans, health and output alias admission. */
    setup(&m,0);struct a733_ce other={0};struct a733_ce_config bad=m.config;
    bad.output_physical=(UINT64_C(1)<<40);assert(!a733_ce_init(&other,&bad,&ops,&m,&m.registry,&m.token,&m.memory));
    bad=m.config;bad.output_physical=bad.descriptor_physical+64;
    assert(!a733_ce_init(&other,&bad,&ops,&m,&m.registry,&m.token,&m.memory));
    bad=m.config;bad.health[3]=512;assert(!a733_ce_init(&other,&bad,&ops,&m,&m.registry,&m.token,&m.memory));
    assert(a733_ce_poll(&m.controller,m.memory.output)==A733_CE_INVALID);
    puts("PASS CE descriptor, both address words, completion/error/health refusal, cache handoff and retained fault model; no entropy qualification");
}
