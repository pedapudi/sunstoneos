#include "a733_dma_copy_application.h"
#include "platform.h"

static struct a733_counter_clock clock_source;
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_dma_pool_session session;
static StaticTask_t client_state;
static StackType_t client_stack[3072];
struct a733_dma_copy_result a733_dma_copy_result;
/* Boot copies only the client's service handle and admitted buffer locations. */
static struct {
    struct dma_service *service;
    const atomic_uint *state;
    unsigned count,bytes;
    unsigned channels[DMA_SERVICE_JOBS];
    unsigned char *source[DMA_SERVICE_JOBS],*destination[DMA_SERVICE_JOBS];
} client_view;
static _Noreturn void fatal(void *unused)
{ (void)unused;platform_panic("DMA copy reservation",0); }
static struct dma_service_reply request(enum dma_service_operation op,unsigned job,uint64_t timeout)
{
    struct dma_service_request r={.operation=op,.job=job,.timeout=timeout};
    struct dma_service_reply reply;
    uint64_t ticket;
    configASSERT(dma_service_send(client_view.service,&r,100,&ticket));
    configASSERT(dma_service_collect(client_view.service,ticket,&reply,100));
    configASSERT(reply.result==DMA_SERVICE_OK);
    return reply;
}
static void client(void *unused)
{
    (void)unused;
    unsigned state=A733_DMA_POOL_WAITING;
    for(unsigned waits=0;waits<2000 && state==A733_DMA_POOL_WAITING;++waits) {
        state=atomic_load_explicit(client_view.state,memory_order_acquire);
        if(state==A733_DMA_POOL_WAITING) vTaskDelay(1);
    }
    configASSERT(state==A733_DMA_POOL_READY);
    for(unsigned job=0;job<client_view.count;++job) {
        unsigned char *src=client_view.source[job],*dst=client_view.destination[job];
        configASSERT(request(DMA_SERVICE_OPEN,job,0).phase==DMA_SERVICE_READY);
        for(unsigned i=0;i<4096;++i) {src[i]=(unsigned char)(i*29+job);dst[i]=0xa5;}
        struct dma_service_reply r=request(DMA_SERVICE_SUBMIT,job,24000000);
        for(unsigned waits=0;waits<2000 && r.phase!=DMA_SERVICE_RESULT;++waits) {
            configASSERT(r.phase==DMA_SERVICE_RUNNING || r.phase==DMA_SERVICE_DRAINING);
            vTaskDelay(1);r=request(DMA_SERVICE_STATUS,job,0);
        }
        configASSERT(r.phase==DMA_SERVICE_RESULT && r.outcome==DMA_SERVICE_COMPLETE && r.holds_buffers);
        /* Complete payload consumption before relinquishing its service hold. */
        for(unsigned i=0;i<4096;++i)
            configASSERT(dst[i]==(i<client_view.bytes ? src[i] : 0xa5));
        configASSERT(request(DMA_SERVICE_REAP,job,0).phase==DMA_SERVICE_READY);
        configASSERT(request(DMA_SERVICE_CLOSE,job,0).phase==DMA_SERVICE_CLOSED);
        atomic_fetch_or_explicit(&a733_dma_copy_result.completed_channels,
            UINT32_C(1)<<client_view.channels[job],memory_order_relaxed);
    }
    atomic_store_explicit(&a733_dma_copy_result.complete,1,memory_order_release);
    /* Preserve the worker, clock lease and parent allocation after completion. */
    vTaskSuspend(NULL);fatal(NULL);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{ (void)task;platform_panic(name,0); }
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());
    configASSERT(a733_counter_clock_init(&clock_source,24000000));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    const struct a733_dma_pool_config config={&a733_dma_copy_handoff,&clock_source,&registry,&locks,
        240000,1024,4,3,fatal,NULL};
    configASSERT(a733_dma_pool_session_init(&session,&config));
    client_view.service=&session.service;client_view.state=&session.state;
    client_view.count=session.count;client_view.bytes=session.copy_bytes;
    for(unsigned i=0;i<session.count;++i) {
        client_view.source[i]=session.mappings[i][0].cpu;
        client_view.destination[i]=session.mappings[i][1].cpu;
        client_view.channels[i]=session.jobs[i].channel;
    }
    TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"dma-copy",3072,NULL,2,
        client_stack,&client_state,2);
    configASSERT(task && dma_service_bind(&session.service,0,task,(UINT32_C(1)<<session.count)-1));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
