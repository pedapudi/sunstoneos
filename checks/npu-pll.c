#include "a733_npu_pll.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define CTRL (CCU+0x2a0)
#define BIT(n) (UINT32_C(1)<<(n))
struct model {
    struct a733_npu_pll p;
    struct a733_npu_pll_config authorized;
    struct a733_clock_leases registry;
    uint32_t regs[0x2000/4], osc;
    uint64_t now, armed, third_lock, output_at;
    unsigned locked, retained, retain_lock, calls, reads, writes, acquire_calls;
    unsigned pending, corrupt_token, fail_write, mismatch, late_write, late_leave, regress, never_lock;
    unsigned lock_reads, transitions, fault_seen;
    uint32_t previous;
    struct {uint32_t address,value;uint64_t time;} trace[24];
};
static const unsigned offsets[]={0x500,0x580,0x588,0xa80,0xa88,0xa8c,0xb00,0xb04,0xc00,0xc0c,0x1b10,0x1b14,0x1b1c};
static int now(void *v,uint64_t *out)
{
    struct model *m=v;++m->calls;
    *out=m->regress ? m->now-1 : m->now;return 1;
}
static enum a733_npu_pll_result acquire(void *v,const struct a733_npu_pll_config *c,
    a733_clock_mask_t shared,a733_clock_mask_t exclusive,struct a733_npu_pll_resources *r)
{
    struct model *m=v;++m->calls;++m->acquire_calls;
    assert(!m->locked);
    assert(!memcmp(c,&m->authorized,sizeof(*c)));
    if (!r->clock.id) assert(a733_clock_acquire(&m->registry,c->owner,shared,exclusive,c->deadline_us,&r->clock)==A733_CLOCK_OK);
    else assert(a733_clock_token_status(&m->registry,&r->clock)==A733_CLOCK_OK);
    if (m->pending && m->acquire_calls==1) return A733_NPU_PLL_PENDING;
    if(m->corrupt_token) ++r->clock.id;
    r->whole_platform=97;
    return A733_NPU_PLL_READY;
}
static int enter(void *v,uint64_t deadline)
{
    struct model *m=v;++m->calls;
    assert(m->now<deadline && !m->locked && !m->retained);m->locked=1;return 1;
}
static int leave(void *v)
{
    struct model *m=v;++m->calls;assert(m->locked && !m->p.failed);
    m->locked=0;if(m->late_leave && m->output_at)m->now=m->p.config.deadline_us;return 1;
}
static void retain(void *v,const struct a733_npu_pll_resources *r,
    const struct a733_npu_pll_resources *observed,unsigned lock_state)
{
    struct model *m=v;++m->calls;assert(m->p.failed && !m->retained);
    assert(lock_state==m->locked);m->retained=1;m->retain_lock=lock_state;
    if(r->clock.id) assert(a733_clock_retain(&m->registry,&r->clock)==A733_CLOCK_FAULT);
    if(m->corrupt_token) assert(observed->clock.id==r->clock.id+1);
}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;++m->calls;++m->reads;assert(m->locked && !m->retained);
    if(a==0x07090160) {*out=m->osc;return 1;}
    assert(a>=CCU && a<CCU+0x2000 && !(a&3));
    if(a==CTRL && m->armed && m->now>=m->armed+3 && !m->never_lock) m->regs[0x2a0/4]|=BIT(28);
    if(a==CCU+0x19a0) m->regs[0x19a0/4]=(m->regs[0x1920/4]&0x10001)==0x10001 ? BIT(16) : 0;
    *out=m->regs[(a-CCU)/4];
    if(a==CTRL && m->p.stage==3) { /* LOCK_WAIT, inspect externally timed observations. */
        assert(m->armed && m->now>=m->armed+3);
        if(!m->never_lock && ++m->lock_reads==3)m->third_lock=m->now;
    }
    if(m->mismatch && m->writes && m->trace[m->writes-1].address==CTRL && a==CTRL && !(m->regs[0x2a0/4]&BIT(27))) *out^=BIT(8);
    return 1;
}
static int write_reg(void *v,uintptr_t a,uint32_t x)
{
    struct model *m=v;++m->calls;assert(m->locked && !m->retained && m->writes<24);
    assert(a==CTRL || a==CCU+0x1920);
    unsigned index=m->writes++;
    m->trace[index].address=(uint32_t)a;m->trace[index].value=x;m->trace[index].time=m->now;
    if(m->fail_write) return 0;
    uint32_t old=m->regs[(a-CCU)/4];
    if(a==CTRL) {
        assert(!(x&BIT(28)));
        /* Specification invariants independent of controller step count. */
        assert(((old^x)&~UINT32_C(0xf870ff02))==0);
        assert(!(old&~x&(BIT(31)|BIT(30))));
        if((old^x)&0x70ff02) assert(!(old&BIT(27)) && (x&0x70ff02)==0x305100);
        if(!(old&BIT(31)) && (x&BIT(31))) assert((x&BIT(30)) && (x&0x70ff02)==0x305100);
        if(!(old&BIT(29)) && (x&BIT(29))) {m->armed=m->now;m->lock_reads=0;}
        if(!(old&BIT(27)) && (x&BIT(27))) {
            assert(m->third_lock && m->now>=m->third_lock+20);
            assert((old&0xf0000000)==0xf0000000);m->output_at=m->now;
        }
        /* Factor and lock-enable transitions clear modeled lock. */
        if((old^x)&(0x70ff02|BIT(29))) old&=~BIT(28);
        m->regs[(a-CCU)/4]=x|(old&BIT(28));
    } else {
        assert(!((old^x)&~UINT32_C(0x10001)));
        if(!(old&1) && (x&1)) assert(old&BIT(16));
        m->regs[(a-CCU)/4]=x;
    }
    if(m->late_write)m->now=m->p.config.deadline_us;
    return 1;
}
static const struct a733_npu_pll_operations ops={acquire,enter,leave,retain,now,read_reg,write_reg};
static void setup(struct model *m,enum a733_npu_pll_mode mode)
{
    memset(m,0,sizeof(*m));m->now=10;m->osc=0x10000002;
    m->regs[0]=0xf8000000|(99<<16)|(99<<8);
    m->regs[0x2a0/4]=mode==A733_NPU_PLL_COLD_ENABLE ? 0x00104520 : 0xf8104520;
    m->regs[0x2ac/4]=0x30000000;m->regs[0x2b0/4]=0xb0000;
    m->regs[0x1920/4]=0x40000000; /* unrelated/reserved fields preserved */
    m->regs[0xb04/4]=BIT(19);
    m->regs[0xc0c/4]=mode==A733_NPU_PLL_COLD_ENABLE ? 1 : 0x10001;
    m->regs[0xc00/4]=mode==A733_NPU_PLL_COLD_ENABLE ? 0 : BIT(31); /* PHY DRAM */
    m->regs[0x580/4]=BIT(31);m->regs[0x588/4]=BIT(31);
    m->authorized=(struct a733_npu_pll_config){.owner=7,.handoff=91,.deadline_us=10000,
        .mode=mode,.initial_control=m->regs[0x2a0/4]};
    for(unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)m->authorized.consumers[i]=m->regs[offsets[i]/4];
    assert(a733_clock_leases_init(&m->registry,now,m));
    assert(a733_npu_pll_init(&m->p,&m->authorized,&ops,m));
}
static enum a733_npu_pll_result run(struct model *m)
{
    for(unsigned i=0;i<600;++i) {
        enum a733_npu_pll_result r=a733_npu_pll_poll(&m->p);
        if(r==A733_NPU_PLL_READY || r==A733_NPU_PLL_RETAINED)return r;
        m->now+=3;
    }
    assert(0);return A733_NPU_PLL_RETAINED;
}
static void terminal(struct model *m)
{
    unsigned calls=m->calls,reads=m->reads,writes=m->writes;
    assert(m->retained && a733_npu_pll_poll(&m->p)==A733_NPU_PLL_RETAINED);
    assert(m->calls==calls && m->reads==reads && m->writes==writes);
    assert(m->p.resources.clock.id && m->registry.failed);
}
int main(void)
{
    struct model m;
    for(unsigned mode=1;mode<=2;++mode) {
        setup(&m,(enum a733_npu_pll_mode)mode);m.pending=1;
        assert(run(&m)==A733_NPU_PLL_READY && !m.retained && !m.locked);
        assert((m.regs[0x2a0/4]&0xff70ff02)==0xf8305100);
        assert((m.regs[0x2a0/4]&0xe0)==0x20 && m.regs[0x1920/4]==0x40010001);
        for(unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)assert(m.regs[offsets[i]/4]==m.authorized.consumers[i]);
        struct a733_clock_token denied={0};
        assert(a733_clock_acquire(&m.registry,8,A733_CLOCK_PLL_NPU,0,10000,&denied)==A733_CLOCK_BUSY);
        assert(a733_clock_token_status(&m.registry,&m.p.resources.clock)==A733_CLOCK_OK);
        unsigned calls=m.calls;assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_READY && m.calls==calls);
        printf("mode %u: ordered PLL programming, timed lock, preserved consumers and exclusive ownership PASS\n",mode);
    }
    /* Refuse unsafe authority descriptions before callbacks or storage mutation. */
    unsigned refusals=0;
    for(unsigned bad=0;bad<10;++bad) {
        setup(&m,A733_NPU_PLL_COLD_ENABLE);struct a733_npu_pll_config c=m.authorized;
        switch(bad) {
        case 0:c.consumers[A733_NPU_PLL_MBUS]|=5u<<24;break;
        case 1:c.consumers[A733_NPU_PLL_DRAM]|=4u<<24;break;
        case 2:c.consumers[A733_NPU_PLL_DRAM_BUS]|=BIT(16);break;
        case 3:c.consumers[A733_NPU_PLL_NPU_MODULE]|=BIT(31);break;
        case 4:c.consumers[A733_NPU_PLL_VE_ENC]|=BIT(31);break;
        case 5:c.consumers[A733_NPU_PLL_CM_NPU]|=1;break;
        case 6:c.consumers[A733_NPU_PLL_NSI]|=BIT(27);break;
        case 7:c.initial_control|=BIT(31);break;
        case 8:c.consumers[A733_NPU_PLL_AHB]|=3u<<24;break;
        case 9:c.consumers[A733_NPU_PLL_VE_BUS]|=1;break;
        }
        struct a733_npu_pll empty={0};unsigned calls=m.calls;
        assert(!a733_npu_pll_init(&empty,&c,&ops,&m) && !empty.initialized && m.calls==calls);++refusals;
    }
    for(unsigned bad=0;bad<5;++bad) {
        setup(&m,A733_NPU_PLL_ENABLED_RETUNE);
        switch(bad) {
        case 0:m.regs[0x2ac/4]|=BIT(27);break;
        case 1:m.regs[0x2b0/4]=0;break;
        case 2:m.regs[0xc00/4]^=1;break;
        case 3:m.regs[0x2a4/4]|=1;break;
        case 4:m.regs[0]=0;break;
        }
        assert(run(&m)==A733_NPU_PLL_RETAINED && !m.writes && m.retain_lock==1);terminal(&m);++refusals;
    }
    printf("%u unsafe config/source/state refusals PASS\n",refusals);
    for(unsigned failure=0;failure<5;++failure) {
        setup(&m,A733_NPU_PLL_ENABLED_RETUNE);
        switch(failure) {
        case 0:m.fail_write=1;break;
        case 1:m.mismatch=1;break;
        case 2:m.late_write=1;break;
        case 3:m.late_leave=1;break;
        case 4:m.never_lock=1;break;
        }
        assert(run(&m)==A733_NPU_PLL_RETAINED);
        assert(m.retain_lock==(failure==3 ? 0u : 1u));terminal(&m);
        if(failure==4)assert(m.p.samples==100 && !m.output_at);
    }
    setup(&m,A733_NPU_PLL_ENABLED_RETUNE);m.pending=1;
    assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_PENDING);
    m.corrupt_token=1;assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_RETAINED);terminal(&m);
    setup(&m,A733_NPU_PLL_ENABLED_RETUNE);
    assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_PENDING);
    m.regress=1;assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_RETAINED);terminal(&m);
    puts("partial write/readback/deadline/lock exhaustion/token replacement/time regression retention PASS");
    return 0;
}
