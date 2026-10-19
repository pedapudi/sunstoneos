#include "uart.h"
/* The dispatcher owns acknowledge/EOI/deactivation. ISR metadata and source
 * mask/rearm share the existing kernel critical section; UART access is task-only. */
void uart_application_irq(void)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 uart.masked=1;
 unsigned count=atomic_load_explicit(&uart.notifications,memory_order_relaxed);
 if(!gicv3_spi_mask(&uart.irq)||!uart.armed||!uart_live()||count>=128)
  atomic_store_explicit(&uart.irq_fault,1,memory_order_release);
 else atomic_store_explicit(&uart.notifications,count+1,memory_order_release);
 atomic_store_explicit(&uart.pending,1,memory_order_release);
 taskEXIT_CRITICAL_FROM_ISR(saved);
}
int uart_irq_rearm(void)
{
 int r=0;taskENTER_CRITICAL();
 if(uart.armed&&uart_live()&&!atomic_load_explicit(&uart.irq_fault,memory_order_acquire)){
  /* A valid IRQ can publish after the task checked pending. Keep its source
   * masked and defer service, without turning that notification into failure. */
  if(atomic_load_explicit(&uart.pending,memory_order_acquire))r=0;
  else if(!uart.masked)r=1;
  else {r=gicv3_spi_rearm(&uart.irq);if(r==1)uart.masked=0;}
 }else r=-1;
 taskEXIT_CRITICAL();return r;
}
int uart_irq_stop(void)
{
 int ok=1;taskENTER_CRITICAL();uart.armed=0;
 if(!uart.masked){uart.masked=1;ok=gicv3_spi_mask(&uart.irq);}
 taskEXIT_CRITICAL();return ok;
}
