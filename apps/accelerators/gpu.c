#include "application.h"
#include "session.h"
#include "boot.h"
#include "result.h"
#include "platform.h"
#include <string.h>
struct accelerator_gpu accelerator_gpu;
extern const unsigned char gpu_firmware[],gpu_firmware_end[];
#define g accelerator_gpu
int accelerator_gpu_live(void)
{return accelerator_shared_live() && !atomic_load_explicit(&g.failed,memory_order_acquire);}
void accelerator_gpu_fail(void)
{
    atomic_store_explicit(&g.failed,1,memory_order_release);
    atomic_store_explicit(&g.result,2,memory_order_release);
}
static _Noreturn void retained(void)
{accelerator_gpu_fail();for(;;)vTaskSuspend(NULL);}
static uint64_t budget(uint64_t requested)
{
    uint64_t now=a733_smp_counter(),end=a733_observe_identity.deadline;
    if(!accelerator_gpu_live() || now>=end)return 0;
    uint64_t us=(end-now)/24;
    return us<requested?us:requested;
}
static struct pvr_gpu_service_reply request(enum pvr_gpu_service_operation op,uint64_t timeout)
{
    struct pvr_gpu_service_reply r;uint64_t ticket;
    if(timeout && !(timeout=budget(timeout)))retained();
    struct pvr_gpu_service_request q={op,timeout};
    if(!accelerator_gpu_live() || !pvr_gpu_service_send(&g.service,&q,1,&ticket))retained();
    while(!pvr_gpu_service_collect(&g.service,ticket,&r,1))if(!accelerator_gpu_live())retained();
    if(r.result!=PVR_GPU_SERVICE_OK)retained();
    return r;
}
static void copy_output(uint32_t sequence)
{
    unsigned total=accelerator_gpu_package()->output_bytes/4;
    for(unsigned first=0;first<total;first+=32){
        struct pvr_gpu_service_reply r;uint64_t ticket;
        if(!accelerator_gpu_live() || !pvr_gpu_service_compute_read_sequence(&g.service,sequence,first,1,&ticket))retained();
        while(!pvr_gpu_service_collect(&g.service,ticket,&r,1))if(!accelerator_gpu_live())retained();
        unsigned n=total-first<32?total-first:32;
        if(r.result!=PVR_GPU_SERVICE_OK || r.compute_sequence!=sequence || r.compute_first_word!=first ||
           r.compute_word_count!=n || r.compute_total_words!=total)retained();
        memcpy(g.output+first,r.compute_words,n*4);
    }
}
static void client(void *argument)
{
    if(argument!=&g)retained();
    /* The transport first delivers a complete pending GPU response. NPU remains
     * available while GPU power-up and the sealed initial input run proceed. */
    while(!atomic_load_explicit(&g.transport_ready,memory_order_acquire)){
        if(!accelerator_gpu_live())retained();vTaskDelay(1);
    }
    struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,1000000);
    while(r.phase!=PVR_GPU_SERVICE_READY){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
    r=request(PVR_GPU_SERVICE_COMPUTE,1000000);
    while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
    if(r.compute_sequence!=1)retained();copy_output(1);
    atomic_store_explicit(&g.sequence,1,memory_order_release);
    atomic_store_explicit(&g.result,1,memory_order_release);
    for(;;){
        while(atomic_load_explicit(&g.mailbox,memory_order_acquire)!=1){
            if(!accelerator_gpu_live())retained();
            (void)request(PVR_GPU_SERVICE_STATUS,0);vTaskDelay(1);
        }
        if(g.operation==3)break;
        uint64_t ticket,timeout=budget(1000000);
        if(!timeout)retained();
        if(pvr_gpu_service_compute_input(&g.service,g.expected,g.input,g.bytes,timeout,1,&ticket)){
            while(!pvr_gpu_service_collect(&g.service,ticket,&r,1))if(!accelerator_gpu_live())retained();
            if(r.result==PVR_GPU_SERVICE_FAULT)retained();
            if(r.result==PVR_GPU_SERVICE_OK){
                uint32_t sequence=r.compute_sequence;
                if(sequence!=g.expected+1)retained();
                atomic_store_explicit(&g.sequence,sequence,memory_order_release);
                while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
                copy_output(sequence);
            }
        }
        /* Rejected service admission leaves the previous sequence/output live. */
        atomic_store_explicit(&g.mailbox,0,memory_order_release);
        atomic_store_explicit(&g.result,1,memory_order_release);
    }
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);
    while(r.phase!=PVR_GPU_SERVICE_STOPPED){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
    atomic_store_explicit(&g.closed,1,memory_order_release);
    atomic_store_explicit(&g.mailbox,0,memory_order_release);
    atomic_store_explicit(&g.result,1,memory_order_release);
    /* Shared PMIC/USB and every reserved parent remain owned. */
    for(;;)vTaskSuspend(NULL);
}
int accelerator_gpu_prepare(void)
{
    struct accelerator_providers p;struct a733_pmic_lifetime lifetime;
    if(g.client || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
       !accelerator_gpu_package() || !accelerator_providers_get(&p))return 0;
    g.client=xTaskCreateStaticAffinitySet(client,"gpu-compute",4096,&g,2,g.stack,&g.task,1);
    static const struct a733_gpu_backend_external external={.revoked=&g.failed};
    if(!g.client || !a733_gpu_platform_native_init(&g.native,&p.locks->callbacks) ||
       !a733_gpu_platform_init(&g.power,p.gpu,ACCELERATOR_GPU_CLIENT,&g.native.operations,&g.native) ||
       a733_pmic_identity(p.pmic,&lifetime)!=A733_PMIC_READY ||
       !a733_gpu_platform_bind_pmic(&g.power,p.gpu_native,p.pmic,&lifetime,ACCELERATOR_GPU_FIRMWARE_OWNER) ||
       !a733_gpu_reserved_prepare(&g.firmware,ACCELERATOR_GPU_FIRMWARE_OWNER,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000) ||
       !a733_gpu_compute_reserved_prepare_package(&g.compute,&g.firmware,&g.service,ACCELERATOR_GPU_COMPUTE_OWNER,accelerator_gpu_package()) ||
       !a733_gpu_backend_bind(&g.backend,&g.service,&g.firmware.config,&g.firmware.memory,&g.power,&external) ||
       !pvr_gpu_service_init(&g.service,&g.firmware.config,3,1) ||
       !pvr_gpu_service_bind(&g.service,0,g.client)){
        accelerator_gpu_fail();if(g.service.worker)vTaskSuspend(g.service.worker);return 0;
    }
    return 1;
}
BaseType_t accelerator_gpu_task_privileged(TaskFunction_t entry,void *argument)
{return (entry==client && argument==&g) || pvr_gpu_service_task_privileged(entry,argument)?pdTRUE:pdFALSE;}
void accelerator_gpu_irq(void)
{
    BaseType_t woken=pdFALSE;
    if(!atomic_load_explicit(&g.failed,memory_order_acquire) &&
       !pvr_gpu_service_interrupt(&g.service,&woken))accelerator_gpu_fail();
    portYIELD_FROM_ISR(woken);
}
