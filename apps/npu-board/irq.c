/* SPI 65 (INTID 97, level high) on core 1, where the NPU worker runs. The
 * EL3 boot programs this table entry (boot.patch) and the shared handler
 * dispatches through it; the source stays disabled until the service rearms
 * it. The mask, rearm and stop callbacks follow apps/npu-interrupt/irq.c and
 * refuse any core but core 1. */
#include "irq_table.h"
#include "npu-board.h"
#include "gicv3_spi.h"
#include "a733_mmio.h"
#define GICD UINT32_C(0x03400000)
#define BIT (UINT32_C(1)<<1)
#define NPU_BOARD_PRIORITY 0xd8u
void npu_board_irq(void);
const struct a733_irq_route a733_irq_routes[]={{97,NPU_BOARD_CORE,NPU_BOARD_PRIORITY,npu_board_irq}};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
_Static_assert(NPU_BOARD_PRIORITY>=A733_IRQ_TABLE_MIN_PRIORITY,"NPU priority must permit FreeRTOS FromISR calls");
static const struct gicv3_spi source={GICD,97};
static struct {
    struct npu_service *service;
    unsigned masked,stopped,fault;
} binding;
static uint32_t read32(uint32_t offset){return *(volatile uint32_t *)(uintptr_t)(GICD+offset);}
static int on_core(void)
{
    uint64_t el,mpidr;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1":"=r"(el),"=r"(mpidr));
    return el==4 && (mpidr&UINT64_C(0xff00ffffff))==(uint64_t)NPU_BOARD_CORE<<8;
}
/* Binding state is protected by the kernel critical section; the callers are
 * the worker and its ISR, both on core 1. */
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
static int rearm(void *cookie)
{
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
            if(!binding.stopped)a733_mmio_write32_dsb_sy_after(GICD+0x28c,BIT);
            if((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT){binding.fault=1;result=-1;}
            else binding.stopped=1;
        }
    }
    taskEXIT_CRITICAL();return result;
}
/* Before the scheduler, on the boot core: the source disabled, inactive and
 * not pending, level-sensitive, at the table's priority and routed to core 1.
 * The route is checked once here; the callbacks check only the calling core. */
int npu_board_irq_bind(struct npu_runtime *rt)
{
    if(!rt || binding.service || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || binding.fault ||
       (read32(0)&(1u<<31)) || ((read32(0x10c)|read32(0x20c)|read32(0x30c))&BIT) ||
       (read32(0xc18)&(3u<<2)) || (read32(0x460)&0xff00u)!=NPU_BOARD_PRIORITY<<8 ||
       *(volatile uint64_t *)(uintptr_t)(GICD+0x6308)!=(uint64_t)NPU_BOARD_CORE<<8)return 0;
    static const struct npu_service_irq_ops ops={mask,rearm,stop};
    binding.masked=1;
    /* The idle-register hint stays until the board shows one INTID 97 per job. */
    if(!npu_runtime_bind_irq(rt,&ops,&binding,UINT32_C(1)<<NPU_BOARD_CORE,1))return 0;
    binding.service=&rt->service;return 1;
}
void npu_board_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    int ok=gicv3_spi_mask(&source);
    binding.masked=1;
    if(!ok || !binding.service || binding.stopped)binding.fault=1;
    BaseType_t woken=pdFALSE;
    if(binding.service)npu_service_irq_notify_from_isr(binding.service,!binding.fault,&woken);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    configASSERT(binding.service);
    portYIELD_FROM_ISR(woken);
}
