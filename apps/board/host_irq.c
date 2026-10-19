#include "application.h"
#include "host_irq.h"
#include "gicv3_spi.h"
/* The EL3 route installer establishes Group1NS, level-high, CPU0 and0xb0.
 * All task/ISR binding fields share the existing kernel critical section. */
static const struct gicv3_spi source={0x03400000,187};
static struct {unsigned bound,armed,masked,stopped,fault,draining,burst,deferred;atomic_uint pending,notifications;} irq;
_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=0xb0,
    "USB2 priority must permit FreeRTOS FromISR calls");
static uint32_t read32(unsigned off)
{return *(volatile const uint32_t *)(uintptr_t)(0x03400000u+off);}
static int quiescent_mask(void)
{
    if(!irq.masked){irq.masked=1;if(!gicv3_spi_mask(&source))return -1;}
    if(read32(0)&(1u<<31))return 0;
    if(read32(0x114)&(1u<<27))return -1;
    return read32(0x314)&(1u<<27)?0:1;
}
int board_host_irq_bind(void)
{
    if(irq.bound||!board.handles[4]||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
       (read32(0)&(1u<<31))||((read32(0x114)|read32(0x214)|read32(0x314))&(1u<<27))||
       ((read32(0xc2c)>>22)&3)||(read32(0x4b8)>>24)!=0xb0)return 0;
    irq.bound=irq.masked=1;return 1;
}
void board_host_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    irq.masked=1;
    unsigned count=atomic_load_explicit(&irq.notifications,memory_order_relaxed);
    if(!gicv3_spi_mask(&source)||!irq.bound||!irq.armed||irq.stopped||!board_live(NULL)||count==UINT32_MAX)irq.fault=1;
    else atomic_store_explicit(&irq.notifications,count+1,memory_order_release);
    atomic_store_explicit(&irq.pending,1,memory_order_release);
    BaseType_t woken=pdFALSE;
    if(irq.bound)vTaskNotifyGiveFromISR(board.handles[4],&woken);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    portYIELD_FROM_ISR(woken);
}
int board_host_irq_before(void)
{
    int r=1,claim=0;taskENTER_CRITICAL();
    if(!irq.bound||irq.stopped||irq.fault||!board_live(NULL))r=-1;
    else if(irq.draining||atomic_load_explicit(&irq.pending,memory_order_acquire))r=quiescent_mask();
    if(r==0){if(++irq.deferred>32)r=-1;}else irq.deferred=0;
    if(r<0)irq.fault=1;
    if(r==1&&!irq.draining&&atomic_load_explicit(&irq.pending,memory_order_acquire)){
        atomic_store_explicit(&irq.pending,0,memory_order_release);claim=1;
    }
    taskEXIT_CRITICAL();
    if(r<=0)return r;
    if(claim){
        if(!board.host.core_started||a733_xhci_interrupt_begin(&board.host.core)!=1)return -1;
        irq.draining=1;
    }
    return 1;
}
int board_host_irq_after(void)
{
    if(!board.host.core_started)return 1;
    int r=a733_xhci_interrupt_start(&board.host.core);if(r<0)return -1;if(!r)return 1;
    taskENTER_CRITICAL();irq.armed=1;taskEXIT_CRITICAL();
    if(board.host.core.irq_service)irq.draining=1;
    if(irq.draining){r=a733_xhci_interrupt_finish(&board.host.core);if(r<0)return -1;if(!r)return 1;irq.draining=0;}
    taskENTER_CRITICAL();
    if(irq.fault||irq.stopped||!board_live(NULL))r=-1;
    else if(atomic_load_explicit(&irq.pending,memory_order_acquire))r=0;
    else if(!irq.masked)r=1;
    else if(read32(0x314)&(1u<<27))r=0;
    else {r=gicv3_spi_rearm(&source);if(r==1)irq.masked=0;}
    if(r==0){if(++irq.deferred>32)r=-1;}else irq.deferred=0;
    if(r<0)irq.fault=1;
    taskEXIT_CRITICAL();return r<0?-1:1;
}
void board_host_irq_wait(void)
{
    if(irq.draining&&++irq.burst<32)return;
    irq.burst=0;(void)ulTaskNotifyTake(pdTRUE,1);
}
void board_host_irq_stop(void)
{
    taskENTER_CRITICAL();irq.stopped=1;irq.armed=0;irq.masked=1;
    if(!gicv3_spi_mask(&source))irq.fault=1;
    taskEXIT_CRITICAL();
    /* No pending clear, source reset, DMA release or drain claim on failure. */
}
