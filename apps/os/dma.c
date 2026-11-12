/* Fixed RAM-copy catalogue, adopted DMA0 and explicit normal retirement. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_dma_pool_session.h"
#include "dma_service_i2s_irq.h"
#include "sunstone_dma.h"
#include "boot.h"
#include "quiesce.h"
#include "sunstone_crumb.h"

__attribute__((used,retain)) const uint64_t os_dma_profile[3]={
    UINT64_C(0x41533733444d4131),1,4096
};
static struct {
    struct a733_dma_handoff handoff;
    struct a733_dma_pool_config config;
    struct a733_dma_pool_session pool;
    struct dma_service_i2s_irq irq;
    uint64_t ticket;
    unsigned operation,retire_reply,mask_fault;
    int mask_result;
    uint64_t mask_deadline,mask_now;
    atomic_uint ready;
} dma;
static enum os_quiesce_result dma_quiesce_step(struct os_quiesce *q)
{
    if(atomic_load_explicit(&dma.ready,memory_order_acquire))return OS_QUIESCE_DONE;
    unsigned state=atomic_load_explicit(&dma.pool.state,memory_order_acquire);
    if(state==A733_DMA_POOL_WAITING)return OS_QUIESCE_PENDING;
    if(state==A733_DMA_POOL_READY){
        int stopped=dma_service_quiesce(&dma.pool.service);
        if(!stopped)return OS_QUIESCE_PENDING;
        if(stopped>0){
            int retired=a733_dma_pool_session_retire(&dma.pool);
            if(!retired)return OS_QUIESCE_PENDING;
            if(retired>0){atomic_store_explicit(&dma.ready,1,memory_order_release);return OS_QUIESCE_DONE;}
        }
    }
    (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x811,
        state|(atomic_load_explicit(&dma.pool.service.quiesce_fault,memory_order_acquire)<<8)|
        (dma.mask_fault<<24),SUNSTONE_CRUMB_HERE());
    return OS_QUIESCE_FAULT;
}
static struct os_quiesce dma_quiesce={.owner=OS_QUIESCE_DMA,
    .order=OS_QUIESCE_DEVICES,.step=dma_quiesce_step};
static _Noreturn void fatal(void *unused)
{
    (void)unused;
    if(os_quiesce_fault(&dma_quiesce,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x812,
        atomic_load_explicit(&dma.pool.state,memory_order_acquire),SUNSTONE_CRUMB_HERE()))
        for(;;)vTaskSuspend(NULL);
    platform_panic("DMA retained state",1);
}
int os_dma_task_privileged(void (*entry)(void *),void *arg)
{return a733_dma_pool_task_privileged(entry,arg)==pdTRUE;}
int os_dma_pins(struct a733_pin_handoff *unused)
{(void)unused;return os_session_admitted()!=NULL;}
static int mask(void *cookie)
{
    struct dma_service_i2s_irq *irq=cookie;
    dma.mask_now=a733_smp_counter();
    if(dma.mask_now>UINT64_MAX-A733_SMP_COUNTER_HZ/10000){dma.mask_fault=4;return -1;}
    dma.mask_deadline=dma.mask_now+A733_SMP_COUNTER_HZ/10000;
    int result=dma.mask_result=gicv3_spi_mask(&irq->spi);
    if(result!=1){dma.mask_fault=1;return result;}
    /* GICD_CTLR.RWP clears before ISENABLER can confirm the SPI is disabled. */
    for(;;){
        result=dma.mask_result=gicv3_spi_masked(&irq->spi);
        dma.mask_now=a733_smp_counter();
        if(result<0||dma.mask_now>=dma.mask_deadline){dma.mask_fault=result<0?2:3;return -1;}
        if(result==1){irq->masked=1;return 1;}
    }
}
int os_dma_start(void)
{
    const struct os_session *s=os_session_admitted();
    uint64_t now;
    if(!s||!ss_application.task||!a733_counter_clock_now(&providers.shared.counter,&now)||
       now>UINT64_MAX-100000)return 0;
    if(!os_quiesce_register(&dma_quiesce))return 0;
    dma.handoff=s->dma.handoff;
    dma.handoff.allowed.deadline_us=now+100000;
    dma.config=(struct a733_dma_pool_config){
        .handoff=&dma.handoff,.clock=&providers.shared.counter,
        .registry=&providers.shared.clocks,.locks=&providers.shared.locks,
        .drain_ticks=A733_SMP_COUNTER_HZ/100,.copy_bytes=4096,
        .priority=4,.affinity=1,.fatal=fatal
    };
    return a733_dma_pool_session_init(&dma.pool,&dma.config)&&
        dma_service_bind(&dma.pool.service,0,(TaskHandle_t)ss_application.task,1)&&
        dma_service_i2s_irq_bind(&dma.irq,&dma.pool.service,A733_SMP_COUNTER_HZ)&&
        dma_service_bind_retirement(&dma.pool.service,mask,&dma.irq);
}
void os_dma_irq(void){dma_service_i2s_irq_handler(&dma.irq);}
/* DMA0 has one interrupt line, INTID 143, and the table one route for it. Admission grants the RAM-copy service or the
 * I2S stream, never both (apps/os/grants.c), and only the granted one starts and enables the line. */
void os_dma0_irq(void){if(os_granted(OS_GRANT_DMA))os_dma_irq();else os_audio_irq();}
int os_dma_return_ready(void)
{return atomic_load_explicit(&dma.ready,memory_order_acquire)!=0;}
static int caller(void)
{
    return xTaskGetCurrentTaskHandle()==(TaskHandle_t)ss_application.task&&
        !os_dma_return_ready()&&!os_quiesce_requested(&dma_quiesce);
}
static int request(enum dma_service_operation op,struct dma_service_reply *reply)
{
    if(!caller())return -1;
    unsigned state=atomic_load_explicit(&dma.pool.state,memory_order_acquire);
    if(state==A733_DMA_POOL_FAILED)return -1;
    if(state!=A733_DMA_POOL_READY)return 0;
    if(!dma.ticket){
        struct dma_service_request r={.operation=op,.job=0,
            .timeout=op==DMA_SERVICE_SUBMIT?A733_SMP_COUNTER_HZ/2:0};
        if(!dma_service_send(&dma.pool.service,&r,0,&dma.ticket))return 0;
        dma.operation=op;
    }
    if(dma.operation!=(unsigned)op)return -1;
    if(!dma_service_collect(&dma.pool.service,dma.ticket,reply,0))return 0;
    dma.ticket=0;
    if(reply->result==DMA_SERVICE_BUSY)return 0;
    return reply->result==DMA_SERVICE_OK?1:-1;
}
int ss_dma_open(struct ss_dma_buffers *out)
{
    struct dma_service_reply r;
    if(!out)return -1;
    int ready=request(DMA_SERVICE_OPEN,&r);
    if(ready!=1)return ready;
    if(r.phase!=DMA_SERVICE_READY||!r.owns_session||r.holds_buffers)return -1;
    *out=(struct ss_dma_buffers){dma.pool.mappings[0][0].cpu,
        dma.pool.mappings[0][1].cpu,4096,0};
    return 1;
}
int ss_dma_submit(void)
{
    struct dma_service_reply r;int ok=request(DMA_SERVICE_SUBMIT,&r);
    if(ok!=1)return ok;
    return r.holds_buffers&&(r.phase==DMA_SERVICE_RUNNING||r.phase==DMA_SERVICE_RESULT)?1:-1;
}
int ss_dma_complete(void)
{
    struct dma_service_reply r;int ok=request(DMA_SERVICE_STATUS,&r);
    if(ok!=1)return ok;
    if(r.phase==DMA_SERVICE_RUNNING||r.phase==DMA_SERVICE_DRAINING)return 0;
    return r.phase==DMA_SERVICE_RESULT&&r.outcome==DMA_SERVICE_COMPLETE&&r.holds_buffers?1:-1;
}
int ss_dma_reap(void)
{
    struct dma_service_reply r;int ok=request(DMA_SERVICE_REAP,&r);
    if(ok!=1)return ok;
    return r.phase==DMA_SERVICE_READY&&!r.holds_buffers?1:-1;
}
int ss_dma_close(void)
{
    struct dma_service_reply r;int ok=request(DMA_SERVICE_CLOSE,&r);
    if(ok!=1)return ok;
    return r.phase==DMA_SERVICE_CLOSED&&!r.holds_buffers&&!r.owns_session?1:-1;
}
int ss_dma_retire(void)
{
    if(os_dma_return_ready())return 1;
    if(!dma.retire_reply){
        struct dma_service_reply reply;int r=request(DMA_SERVICE_RETIRE,&reply);
        if(r!=1)return r;
        dma.retire_reply=1;
    }
    if(!caller())return -1;
    if(!dma_service_retired(&dma.pool.service))return 0;
    int r=a733_dma_pool_session_retire(&dma.pool);
    if(r==1)atomic_store_explicit(&dma.ready,1,memory_order_release);
    return r;
}
