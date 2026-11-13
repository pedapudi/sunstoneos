/* Finite consumer of the OS RAM-copy owner; no direct device access. */
#include "dma_events-app.h"
#include "sunstone_dma.h"
#include "report.h"
#include <string.h>
static uint64_t deadline;
static uint64_t now(void){return ss_counter_ticks()/24;}
static void wait(void)
{
    uint64_t t=now();
    if(ss_live()<0||t>=deadline)ss_fail(0xd01);
    ss_progress();ss_wait_until(deadline);
}
static void sleep_until(uint64_t until)
{
    for(;;){
        uint64_t t=now();if(t>=until)return;
        if(ss_live()<0||t>=deadline)ss_fail(0xd02);
        ss_progress();ss_wait_until(until);
    }
}
static void accepted(int (*operation)(void),unsigned site)
{
    int r;
    while(!(r=operation()))wait();
    if(r<0)ss_fail(site);
}
static void top(void)
{
    char line[256];
    const char expected[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    while(!ss_clock_ready()||ss_throttled())wait();
    if(ss_clock_info(line,sizeof(line))<(int)(sizeof(expected)-1)||memcmp(line,expected,sizeof(expected)-1))ss_fail(0xd03);
}
void dma_events_main(const struct ss_grant *grant)
{
    if(!grant||!grant->finish_by_us)ss_fail(0xd04);
    deadline=grant->finish_by_us;top();
    struct dma_event_result result={.magic=UINT64_C(0x31545645414d444f),.version=1};
    struct ss_dma_buffers buffers;
    int r;while(!(r=ss_dma_open(&buffers)))wait();if(r<0||buffers.bytes!=4096)ss_fail(0xd05);
    /* Let the OPEN publication leave the worker's notification before the
     * declared one-second no-event observation window. */
    sleep_until(now()+DMA_EVENT_IDLE_US);
    if(!ss_dma_owner_stats(&result.before)||!result.before.waiting)ss_fail(0xd06);
    result.idle_begin_us=now();sleep_until(result.idle_begin_us+DMA_EVENT_IDLE_US);result.idle_end_us=now();
    if(!ss_dma_owner_stats(&result.after)||!result.after.waiting||
       result.after.wakes!=result.before.wakes||result.after.idle_timeouts!=result.before.idle_timeouts||
       result.after.irqs!=result.before.irqs)ss_fail(0xd07);
    ss_activity_begin();
    for(unsigned job=0;job<DMA_EVENT_JOBS;++job){
        struct ss_dma_owner_stats started,completed;
        if(!ss_dma_owner_stats(&started))ss_fail(0xd0f);
        uint8_t *source=buffers.source,*destination=buffers.destination;
        for(unsigned i=0;i<buffers.bytes;++i){source[i]=(uint8_t)(i*17u+job*31u);destination[i]=0xa5;}
        accepted(ss_dma_submit,0xd08);accepted(ss_dma_complete,0xd09);
        for(unsigned i=0;i<buffers.bytes;++i)if(destination[i]!=(uint8_t)(i*17u+job*31u))ss_fail_value(0xd0a,i);
        if(!ss_dma_owner_stats(&completed)||completed.irqs<=started.irqs||
           completed.deadline_expiries!=started.deadline_expiries)ss_fail(0xd10);
        result.job_irqs[job]=completed.irqs-started.irqs;
        result.job_deadline_expiries[job]=completed.deadline_expiries-started.deadline_expiries;
        ++result.jobs;result.compared_bytes+=buffers.bytes;accepted(ss_dma_reap,0xd0b);
    }
    accepted(ss_dma_close,0xd0c);accepted(ss_dma_retire,0xd0d);
    if(!ss_dma_owner_stats(&result.stopped)||!result.stopped.retired||
       result.stopped.irqs-result.after.irqs<DMA_EVENT_JOBS)ss_fail(0xd0e);
    ss_activity_end(1);ss_finish(&result,sizeof(result));
}
