/* Actual deferred UART ISR body; GIC writes and RTOS calls modeled.
 * No CPU startup, exception delivery or UART hardware execution. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SUNSTONE_UART_PRIVATE_H
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef long BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
static unsigned critical, masks, notified, yielded;
static int mask_result=1, live=1;
static TaskHandle_t woken_task;
static void enter(void){assert(!critical);critical=1;}
static void leave(void){assert(critical);critical=0;}
#define taskENTER_CRITICAL() enter()
#define taskEXIT_CRITICAL() leave()
#define taskENTER_CRITICAL_FROM_ISR() (enter(),0u)
#define taskEXIT_CRITICAL_FROM_ISR(saved) do{assert((saved)==0);leave();}while(0)
static void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *woken)
{assert(critical&&task&&woken);woken_task=task;++notified;*woken=pdTRUE;}
#define portYIELD_FROM_ISR(woken) do{assert(!critical);yielded+=(woken)!=pdFALSE;}while(0)
#include "gicv3_spi.h"
int gicv3_spi_mask(const struct gicv3_spi *spi)
{assert(critical&&spi->intid==36);++masks;return mask_result;}
int gicv3_spi_rearm(const struct gicv3_spi *spi)
{assert(critical&&spi->intid==36);return 1;}
static struct {
 TaskHandle_t handle;struct gicv3_spi irq;
 atomic_uint pending,irq_fault,notifications;unsigned masked,armed;
} uart;
static int uart_live(void){return live;}
#include "../apps/uart/irq.c"
static void fresh(void)
{
 memset(&uart,0,sizeof(uart));critical=masks=notified=yielded=0;mask_result=live=1;woken_task=0;
 uart.irq=(struct gicv3_spi){0x03400000,36};uart.handle=(TaskHandle_t)(uintptr_t)0x1234;uart.armed=1;
}
int main(void)
{
 fresh();uart_application_irq();
 assert(masks==1&&notified==1&&yielded==1&&woken_task==uart.handle&&uart.masked);
 assert(atomic_load(&uart.pending)==1&&atomic_load(&uart.notifications)==1&&!atomic_load(&uart.irq_fault));
 /* A faulted delivery still wakes the owner so it observes the fault promptly. */
 fresh();uart.armed=0;uart_application_irq();
 assert(notified==1&&atomic_load(&uart.irq_fault)==1&&atomic_load(&uart.pending)==1);
 fresh();uart.handle=0;uart_application_irq();assert(!notified&&!yielded&&atomic_load(&uart.pending)==1);
 puts("PASS deferred UART ISR masks, publishes pending and notifies the owner task");
 return 0;
}
