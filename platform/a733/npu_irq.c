/* INTID 97 (SPI 65, level high) bound to the NPU service: mask, rearm, stop
 * and the ISR. The route is configuration. A single-core image compiles this
 * file as it is: core 0, priority 0xb0, the API in a733_npu_irq.h. A runtime
 * image includes it (apps/os/npu-irq.c, apps/npu-board/irq.c) after defining
 * NPU_IRQ_CORE, NPU_IRQ_PRIORITY, NPU_IRQ_HANDLER and NPU_IRQ_EXTERNAL_BIND,
 * the bind function that attaches the callbacks to an NPU runtime. */
#include "a733_npu_irq.h"
#include "npu_service.h"
#include "gicv3_spi.h"
#include "a733_mmio.h"
#include "sunstone_crumb.h"

#ifndef NPU_IRQ_CORE
#define NPU_IRQ_CORE 0u
#endif
#ifndef NPU_IRQ_PRIORITY
#define NPU_IRQ_PRIORITY 0xb0u
#endif
#ifndef NPU_IRQ_HANDLER
#define NPU_IRQ_HANDLER npu_application_irq
#endif
#ifndef NPU_IRQ_QUIET_QUERY
#define NPU_IRQ_QUIET_QUERY npu_application_irq_quiet
#endif
#define GICD UINT32_C(0x03400000)
#define BIT (UINT32_C(1)<<1)
static const struct gicv3_spi source={GICD,97};
static struct {
    struct npu_service *service;
    unsigned masked,stopped,fault;
} binding;

#ifndef NPU_IRQ_EXTERNAL_BIND
_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=NPU_IRQ_PRIORITY,
    "NPU priority must permit FreeRTOS FromISR calls");
#endif
static uint32_t read32(uint32_t offset)
{ return *(volatile uint32_t *)(uintptr_t)(GICD+offset); }
/* EL1 on the core that receives the interrupt. */
static int on_core(void)
{
    uint64_t el,mpidr;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1" : "=r"(el),"=r"(mpidr));
    return el==4 && (mpidr&UINT64_C(0xff00ffffff))==(uint64_t)NPU_IRQ_CORE<<8;
}
/* All mutable binding state is protected by the kernel critical section.
 * The callers are the service worker and its ISR, both on the routed core. */
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
    if(cookie!=&binding || !on_core())return -1;
    taskENTER_CRITICAL();int result=mask_locked();taskEXIT_CRITICAL();
    return result;
}
int NPU_IRQ_QUIET_QUERY(void *cookie)
{
    if(cookie!=&binding || !on_core() || !binding.service || binding.fault || binding.stopped)return -1;
    if(read32(0)&(1u<<31))return 0;
    return ((read32(0x20c)|read32(0x30c))&BIT) ? 0 : 1;
}
static int rearm(void *cookie)
{
    /* The service holds its critical section across pending-event inspection
     * and this callback. Do not clear pending: a level event may already exist. */
    if(cookie!=&binding || !on_core() || binding.fault || binding.stopped)return -1;
    if(!binding.masked)return 1;
    if(read32(0x30c)&BIT)return 0;
    int result=gicv3_spi_rearm(&source);
    if(result==1)binding.masked=0;
    if(result<0)binding.fault=1;
    return result;
}
static int stop(void *cookie)
{
    if(cookie!=&binding || !on_core())return -1;
    taskENTER_CRITICAL();
    int result=-1;
    if(binding.service && binding.service->controller.state==A733_NPU_CLOSED) {
        result=mask_locked();
        if(result==1) {
            /* CLOSE has disabled the device source and proved full idle.
             * Clear only this lifetime's pending SPI, after deactivation. */
            if(!binding.stopped) {
                a733_mmio_write32_dsb_sy_after(GICD+0x28c,BIT);
            }
            if((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT) {
                binding.fault=1;result=-1;
            } else binding.stopped=1;
        }
    }
    taskEXIT_CRITICAL();return result;
}
#ifndef NPU_IRQ_EXTERNAL_BIND
int npu_application_irq_restart(void *cookie)
{
    if(cookie!=&binding || !on_core() || !binding.service || !binding.stopped ||
       !binding.masked || binding.fault || (read32(0)&(1u<<31)) ||
       ((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT))return -1;
    binding.stopped=0;return 1;
}
#endif
/* Before the scheduler, at EL1: the source disabled, inactive and not
 * pending, level-sensitive, at NPU_IRQ_PRIORITY and routed to NPU_IRQ_CORE.
 * The route is checked once here; the callbacks check only the calling core.
 * IGROUPR is RAZ/WI in Nonsecure state with DS=0. The EL3 installer owns that
 * validation; the priority and route readbacks here require access to the
 * already-configured Nonsecure interrupt. */
/* Zero when the state is ready; otherwise the number of the first failed
 * clause, recorded as the failure value so a refused bind names its cause:
 * 1 not EL1 or already bound or faulted, 2 scheduler started, 3 distributor
 * write pending, 4 source enabled, 5 source pending (a level event from a
 * device left running), 6 source active, 7 not level-sensitive, 8 wrong
 * priority, 9 wrong route. */
static unsigned bind_refusal(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
    if(el!=4 || binding.service || binding.fault)return 1;
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 2;
    if(read32(0)&(1u<<31))return 3;
    if(read32(0x10c)&BIT)return 4;
    if(read32(0x20c)&BIT)return 5;
    if(read32(0x30c)&BIT)return 6;
    if(read32(0xc18)&(3u<<2))return 7;
    if((read32(0x460)&0xff00u)!=NPU_IRQ_PRIORITY<<8)return 8;
    if(*(volatile uint64_t *)(uintptr_t)(GICD+0x6308)!=(uint64_t)NPU_IRQ_CORE<<8)return 9;
    return 0;
}
static int bind_ready(void)
{
    const unsigned reason=bind_refusal();
    if(reason)sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),0xb00u|reason,0);
    return !reason;
}
static const struct npu_service_irq_ops ops={mask,rearm,stop};
#ifndef NPU_IRQ_EXTERNAL_BIND
int npu_application_irq_bind(struct npu_service *service)
{
    if(!service || !on_core() || !bind_ready())return 0;
    binding.masked=1;
    /* Every callback refuses off the routed core, so the service worker must
     * be pinned to that core alone. */
    if(!npu_service_bind_irq(service,&ops,&binding,UINT32_C(1)<<NPU_IRQ_CORE))return 0;
    binding.service=service;return 1;
}
#else
#include "npu_runtime.h"
/* Before the scheduler, on the boot core. The idle-register hint stays until
 * the board shows one INTID 97 per job. */
#ifndef NPU_IRQ_IDLE_HINT
#define NPU_IRQ_IDLE_HINT 1
#endif
int NPU_IRQ_EXTERNAL_BIND(struct npu_runtime *rt)
{
    if(!rt || !bind_ready())return 0;
    binding.masked=1;
    if(!npu_runtime_bind_irq(rt,&ops,&binding,UINT32_C(1)<<NPU_IRQ_CORE,NPU_IRQ_IDLE_HINT))return 0;
    binding.service=&rt->service;return 1;
}
#endif
#if !defined(NPU_IRQ_EXTERNAL_BIND) || defined(NPU_IRQ_CAPTURE_TICKS)
/* System counter at the latest delivery, before the mask; read by the finite
 * client after each completed job to time the job without the worker's wake. */
static uint64_t delivered_ticks;
#ifdef NPU_IRQ_CAPTURE_TICKS
static unsigned delivered_count;
#endif
uint64_t npu_application_irq_ticks(void)
{ return __atomic_load_n(&delivered_ticks,__ATOMIC_ACQUIRE); }
#ifdef NPU_IRQ_CAPTURE_TICKS
unsigned npu_application_irq_count(void)
{ return __atomic_load_n(&delivered_count,__ATOMIC_ACQUIRE); }
#endif
#endif
void NPU_IRQ_HANDLER(void)
{
    /* A claimed resident queue owns the destructive event read. Leave the
     * level SPI enabled: the queue acknowledges the device before EOI and
     * directly notifies its waiting client. Errors mask this same source. */
    if(binding.service && binding.service->direct) {
        BaseType_t woken=pdFALSE;
        binding.service->direct->irq(binding.service->direct->context,&woken);
        if(atomic_load_explicit(&binding.service->irq_pending,memory_order_acquire)) {
            if(!gicv3_spi_mask(&source))binding.fault=1;
            binding.masked=1;
        }
        portYIELD_FROM_ISR(woken);
        return;
    }
#if !defined(NPU_IRQ_EXTERNAL_BIND) || defined(NPU_IRQ_CAPTURE_TICKS)
    uint64_t now;__asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(now) :: "memory");
    __atomic_store_n(&delivered_ticks,now,__ATOMIC_RELEASE);
#ifdef NPU_IRQ_CAPTURE_TICKS
    __atomic_add_fetch(&delivered_count,1,__ATOMIC_RELEASE);
#endif
#endif
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
