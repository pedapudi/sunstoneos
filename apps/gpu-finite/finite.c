#include "finite.h"
#include "../gpu-render/application.h"
#include "platform.h"
#include "a733_pmic_native.h"
#include "aarch64_device_memory.h"

static atomic_uint pmic_stop,pmic_parked;
static _Noreturn void retained(unsigned reason)
{
    gpu_application_fail(NULL);
    platform_puts("INCOMPLETE GPU finite stop: ");platform_hex(reason);platform_puts("\n");
    /* Potentially active device memory must not take the successful return path. */
    vTaskSuspend(NULL);
    platform_panic("GPU finite retained",reason);
}
int gpu_finite_bind_pmic(void)
{
    struct a733_native_usb *n=&app.shared;
    if(!n->used||!app.handles[0]||!app.handles[2]||app.handles[1]||n->pmic_task||n->usb_task)return 0;
    /* Reuse the existing provider constructors without constructing USB0. */
    n->pmic_task=app.handles[0];
    return a733_pmic_native_init(&n->pmic_native,&n->locks,n->pmic_task)&&
        a733_pmic_init(&n->pmic,&n->boot.pmic,&n->pmic_native.operations,&n->pmic_native,
            &n->clocks,&n->rails,&n->pins)&&
        a733_gpu_resources_native_use_pmic(&n->gpu_native,&n->gpu,&n->pmic);
}
void gpu_finite_pmic_checkpoint(void)
{
    if(!atomic_load_explicit(&pmic_stop,memory_order_acquire))return;
    atomic_store_explicit(&pmic_parked,1,memory_order_release);
    for(;;)vTaskDelay(1);
}
static int irq_read(unsigned offset,uint32_t *out)
{
    struct aarch64_translation r;
    uintptr_t address=A733_GPU_GICD+offset;
    if(!aarch64_device_translate(address,0,&r)||r.physical!=address||r.attributes!=0)return 0;
    __asm__ volatile("dsb sy":::"memory");
    *out=*(volatile uint32_t *)address;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int irq_quiet(void)
{
    uint32_t control,enabled,pending,active;
    if(!irq_read(0,&control)||!irq_read(0x108,&enabled)||!irq_read(0x208,&pending)||
       !irq_read(0x308,&active))return -1;
    return !(control&UINT32_C(0x80000000))&&
        !((enabled|pending|active)&UINT32_C(0x80000000));
}
static int stopped(void)
{
    return gpu_application_live(NULL)&&app.service.phase==PVR_GPU_SERVICE_STOPPED&&
        app.service.owner==PVR_GPU_SERVICE_CLIENTS&&app.service.irq_masked&&!app.service.irq_fault&&
        app.service.stop.phase==PVR_GPU_STOP_STOPPED&&app.service.stop.isolation_observed&&
        app.backend.phase==A733_GPU_BACKEND_ISOLATED&&!atomic_load_explicit(&app.backend.error,memory_order_acquire)&&
        app.power.phase==A733_GPU_PLATFORM_OFF&&!app.power.token&&!app.power.delegated&&
        app.firmware.memory.phase==A733_GPU_MEMORY_OFFLINE&&!app.shared.usb_task&&
        !atomic_load_explicit(&app.shared.usb_prepared,memory_order_acquire)&&!app.shared.usb.initialized;
}
_Noreturn void gpu_finite_complete(void)
{
    if(!stopped()||!app.service.worker||app.service.worker==xTaskGetCurrentTaskHandle())retained(1);
    /* CLOSE retired device callbacks. Suspend the worker before its next poll. */
    vTaskSuspend(app.service.worker);
    atomic_store_explicit(&pmic_stop,1,memory_order_release);
    while(!atomic_load_explicit(&pmic_parked,memory_order_acquire)){
        if(!gpu_application_live(NULL))retained(2);vTaskDelay(1);
    }
    struct a733_pmic *p=&app.shared.pmic;
    if(p->held||p->retained||p->failed||p->active!=A733_PMIC_REQUESTS||
       atomic_load_explicit(&app.shared.locks.retained,memory_order_acquire))retained(3);
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        if(p->slots[i].state||p->slots[i].borrow.id)retained(4);
    if(xSemaphoreGetMutexHolder(app.shared.locks.mutex[0])||
       xSemaphoreGetMutexHolder(app.shared.locks.mutex[1]))retained(5);
    int quiet;
    do{quiet=irq_quiet();if(quiet<0||!gpu_application_live(NULL))retained(6);
        if(!quiet)vTaskDelay(1);
    }while(!quiet);
    platform_puts("GPU_STAGE=STOPPED\nGPU_COMPUTE_WORDS=\n");
    for(unsigned n=0;n<2;++n){
        platform_puts("GPU_SEQUENCE=");platform_hex(app.compute_sequence[n]);platform_puts("\n");
        for(unsigned i=0;i<PVR_COMPUTE_JOB_WORDS;++i){platform_hex(app.compute_words[n][i]);platform_puts("\n");}
    }
    if(!stopped()||!atomic_load_explicit(&pmic_parked,memory_order_acquire)||irq_quiet()!=1)retained(7);
    /* Stop scheduling on the client stack before SMC. The monitor owns final
     * cache maintenance, secure state, RAM restoration and CPU/ROM return. */
    taskENTER_CRITICAL();
    atomic_store_explicit(&app.result,1,memory_order_release);
    platform_puts("PASS GPU two copied results, CLOSE and parked providers\n");
    platform_exit(0);
}
