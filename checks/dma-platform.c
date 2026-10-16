#include "a733_dma_platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU 0x02002000u
#define DMA 0x04601000u
#define OSC 0x07090160u
#define READY A733_DMA_PLATFORM_READY
#define BUSY A733_DMA_PLATFORM_BUSY
#define PENDING A733_DMA_PLATFORM_PENDING
#define RETAINED A733_DMA_PLATFORM_RETAINED
struct model {
    struct a733_clock_leases leases;
    uint32_t ccu[0x2000/4], dma[0x500/4], osc;
    uint64_t now, deadline;
    unsigned locked, reads, writes, acquire_calls, leaves, parent_reads;
    unsigned busy_once, pending_once, bad_handoff, late_acquire, regress_acquire;
    uintptr_t fail_write, late_write, late_read, fail_read;
    unsigned late_leave;
    uintptr_t addresses[16];uint32_t values[16];
};
static int now(void *cookie,uint64_t *v) {*v=((struct model *)cookie)->now;return 1;}
static enum a733_dma_platform_result acquire(void *cookie,
    const struct a733_dma_platform_config *c,uint32_t shared,struct a733_clock_token *token)
{
    struct model *m=cookie;assert(!m->locked);++m->acquire_calls;
    if (m->bad_handoff || c->handoff!=0x1234) return RETAINED;
    if (m->busy_once) {m->busy_once=0;return BUSY;}
    if (!token->id) {
        assert(a733_clock_acquire(&m->leases,c->owner,shared,A733_CLOCK_DMA0,c->deadline_us,token)==A733_CLOCK_OK);
    }
    if (m->late_acquire) m->now=m->deadline;
    if (m->regress_acquire) --m->now;
    if (m->pending_once) {m->pending_once=0;return PENDING;}
    return READY;
}
static int enter(void *cookie,uint64_t deadline)
{struct model *m=cookie;assert(!m->locked && m->now<deadline);m->locked=1;return 1;}
static void leave(void *cookie)
{struct model *m=cookie;assert(m->locked);m->locked=0;++m->leaves;if(m->late_leave)m->now=m->deadline;}
static uint32_t *reg(struct model *m,uintptr_t a)
{
    if (a==OSC) return &m->osc;
    if (a>=CCU && a<CCU+0x2000 && !(a%4)) return &m->ccu[(a-CCU)/4];
    assert(a>=DMA && a<DMA+0x500 && !(a%4));
    return &m->dma[(a-DMA)/4];
}
static int read_reg(void *cookie,uintptr_t a,uint32_t *v)
{
    struct model *m=cookie;assert(m->locked);++m->reads;
    if (a>=CCU && a<=CCU+0xac) ++m->parent_reads;
    if (a>=DMA && a<DMA+0x500) assert((m->ccu[0x704/4]&0x10001)==0x10001 && (m->ccu[0x5e4/4]&1));
    *v=*reg(m,a);if(a==m->late_read)m->now=m->deadline;return a!=m->fail_read;
}
static int write_reg(void *cookie,uintptr_t a,uint32_t v)
{
    struct model *m=cookie;assert(m->locked && m->writes<16);
    if (a==CCU+0x704) assert(!(m->ccu[0x704/4]&~v&0x10001));
    else if (a==CCU+0x5e4) assert((v&0x40302)==0x40302);
    else assert(a==DMA+0x28 || a==DMA+0x34 || a==DMA+0x38);
    m->addresses[m->writes]=a;m->values[m->writes++]=v;
    *reg(m,a)=v;if(a==m->late_write)m->now=m->deadline;
    return a!=m->fail_write;
}
static const struct a733_dma_platform_operations ops={acquire,enter,leave,now,read_reg,write_reg};
static struct a733_dma_platform_config config(unsigned cold)
{
    return (struct a733_dma_platform_config){
        .fabric={0x03000002,0x82000001,0x84000001},.rate_hz={200000000,400000000,200000000},
        .expected_cpu=cold?0:0xff,.expected_mcu=0xff00,.desired_cpu=0xff,.desired_mcu=0xff00,
        .route_change_mask=0xff,.owner=7,.handoff=0x1234,.deadline_us=10000,.cold=cold};
}
static void setup(struct model *m,struct a733_dma_platform *p,struct a733_dma_platform_config c)
{
    memset(m,0,sizeof(*m));memset(p,0,sizeof(*p));m->now=100;m->deadline=c.deadline_us;
    assert(a733_clock_leases_init(&m->leases,now,m));
    for(unsigned i=0;i<3;++i) m->ccu[(unsigned[]){0x500/4,0x580/4,0x588/4}[i]]=c.fabric[i]|0x00400000;
    m->ccu[0]=0xf8000000|(99<<16)|(99<<8);
    m->ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    m->ccu[0x1908/4]=0x8fff0fff; m->ccu[0x1988/4]=0x0fff0000;
    m->ccu[0x704/4]=0x200002|(c.cold?0:0x10001);
    m->ccu[0x5e4/4]=0x40302|0x1000|(c.cold?0:1);
    m->dma[0x28/4]=c.cold?0:4;
    m->dma[0x34/4]=0xabcd0000|c.expected_cpu;
    m->dma[0x38/4]=0x98760000|c.expected_mcu;
    assert(a733_dma_platform_init(p,&c,&ops,m));
}
static enum a733_dma_platform_result finish(struct model *m,struct a733_dma_platform *p)
{
    for(unsigned i=0;i<12;++i) {
        enum a733_dma_platform_result r=a733_dma_platform_poll(p);
        assert(!m->locked);if(r==READY || r==RETAINED)return r;m->now+=20;
    }
    assert(0);return RETAINED;
}
static void retained(struct model *m,struct a733_dma_platform *p)
{
    assert(p->failed && !a733_dma_platform_ready(p));
    unsigned reads=m->reads,writes=m->writes,acquires=m->acquire_calls;
    assert(a733_dma_platform_poll(p)==RETAINED);
    assert(m->reads==reads && m->writes==writes && m->acquire_calls==acquires);
    if(p->token.id) {
        struct a733_clock_token out={0};
        /* The failed owner keeps its token; no automatic registry release. */
        assert(a733_clock_acquire(&m->leases,8,0,A733_CLOCK_DMA0,UINT64_MAX,&out)==A733_CLOCK_BUSY);
    }
}
int main(int argc, char **argv)
{
    struct model m;struct a733_dma_platform p;struct a733_dma_platform_config c=config(1);
    if(argc==2 && !strcmp(argv[1],"--parent-gates-only")) {
        const unsigned mhz[]={600,400,800,480};
        const uint32_t masks[]={0xa00,0x806,0x400,0x180};
        for(unsigned branch=0;branch<4;++branch) {
            c=config(0);c.fabric[0]=0;c.fabric[1]=c.fabric[2]=0x80000000;
            c.rate_hz[0]=c.rate_hz[1]=c.rate_hz[2]=24000000;
            unsigned slot=branch==1?2:1;
            unsigned selector=branch==0?3:branch==1?4:branch==2?2:4;
            c.fabric[slot]=0x80000000u|(selector<<24);c.rate_hz[slot]=mhz[branch]*1000000u;
            uint32_t low=masks[branch],high=low<<16;
            setup(&m,&p,c);m.ccu[0x1908/4]=low|high;m.ccu[0x1988/4]=high;
            assert(finish(&m,&p)==READY && !m.writes);
            for(unsigned bit=0;bit<12;++bit) if(low&(1u<<bit)) {
                for(unsigned kind=0;kind<3;++kind) {
                    setup(&m,&p,c);m.ccu[0x1908/4]=low|high;m.ccu[0x1988/4]=high;
                    unsigned offset=kind==2?0x1988:0x1908;
                    m.ccu[offset/4]&=~(1u<<(bit+(kind?16:0)));
                    assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
                }
            }
        }
        for(unsigned i=0;i<2;++i) for(unsigned late=0;late<2;++late) {
            setup(&m,&p,config(0));uintptr_t address=CCU+(i?0x1988:0x1908);
            if(late)m.late_read=address;else m.fail_read=address;
            assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
        }
        c=config(0);c.fabric[0]=0;c.fabric[1]=c.fabric[2]=0x80000000;
        c.rate_hz[0]=c.rate_hz[1]=c.rate_hz[2]=24000000;
        setup(&m,&p,c);m.ccu[0x1908/4]=m.ccu[0x1988/4]=0;m.fail_read=CCU+0x1908;
        assert(finish(&m,&p)==READY && !m.writes && !m.parent_reads);
        puts("PASS DMA selected PERI0 branches: No-Auto/software/effective gates, unselected branches, failed/late reads and retained ownership");
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--pll-input-only")) {
        setup(&m,&p,config(0));
        assert(finish(&m,&p)==READY && !m.writes);
        setup(&m,&p,config(0));m.ccu[0xa0/4]|=2;
        assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
        puts("DMA inherited PERI0 input divisor: unchanged profile admitted, divided input retained without writes");
        return 0;
    }
    setup(&m,&p,c);m.busy_once=1;m.pending_once=1;
    assert(a733_dma_platform_poll(&p)==BUSY && !p.token.id && !m.reads && !m.writes);
    assert(a733_dma_platform_poll(&p)==PENDING && p.token.id && !m.reads);
    uint64_t id=p.token.id;
    assert(a733_dma_platform_poll(&p)==PENDING && p.token.id==id);
    assert(a733_dma_platform_poll(&p)==PENDING && !m.writes);
    /* Another authorized writer changes an unrelated field between locks. */
    m.ccu[0x704/4]|=0x400004;m.ccu[0x5e4/4]|=0x800;
    assert(a733_dma_platform_poll(&p)==PENDING && !m.writes);
    m.now+=20;
    assert(finish(&m,&p)==READY && a733_dma_platform_ready(&p));
    assert(m.ccu[0x704/4]==0x610007 && m.ccu[0x5e4/4]==0x41b03);
    assert(m.writes==5 && m.addresses[0]==CCU+0x704 && m.addresses[1]==CCU+0x704 &&
        m.addresses[2]==CCU+0x5e4 && m.addresses[3]==DMA+0x28 && m.addresses[4]==DMA+0x34);
    assert(m.dma[0x34/4]==0xabcd00ff && m.dma[0x38/4]==0x9876ff00);
    printf("{\"ref\":%u,\"peri\":%u,\"fabric\":[%u,%u,%u],\"rates\":[%u,%u,%u],\"writes\":[",
        m.ccu[0],m.ccu[0xa0/4],c.fabric[0],c.fabric[1],c.fabric[2],c.rate_hz[0],c.rate_hz[1],c.rate_hz[2]);
    for(unsigned i=0;i<m.writes;++i)printf("%s[%lu,%u]",i?",":"",(unsigned long)m.addresses[i],m.values[i]);
    puts("]}");
    setup(&m,&p,config(0));assert(finish(&m,&p)==READY && !m.writes);
    c=config(0);c.fabric[0]=0;c.fabric[1]=c.fabric[2]=0x80000000;
    c.rate_hz[0]=c.rate_hz[1]=c.rate_hz[2]=24000000;
    setup(&m,&p,c);m.ccu[0]=m.ccu[0xa0/4]=0;
    assert(finish(&m,&p)==READY && !m.parent_reads && p.shared==(A733_CLOCK_FABRIC|A733_CLOCK_OSC));
    c=config(0);c.fabric[1]=c.fabric[2]=0x84000000;c.fabric[2]=0x83000000;
    c.rate_hz[1]=c.rate_hz[2]=480000000;
    setup(&m,&p,c);assert(finish(&m,&p)==READY && !m.writes);
    setup(&m,&p,c);m.ccu[0x1908/4]&=~(1u<<7);assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
    c=config(1);uint32_t scopes=0x12345678;
    c.fabric[1]=0x81000000;assert(!a733_dma_platform_scopes(&c,&scopes) && scopes==0x12345678);
    c=config(1);c.route_change_mask=0;assert(!a733_dma_platform_scopes(&c,&scopes));
    setup(&m,&p,config(1));m.bad_handoff=1;assert(finish(&m,&p)==RETAINED && !m.reads && !p.token.id);retained(&m,&p);
    setup(&m,&p,config(1));m.ccu[0x5e4/4]&=~2u;assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
    setup(&m,&p,config(1));m.ccu[0xa0/4]^=1<<8;assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
    setup(&m,&p,config(1));m.ccu[0x704/4]|=0x10001;assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
    setup(&m,&p,config(1));m.dma[(0x100+15*0x40)/4]=1;
    assert(finish(&m,&p)==RETAINED && m.writes==3 && m.dma[0x34/4]==0xabcd0000);retained(&m,&p);
    setup(&m,&p,config(1));m.fail_write=CCU+0x704;
    assert(finish(&m,&p)==RETAINED && m.writes==1 && (m.ccu[0x704/4]&0x10000));retained(&m,&p);
    setup(&m,&p,config(1));m.late_write=DMA+0x34;
    assert(finish(&m,&p)==RETAINED && m.writes==5);retained(&m,&p);
    setup(&m,&p,config(0));m.late_read=DMA+0x28;
    assert(finish(&m,&p)==RETAINED && !m.writes);retained(&m,&p);
    setup(&m,&p,config(1));m.late_acquire=1;
    assert(finish(&m,&p)==RETAINED && p.token.id && !m.reads);retained(&m,&p);
    setup(&m,&p,config(1));m.regress_acquire=1;
    assert(finish(&m,&p)==RETAINED && p.token.id && !m.reads);retained(&m,&p);
    setup(&m,&p,config(0));assert(a733_dma_platform_poll(&p)==PENDING);
    assert(a733_dma_platform_poll(&p)==PENDING);m.late_leave=1;
    assert(a733_dma_platform_poll(&p)==RETAINED && !m.writes);retained(&m,&p);
    puts("DMA platform ownership, clock admission, ordering and retained failures pass");
}
