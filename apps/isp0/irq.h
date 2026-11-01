#ifndef SUNSTONE_ISP0_PRIVATE_IRQ_H
#define SUNSTONE_ISP0_PRIVATE_IRQ_H
/* Included by the privileged owner after its checked MMIO functions. */
#if defined(SUNSTONE_ISP0_IRQ)
#include "gicv3_spi.h"
#include "irq_table.h"
static const struct gicv3_spi isp0_sources[]={
 {0x03400000,149},{0x03400000,153},{0x03400000,161}
};
static atomic_uint irq_fault;
static unsigned irq_bound,irq_index,irq_phase;
static void isp0_interrupt(unsigned index)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 if(!gicv3_spi_mask(&isp0_sources[index])||!irq_bound||!isp0.handle||!board_live(NULL))
  atomic_store_explicit(&irq_fault,1,memory_order_release);
 else atomic_fetch_add_explicit(&isp0.interrupts[index],1,memory_order_relaxed);
 BaseType_t woken=pdFALSE;
 if(irq_bound)vTaskNotifyGiveFromISR(isp0.handle,&woken);
 taskEXIT_CRITICAL_FROM_ISR(saved);portYIELD_FROM_ISR(woken);
}
static void isp0_dma_interrupt(void){isp0_interrupt(0);}
static void isp0_vipp_interrupt(void){isp0_interrupt(1);}
static void isp0_save_interrupt(void){isp0_interrupt(2);}
const struct a733_irq_route a733_irq_routes[]={
 {149,0,0xd8,isp0_dma_interrupt},{153,0,0xd8,isp0_vipp_interrupt},{161,0,0xd8,isp0_save_interrupt}
};
const unsigned a733_irq_route_count=3;
static int isp0_irq_arm(void)
{
 /* Preserve raw completion flags. Each source wakes once per capture; ISP
  * SAVE (bit2) follows LOAD (bit3), so LOAD cannot mask the later SAVE wake.
  * Device backing, routes and source enables remain retained after success. */
 static const uintptr_t enables[]={0x05830250,0x05910020,0x05900028};
 static const uint32_t masks[]={0x2fe045,0x100,0x7f0e04};
 if(!owner()||atomic_load_explicit(&irq_fault,memory_order_acquire))return -1;
 int result=0;taskENTER_CRITICAL();
 irq_bound=1;
 const struct gicv3_spi *source=&isp0_sources[irq_index];
 uint32_t bit=1u<<(source->intid%32),word=4u*(source->intid/32),v;
 if(!irq_phase){
  if(!gicv3_spi_mask(source))result=-1;else irq_phase=1;
 }else if(irq_phase==1){
  if(!load(0x03400000,&v))result=-1;
  else if(!(v&(1u<<31))){
   if(!load(0x03400300+word,&v))result=-1;
   else if(!(v&bit))irq_phase=2;
  }
 }else if(irq_phase==2){
  /* Old latched status has been cleared by retained rearm. Clear only this
   * owned, masked GIC pending bit before enabling the next capture. A level
   * already asserted by a new VIPP load remains eligible for delivery. */
  if(!store(0x03400280+word,bit)||!store(enables[irq_index],masks[irq_index])||
     !load(enables[irq_index],&v)||v!=masks[irq_index])result=-1;
  else irq_phase=3;
 }else{
  result=gicv3_spi_rearm(source);
  if(result==1){
   irq_phase=0;
   if(++irq_index==3)irq_index=0;else result=0;
  }
 }
 taskEXIT_CRITICAL();return result;
}
static int isp0_irq_healthy(void)
{return !atomic_load_explicit(&irq_fault,memory_order_acquire);}
static void isp0_wait(void)
{
 if(!isp0.waiting){vTaskDelay(1);return;}
 uint64_t t;
 if(now(&isp0,&t)!=1||t>=deadline())return;
 uint64_t remaining=deadline()-t;
 TickType_t ticks=(TickType_t)((remaining+999)/1000);
 if(!ticks)ticks=1;
 (void)ulTaskNotifyTake(pdTRUE,ticks);
}
#else
static int isp0_irq_arm(void){return 1;}
static int isp0_irq_healthy(void){return 1;}
static void isp0_wait(void){vTaskDelay(1);}
#endif
#endif
