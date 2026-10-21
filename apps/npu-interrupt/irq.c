#include "irq.h"
#include "npu_service.h"
#include "gicv3_spi.h"

#define GICD UINT32_C(0x03400000)
#define BIT (UINT32_C(1)<<1)
static const struct gicv3_spi source={GICD,97};
static struct {
    struct npu_service *service;
    unsigned masked,stopped,fault;
} binding;

_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=0xb0,
    "NPU priority must permit FreeRTOS FromISR calls");
static uint32_t read32(uint32_t offset)
{ return *(volatile uint32_t *)(uintptr_t)(GICD+offset); }
static int boot_el1(void)
{
    uint64_t el,mpidr;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1" : "=r"(el),"=r"(mpidr));
    return el==4 && !(mpidr&UINT64_C(0xff00ffffff));
}
/* All mutable binding state is protected by the kernel critical section.
 * The sole caller is the owner task or its ISR on the admitted boot CPU. */
static int mask_locked(void)
{
    if(!binding.masked) {
        binding.masked=1;
        if(!gicv3_spi_mask(&source))binding.fault=1;
    }
    if(binding.fault)return -1;
    if(read32(0)&(1u<<31))return 0;
    if(read32(0x10c)&BIT){binding.fault=1;return -1;}
    if(read32(0x30c)&BIT)return 0;
    return 1;
}
static int mask(void *cookie)
{
    if(cookie!=&binding || !boot_el1())return -1;
    taskENTER_CRITICAL();int result=mask_locked();taskEXIT_CRITICAL();
    return result;
}
static int rearm(void *cookie)
{
    /* The service holds its critical section across pending-event inspection
     * and this callback. Do not clear pending: a level event may already exist. */
    if(cookie!=&binding || !boot_el1() || binding.fault || binding.stopped)return -1;
    if(!binding.masked)return 1;
    if(read32(0x30c)&BIT)return 0;
    int result=gicv3_spi_rearm(&source);
    if(result==1)binding.masked=0;
    if(result<0)binding.fault=1;
    return result;
}
static int stop(void *cookie)
{
    if(cookie!=&binding || !boot_el1())return -1;
    taskENTER_CRITICAL();
    int result=-1;
    if(binding.service && binding.service->controller.state==A733_NPU_CLOSED) {
        result=mask_locked();
        if(result==1) {
            /* CLOSE has disabled the device source and proved full idle.
             * Clear only this lifetime's pending SPI, after deactivation. */
            if(!binding.stopped) {
                *(volatile uint32_t *)(uintptr_t)(GICD+0x28c)=BIT;
                __asm__ volatile("dsb sy" ::: "memory");
            }
            if((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT) {
                binding.fault=1;result=-1;
            } else binding.stopped=1;
        }
    }
    taskEXIT_CRITICAL();return result;
}
int npu_application_irq_bind(struct npu_service *service)
{
    if(!service || binding.service || !boot_el1() ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || binding.fault ||
       (read32(0)&(1u<<31)) ||
       ((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT) ||
       (read32(0xc18)&(3u<<2)) ||
       (read32(0x460)&0xff00u)!=0xb000u ||
       *(volatile uint64_t *)(uintptr_t)(GICD+0x6308)!=0)return 0;
    /* IGROUPR is RAZ/WI in Nonsecure state with DS=0. The EL3 installer owns
     * that validation; the priority and route readbacks here require access
     * to the already-configured Nonsecure interrupt. */
    static const struct npu_service_irq_ops ops={mask,rearm,stop};
    binding.masked=1;
    /* The EL3 installer routes INTID97 to CPU0 and every callback refuses off
     * CPU0, so the service worker must be pinned to CPU0 alone. */
    if(!npu_service_bind_irq(service,&ops,&binding,1))return 0;
    binding.service=service;return 1;
}
void npu_application_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    int ok=gicv3_spi_mask(&source);
    binding.masked=1;
    if(!ok || !binding.service || binding.stopped)binding.fault=1;
    BaseType_t woken=pdFALSE;
    if(binding.service)
        npu_service_irq_notify_from_isr(binding.service,!binding.fault,&woken);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    configASSERT(binding.service); /* Unexpected pre-binding delivery is retained. */
    portYIELD_FROM_ISR(woken);
}
