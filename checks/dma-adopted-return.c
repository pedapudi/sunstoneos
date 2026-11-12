#define main dma_platform_existing_cases
#include "dma-platform.c"
#undef main
#include "a733_dma_regs.h"

static int locked_busy(void *cookie,uint64_t deadline)
{struct model *m=cookie;assert(m->now<deadline);m->now+=10;return 0;}
static void adopted(struct model *m,struct a733_dma_platform *p)
{
    struct a733_dma_platform_config c=config(0);c.route_change_mask=0;
    setup(m,p,c);assert(finish(m,p)==READY && !m->writes);
    /* Startup's deadline has elapsed; retirement gets its own bounded window. */
    m->now=11000;m->deadline=12000;
}
static void cold_started(struct model *m,struct a733_dma_platform *p)
{
    setup(m,p,config(1));assert(finish(m,p)==READY&&m->writes);
    assert(p->config.cold&&p->config.expected_cpu!=p->config.desired_cpu);
    m->now=11000;m->deadline=12000;
}
int main(void)
{
    struct model m;struct a733_dma_platform p;
    adopted(&m,&p);
    uint64_t token=p.token.id,original_deadline=p.config.deadline_us;
    unsigned before=m.reads;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==READY);
    assert(m.reads>before && !m.writes && !m.locked && p.token.id==token &&
           p.config.deadline_us==original_deadline && !p.failed);
    adopted(&m,&p);m.dma[0x34/4]^=1;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && p.failed && !m.writes && !m.locked);
    assert(p.retire_fault==A733_DMA_RETIRE_CONTROLLER);
    unsigned first=p.retire_fault;uint64_t when=p.retire_now;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline+100)==RETAINED &&
           p.retire_fault==first && p.retire_now==when);
    adopted(&m,&p);m.ccu[0x5e4/4]&=~1u;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && !m.writes && !m.locked);
    assert(p.retire_fault==A733_DMA_RETIRE_BASELINE);
    adopted(&m,&p);m.dma[0x30/4]=1;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && !m.writes && !m.locked);
    adopted(&m,&p);m.fail_read=DMA+0x30;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && !m.writes && !m.locked);
    adopted(&m,&p);before=m.reads;m.now=m.deadline;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && m.reads==before && !m.writes);
    assert(p.retire_fault==A733_DMA_RETIRE_TIME);
    adopted(&m,&p);m.late_leave=1;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED && p.failed &&
           m.now==m.deadline && !m.locked && !m.writes);
    assert(p.retire_fault==A733_DMA_RETIRE_AFTER_LEAVE && p.retire_now==m.deadline);
    adopted(&m,&p);p.ops.enter=locked_busy;
    unsigned busy=0;
    while(a733_dma_platform_verify_adopted(&p,m.deadline)==BUSY){assert(++busy<=100);}
    assert(busy>1 && p.failed && m.now>=m.deadline && !m.writes && !m.locked);
    assert(p.retire_fault==A733_DMA_RETIRE_ENTER);
    cold_started(&m,&p);
    struct a733_dma_platform_config original=p.config;struct a733_clock_token held=p.token;
    before=m.writes;
    assert(a733_dma_platform_verify_adopted(&p,m.deadline)==RETAINED&&!p.failed);
    assert(a733_dma_platform_verify_retired(&p,m.deadline)==READY&&m.writes==before);
    assert(!memcmp(&original,&p.config,sizeof(original))&&!memcmp(&held,&p.token,sizeof(held)));
    cold_started(&m,&p);before=m.writes;m.dma[A733_DMA_REG_CPU_ROUTE/4]=p.config.expected_cpu;
    assert(a733_dma_platform_verify_retired(&p,m.deadline)==RETAINED&&m.writes==before);
    cold_started(&m,&p);before=m.writes;m.dma[A733_DMA_REG_MCU_ROUTE/4]^=1;
    assert(a733_dma_platform_verify_retired(&p,m.deadline)==RETAINED&&m.writes==before);
    cold_started(&m,&p);before=m.writes;m.dma[A733_DMA_REG_AUTO_GATE/4]=0;
    assert(a733_dma_platform_verify_retired(&p,m.deadline)==RETAINED&&m.writes==before);
    const unsigned quiet_offsets[]={A733_DMA_QUIET_REGISTERS};
    for(unsigned ch=0;ch<16;++ch)for(unsigned j=0;j<sizeof(quiet_offsets)/sizeof(*quiet_offsets);++j){
        cold_started(&m,&p);before=m.writes;
        m.dma[(A733_DMA_REG_CHANNELS+ch*A733_DMA_REG_CHANNEL_STRIDE+quiet_offsets[j])/4]=1;
        assert(a733_dma_platform_verify_retired(&p,m.deadline)==RETAINED&&m.writes==before&&!m.locked);
        assert(p.retire_fault==A733_DMA_RETIRE_CONTROLLER&&p.config.cold);
    }
    setup(&m,&p,config(1));before=m.reads;
    assert(a733_dma_platform_verify_retired(&p,m.deadline)==RETAINED&&m.reads==before&&!m.writes);
    puts("PASS DMA final verification: adopted and cold routes, 96 dirty channel registers, unchanged token/config, no return writes, bounded contention");
    return 0;
}
