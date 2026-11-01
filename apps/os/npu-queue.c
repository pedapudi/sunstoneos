/* EL0 SPSC adapter of the existing resident NPU direct queue. */
#include "os.h"
#include "npu.h"
#include "npu_runtime_queue.h"
#include "sunstone_npu_queue_service.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include <string.h>
/* Applications may provide one constant edge table in their image. */
__attribute__((weak)) const struct ss_npu_queue_edges ss_npu_queue_edges={0};
#define MAX_EDGES (SS_NPU_QUEUE_MAX_HANDLES*SS_NPU_QUEUE_MAX_TENSORS)
static struct ss_npu_queue_edge edges[MAX_EDGES];
static size_t edge_count;
static unsigned edges_prepared;
int os_npu_queue_prepare_edges(struct npu_runtime *rt)
{
    if(!rt||edges_prepared||ss_npu_queue_edges.count>MAX_EDGES||
       (ss_npu_queue_edges.count&&!ss_npu_queue_edges.edges))return 0;
    edge_count=ss_npu_queue_edges.count;
    if(edge_count)memcpy(edges,ss_npu_queue_edges.edges,edge_count*sizeof(*edges));
    for(size_t i=0;i<edge_count;++i){
        const struct ss_npu_queue_edge *e=&edges[i];
        if(e->producer>=rt->slots||e->consumer>=rt->slots||e->producer==e->consumer||
           e->output>=rt->slot[e->producer].layout.outputs||e->input>=rt->slot[e->consumer].layout.inputs)return 0;
        for(size_t j=0;j<i;++j)
            if(edges[j].consumer==e->consumer&&edges[j].input==e->input)return 0;
    }
    for(size_t i=0;i<edge_count;++i){
        const struct ss_npu_queue_edge *e=&edges[i];
        if(!npu_runtime_link_tensor(rt,e->producer,e->output,e->consumer,e->input))return 0;
    }
    edges_prepared=1;return 1;
}
static struct ss_npu_queue ring;
static struct npu_runtime_queue direct;
static struct npu_queue_command commands[SS_NPU_QUEUE_MAX_HANDLES];
static struct npu_queue_job job;
static struct npu_queue_fence hardware_fence;
static unsigned opened,closed,cache_stride;
static uint64_t original_sctlr;
static BaseType_t *irq_woken;
static unsigned in_svc;
static uintptr_t identity(void *unused)
{(void)unused;return (uintptr_t)xTaskGetCurrentTaskHandle();}
static int on_core(void *unused)
{
    (void)unused;
    /* TPIDRRO is the port's EL0-safe task/core publication. */
    return portGET_CORE_ID()==OS_NPU_CORE;
}
static void request(void *unused)
{
    (void)unused;
    /* Wake the existing mailbox owner through its persistent RTOS event. */
    extern void os_npu_queue_notify(void);
    os_npu_queue_notify();
}
static void notify(void *unused,uintptr_t producer,int isr)
{
    (void)unused;if(!producer||in_svc)return;
    /* The SVC's producer is already running and observes failure on return.
     * No task-level notification/critical section may unmask this exception. */
    if(isr)vTaskNotifyGiveFromISR((TaskHandle_t)producer,irq_woken);
    else xTaskNotifyGive((TaskHandle_t)producer);
}
static void wait_event(void *unused)
{(void)unused;(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);}
static int mapped(struct npu_runtime_region r)
{
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,
        {{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    return r.cpu&&r.bytes&&!((r.cpu|r.bytes)&63)&&
        a733_mapping_check_stride(aarch64_device_translate_el0,r.cpu,r.physical,r.bytes,4096,&wb);
}
static int open_queue(void *unused,unsigned depth)
{
    (void)unused;(void)depth;
    if(opened||os_npu_stopping()||!os_npu_queue_begin())return -1;
    struct npu_runtime *rt=os_npu_queue_runtime();
    if(!rt||!edges_prepared||rt->slots>SS_NPU_QUEUE_MAX_HANDLES||
       (cache_stride=aarch64_device_cache_stride())!=64)return -1;
    for(unsigned i=0;i<rt->slots;++i){
        struct npu_runtime_slot *s=&rt->slot[i];
        struct ss_npu_tensor in[SS_NPU_QUEUE_MAX_TENSORS],out[SS_NPU_QUEUE_MAX_TENSORS];
        if(s->layout.inputs>SS_NPU_QUEUE_MAX_TENSORS||s->layout.outputs>SS_NPU_QUEUE_MAX_TENSORS||
           !mapped(s->read)||!mapped(s->write))return -1;
        for(unsigned j=0;j<s->layout.inputs;++j)in[j]=(struct ss_npu_tensor){s->tensors[j].data,s->tensors[j].bytes};
        for(unsigned j=0;j<s->layout.outputs;++j)out[j]=(struct ss_npu_tensor){s->tensors[s->layout.inputs+j].data,s->tensors[s->layout.inputs+j].bytes};
        if(!ss_npu_queue_service_handle(&ring,in,s->layout.inputs,out,s->layout.outputs,
            (struct ss_npu_cache_span){s->read.cpu,s->read.bytes},
            (struct ss_npu_cache_span){s->write.cpu,s->write.bytes}))return -1;
    }
    if(!ss_npu_queue_service_edges(&ring,edges,edge_count))return -1;
    if(npu_runtime_queue_claim(&direct,rt)!=1)return -1;
    for(unsigned i=0;i<rt->slots;++i)
        if(!npu_queue_register(&direct.queue,&commands[i],&rt->commands[i+1]))return -1;
    /* Only the pinned producer on this core performs CIVAC. The same WB
     * identity view is used at EL0 and EL1; no NC alias is introduced. */
    __asm__ volatile("mrs %0,sctlr_el1":"=r"(original_sctlr));
    uint64_t enabled=original_sctlr|(UINT64_C(1)<<26);
    __asm__ volatile("msr sctlr_el1,%0\n isb"::"r"(enabled):"memory");
    ring.job_limit=os_session()->npu.jobs;
    opened=1;return 1;
}
static int close_queue(void *unused)
{
    (void)unused;if(!opened||os_npu_progress()!=1)return -1;
    int r=npu_runtime_queue_return(&direct);if(r!=1)return r;
    __asm__ volatile("msr sctlr_el1,%0\n isb"::"r"(original_sctlr):"memory");
    os_npu_queue_end();
    if(os_npu_close()!=1)return -1;
    closed=1;return 1;
}
static int start(void *unused,unsigned handle,uint64_t sequence,uint64_t after,uint64_t timeout)
{
    (void)unused;
    const struct os_session *s=os_session();
    if(!opened||closed||os_npu_stopping()||!s||sequence>s->npu.jobs||handle>ring.handles||!handle)return -1;
    struct npu_queue_fence dependency={0};
    if(after)dependency=(struct npu_queue_fence){&direct.queue,after};
    /* The handle lease keeps every DMA buffer pinned across record release. */
    return npu_queue_submit(&direct.queue,&job,&commands[handle-1],dependency,timeout,&hardware_fence);
}
static int finish(void *unused,uint64_t sequence)
{
    (void)unused;
    return hardware_fence.value==sequence&&npu_queue_fence_status(&direct.queue,hardware_fence)==1&&
        npu_queue_release(&direct.queue,&job)?1:-1;
}
static void cache(void *unused,uintptr_t address,size_t bytes)
{
    (void)unused;
    /* UCI permits CIVAC at EL0. Outputs have no dirty CPU lines during the
     * device lease, so CIVAC after completion cannot overwrite device data.
     * IVAC is deliberately avoided: it is undefined at EL0. */
    for(size_t offset=0;offset<bytes;offset+=cache_stride)
        __asm__ volatile("dc civac,%0"::"r"(address+offset):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
int ss_npu_queue_svc(void)
{
    /* Entered with local IRQs masked by the real FreeRTOS SVC handler. */
    if(!opened||closed||!on_core(NULL)||identity(NULL)!=ring.producer)return -1;
    in_svc=1;
    int result=ss_npu_queue_service_progress(&ring,0,0);
    in_svc=0;return result;
}
static int doorbell(void *unused)
{
    (void)unused;register long result __asm__("x0");
    __asm__ volatile("svc #120":"=r"(result)::"memory");return (int)result;
}
static void progress(void *unused,BaseType_t *woken)
{
    (void)unused;irq_woken=woken;
    if(atomic_load_explicit(&direct.runtime->failed,memory_order_acquire))ss_npu_queue_service_fail(&ring,1);
    else (void)ss_npu_queue_service_progress(&ring,1,1);
    irq_woken=NULL;
}
int os_npu_queue_sdk_bind(void)
{
    const struct ss_npu_queue_backend b={identity,on_core,request,open_queue,close_queue,
        start,finish,cache,doorbell,notify,wait_event};
    direct.progress_from_isr=progress;direct.progress_cookie=NULL;
    return ss_npu_queue_service_bind(&ring,&b,NULL);
}
int os_npu_queue_sdk_step(void){return ss_npu_queue_service_step(&ring);}
int os_npu_queue_sdk_closed(void){return closed;}
void os_npu_queue_sdk_fail(void){ss_npu_queue_service_fail(&ring,0);}
