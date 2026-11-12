/* EL0 SPSC adapter of the existing resident NPU direct queue. */
#include "os.h"
#include "npu.h"
#include "npu_runtime_queue.h"
#include "sunstone_npu_queue_service.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "sunstone_crumb.h"
#include "../board/application.h"
#include "app_large.h"
#include <string.h>
/* The CPU's own instructions of the adapter: on the board they are the real ones; a host build of the open path (the check
 * checks/os-npu-service.py) has no CPU to configure, so they do nothing there. */
#ifdef __aarch64__
#define CPU_READ_SCTLR(v) __asm__ volatile("mrs %0,sctlr_el1":"=r"(v))
#define CPU_WRITE_SCTLR(v) __asm__ volatile("msr sctlr_el1,%0\n isb"::"r"(v):"memory")
#define CPU_CLEAN_INVALIDATE(a) __asm__ volatile("dc civac,%0"::"r"(a):"memory")
#define CPU_DSB() __asm__ volatile("dsb sy":::"memory")
#else
#define CPU_READ_SCTLR(v) ((v)=0)
#define CPU_WRITE_SCTLR(v) ((void)(v))
#define CPU_CLEAN_INVALIDATE(a) ((void)(a))
#define CPU_DSB() ((void)0)
#endif
/* Applications may provide one constant edge table in their image. */
__attribute__((weak)) const struct ss_npu_queue_edges ss_npu_queue_edges={0};
/* The adapter's tables come from the runtime's reservation in the NPU pool (os_npu_queue_tables), sized by the declaration:
 * the image's constant edges and those the application may add, the private copy of the requested edges and the hardware
 * command record of each handle. */
static struct ss_npu_queue_edge *edges,*requested_copy;
static size_t edge_capacity,copy_capacity;
static struct npu_queue_command *commands;
_Static_assert(sizeof(struct npu_queue_command)<=64,"the runtime reserves 64 bytes for each handle's command record");
static size_t edge_count;
static unsigned edges_ready;
static unsigned edges_prepared;
int os_npu_queue_prepare_edges(struct npu_runtime *rt)
{
    if(!rt||edges_prepared||!edges_ready||ss_npu_queue_edges.count>edge_capacity||
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
/* The queue's tables, taken at start (before the scheduler). The ring's handles, groups and requested-edge slots are in the pool,
 * where EL0 reads and writes them by design. The edge tables and the command records are what EL1 trusts: they come from the
 * metadata allocator, where no EL0 mapping reaches. 0 when either block is too small for the declaration. */
int os_npu_queue_tables(struct npu_runtime *rt)
{
    if(!rt||!rt->queue_tables||!rt->queue_meta||edges_ready)return 0;
    const size_t edge_bytes=((rt->queue_image_links+rt->queue_links)*sizeof(*edges)+63u)&~(size_t)63;
    const size_t copy_bytes=(rt->queue_links*sizeof(*requested_copy)+63u)&~(size_t)63;
    if(ss_npu_queue_meta_bytes(rt->slots,rt->queue_image_links,rt->queue_links)>rt->queue_meta_bytes||
       ss_npu_queue_tables_bytes(rt->slots,rt->queue_tensors,rt->queue_links,rt->queue_depth)>rt->queue_bytes||
       !ss_npu_queue_service_tables(&ring,rt->slots,rt->queue_tensors,rt->queue_links,rt->queue_depth,rt->queue_tables,
                                    rt->queue_bytes))return 0;
    uint8_t *at=rt->queue_meta;
    edges=(void *)at;at+=edge_bytes;requested_copy=(void *)at;at+=copy_bytes;commands=(void *)at;
    edge_capacity=rt->queue_image_links+rt->queue_links;copy_capacity=rt->queue_links;
    edges_ready=1;return 1;
}
static struct npu_runtime_queue direct;
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
    return portGET_CORE_ID()==os_npu_queue_core();
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
        a733_mapping_check_stride(os_large_translate_el0,r.cpu,r.physical,r.bytes,4096,&wb);
}
/* Tensor links the application chose at open, after the scheduler started (ss_npu_queue_open_linked). The table is copied
 * once from the queue, which the application can write; every edge is checked against the loaded graphs and against the
 * edges already made (the image's constant table), at most as many as the session declared, and linked while the runtime
 * is idle, before the handles are built and the direct queue is claimed. */
static int link_requested(struct npu_runtime *rt)
{
    const size_t asked=ring.requested_links;
    if(!asked)return 1;
    if(asked>os_npu_links()||asked>copy_capacity||asked>edge_capacity-edge_count)return 0;
    memcpy(requested_copy,ring.requested,asked*sizeof(*requested_copy));
    for(size_t i=0;i<asked;++i){
        const struct ss_npu_queue_edge *e=&requested_copy[i];
        if(e->producer>=rt->slots||e->consumer>=rt->slots||e->producer==e->consumer||
           e->output>=rt->slot[e->producer].layout.outputs||e->input>=rt->slot[e->consumer].layout.inputs)return 0;
        for(size_t j=0;j<edge_count+i;++j){
            const struct ss_npu_queue_edge *o=j<edge_count?&edges[j]:&requested_copy[j-edge_count];
            if(o->consumer==e->consumer&&o->input==e->input)return 0;
        }
    }
    for(size_t i=0;i<asked;++i){
        const struct ss_npu_queue_edge *e=&requested_copy[i];
        if(!npu_runtime_link_tensor_running(rt,e->producer,e->output,e->consumer,e->input))return 0;
        edges[edge_count++]=*e;
    }
    return 1;
}
/* Each refusal of the open names itself: the failure record carries the statement's line and a detail unique to it. */
#define QFAIL(detail) (board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),(detail),SUNSTONE_CRUMB_HERE()),-1)
static int open_queue(void *unused,unsigned depth)
{
    (void)unused;
    /* The ring was admitted at the declared depth; a deeper one is refused, never silently shortened. */
    if(depth>os_npu_in_flight())return QFAIL(0x701);
    if(opened||os_npu_stopping()||!os_npu_queue_begin())return QFAIL(0x702);
    struct npu_runtime *rt=os_npu_queue_runtime();
    /* Slot i holds graph i of the image catalogue: load them now (the first open), then make the image's own links. */
    if(rt&&edges_ready&&!edges_prepared){
        for(unsigned i=0;i<rt->slots;++i){const int r=npu_runtime_load_resident(rt,i,i);if(r<0)return QFAIL(0x703);if(!r){os_npu_queue_end();return 0;}}
        if(!os_npu_queue_prepare_edges(rt))return QFAIL(0x704);
    }
    if(!rt||!edges_prepared||
       (cache_stride=aarch64_device_cache_stride())!=64)return QFAIL(0x705);
    if(!link_requested(rt))return QFAIL(0x706);
    for(unsigned i=0;i<rt->slots;++i){
        struct npu_runtime_slot *s=&rt->slot[i];
        /* The tensors of the handle, staged in the runtime's scratch (as many as the slot's graph has). */
        struct ss_npu_tensor *in=rt->scratch,*out=in+s->layout.inputs;
        if((size_t)(s->layout.inputs+s->layout.outputs)*sizeof(*in)>rt->scratch_bytes)return QFAIL(0x707);
        /* The slot's read and write regions must be reachable by the application at EL0, in the application's own tables
         * (the detail carries the slot). */
        if(!mapped(s->read))return QFAIL(0x70c|(i<<16));
        if(!mapped(s->write))return QFAIL(0x70d|(i<<16));
        for(unsigned j=0;j<s->layout.inputs;++j)in[j]=(struct ss_npu_tensor){s->tensors[j].data,s->tensors[j].bytes};
        for(unsigned j=0;j<s->layout.outputs;++j)out[j]=(struct ss_npu_tensor){s->tensors[s->layout.inputs+j].data,s->tensors[s->layout.inputs+j].bytes};
        if(!ss_npu_queue_service_handle(&ring,in,s->layout.inputs,out,s->layout.outputs,
            (struct ss_npu_cache_span){s->read.cpu,s->read.bytes},
            (struct ss_npu_cache_span){s->write.cpu,s->write.bytes}))return QFAIL(0x708);
    }
    if(!ss_npu_queue_service_edges(&ring,edges,edge_count))return QFAIL(0x709);
    if(npu_runtime_queue_claim(&direct,rt)!=1)return QFAIL(0x70a);
    for(unsigned i=0;i<rt->slots;++i)
        if(!npu_queue_register(&direct.queue,&commands[i],&rt->commands[i+1]))return QFAIL(0x70b);
    /* Only the pinned producer on this core performs CIVAC. The same WB
     * identity view is used at EL0 and EL1; no NC alias is introduced. */
    CPU_READ_SCTLR(original_sctlr);
    uint64_t enabled=original_sctlr|(UINT64_C(1)<<26);
    CPU_WRITE_SCTLR(enabled);
    ring.job_limit=UINT64_MAX; /* no lifetime count of jobs; the 64-bit sequence is the only bound */
    opened=1;return 1;
}
static int close_queue(void *unused)
{
    (void)unused;if(!opened||os_npu_progress()!=1)return -1;
    int r=npu_runtime_queue_return(&direct);if(r!=1)return r;
    CPU_WRITE_SCTLR(original_sctlr);
    os_npu_queue_end();
    if(os_npu_close()!=1)return -1;
    closed=1;return 1;
}
static int start(void *unused,unsigned handle,uint64_t sequence,uint64_t after,uint64_t timeout)
{
    (void)unused;(void)sequence;
    const struct os_session *s=os_session();
    if(!opened||closed||os_npu_stopping()||!s||handle>ring.handles||!handle||handle>direct.runtime->slots)return -1;
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
        CPU_CLEAN_INVALIDATE(address+offset);
    CPU_DSB();
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
    (void)unused;
#ifdef __aarch64__
    register long result __asm__("x0");
    __asm__ volatile("svc #120":"=r"(result)::"memory");return (int)result;
#else
    return ss_npu_queue_svc(); /* the host has no supervisor call: the same entry runs in place */
#endif
}
static void progress(void *unused,BaseType_t *woken)
{
    (void)unused;irq_woken=woken;
    if(atomic_load_explicit(&direct.runtime->service.signals->runtime_failed,memory_order_acquire))ss_npu_queue_service_fail(&ring,1);
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
