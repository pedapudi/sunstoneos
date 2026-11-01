#define main dma_platform_existing_cases
#include "dma-platform.c"
#undef main

static int locked_busy(void *cookie,uint64_t deadline)
{struct model *m=cookie;assert(m->now<deadline);m->now+=10;return 0;}
static void adopted(struct model *m,struct a733_dma_platform *p)
{
    struct a733_dma_platform_config c=config(0);c.route_change_mask=0;
    setup(m,p,c);assert(finish(m,p)==READY && !m->writes);
    /* Startup's deadline has elapsed; retirement gets its own bounded window. */
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
    puts("PASS adopted DMA return: no writes, unchanged token/config, changed route/gate, busy engine, read failure and finite lock contention");
    return 0;
}
