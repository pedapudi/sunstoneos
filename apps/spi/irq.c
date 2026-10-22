#include "spi.h"
#include "irq.h"
#include "gicv3_spi.h"
#include "a733_mmio.h"
static const struct gicv3_spi source={UINT32_C(0x03400000),59};
static struct {
 unsigned bound,armed,masked,service,stopped,fault,deferred;
 atomic_uint pending,notifications;
} irq;
_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=0xb0,
 "SPI priority must permit FreeRTOS FromISR calls");
static uint32_t read32(unsigned off)
{return a733_mmio_read32_dsb_sy_both(0x03400000u+off);}
static int quiescent(void)
{
 if(read32(0)&(1u<<31))return 0;
 if(read32(0x104)&(1u<<27))return -1;
 return read32(0x304)&(1u<<27)?0:1;
}
int spi_irq_bind(void)
{
 if(spi.session.version!=2||irq.bound||!spi.handle||
    xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
    (read32(0)&(1u<<31))||
    ((read32(0x104)|read32(0x204)|read32(0x304))&(1u<<27))||
    ((read32(0xc0c)>>22)&3)||(read32(0x438)>>24)!=0xb0)return 0;
 /* Group1NS and CPU0 routing were established by private EL3 startup.
  * Group registers cannot establish that fact from the Nonsecure view. */
 irq.bound=irq.masked=1;return 1;
}
void spi_interrupt(void)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 irq.masked=1;
 unsigned count=atomic_load_explicit(&irq.notifications,memory_order_relaxed);
 if(!gicv3_spi_mask(&source)||!irq.bound||!irq.armed||irq.stopped||
    !spi_live()||count>=32){
  irq.fault=1;atomic_store_explicit(&spi.failed,1,memory_order_release);
  atomic_store_explicit(&spi.result,2,memory_order_release);
 }else atomic_store_explicit(&irq.notifications,count+1,memory_order_release);
 atomic_store_explicit(&irq.pending,1,memory_order_release);
 BaseType_t woken=pdFALSE;
 if(irq.bound)vTaskNotifyGiveFromISR(spi.handle,&woken);
 taskEXIT_CRITICAL_FROM_ISR(saved);portYIELD_FROM_ISR(woken);
}
int spi_irq_arm(void)
{
 if(spi.native.operations.current(&spi.native)!=1)return -1;
 taskENTER_CRITICAL();int r;
 if(!irq.bound||irq.stopped||irq.fault||!spi_live()||!spi.platform.interrupt_armed)r=-1;
 else{
  irq.armed=1;r=gicv3_spi_rearm(&source);
  if(r==1)irq.masked=0;
 }
 if(r==0){if(++irq.deferred>32)r=-1;}else irq.deferred=0;
 if(r<0)irq.fault=1;
 taskEXIT_CRITICAL();return r;
}
int spi_irq_claim(void)
{
 if(spi.native.operations.current(&spi.native)!=1)return -1;
 taskENTER_CRITICAL();int r=0;
 if(!irq.bound||irq.stopped||irq.fault||!spi_live())r=-1;
 else if(irq.service)r=1;
 else if(!irq.armed)r=-1;
 else if(atomic_load_explicit(&irq.pending,memory_order_acquire)){
  r=quiescent();
  if(r==1){
   atomic_store_explicit(&irq.pending,0,memory_order_release);
   irq.armed=0;irq.service=1;
  }
 }
 if(r==0&&atomic_load_explicit(&irq.pending,memory_order_acquire)){
  if(++irq.deferred>32)r=-1;
 }else irq.deferred=0;
 if(r<0)irq.fault=1;
 taskEXIT_CRITICAL();return r;
}
void spi_irq_stop(void)
{
 taskENTER_CRITICAL();
 if(irq.bound){
  irq.stopped=1;irq.armed=0;irq.masked=1;
  if(!gicv3_spi_mask(&source))irq.fault=1;
 }
 taskEXIT_CRITICAL();
 /* Masking establishes no drain, peer reset or permission to reuse buffers. */
}
void spi_irq_wait(void){(void)ulTaskNotifyTake(pdTRUE,1);}
unsigned spi_irq_notifications(void)
{
 taskENTER_CRITICAL();
 unsigned count=irq.bound&&irq.service&&!irq.fault&&!irq.stopped?
  atomic_load_explicit(&irq.notifications,memory_order_acquire):0;
 taskEXIT_CRITICAL();return count;
}
