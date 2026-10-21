/* Deferred IRQ application model. The handler, task and FIFO driver are actual
 * sources; dispatcher delivery, GIC and platform ownership are modeled here.
 * Real clock/pin/platform synchronization is checked in a733-uart-irq-platform. */
#define SUNSTONE_UART_PRIVATE_H
#define SUNSTONE_A733_SMP_BOOT_H
#include "a733_uart_platform.h"
#include "a733_storage_digest.h"
#include "gicv3_spi.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define UART_OUTPUT_BYTES 144u
typedef void *TaskHandle_t;
struct uart_application {
 struct {unsigned tx_bytes,rx_bytes;} session;
 struct a733_uart_platform platform;
 struct gicv3_spi irq;TaskHandle_t handle;
 atomic_uint result,failed,pending,irq_fault,notifications;
 unsigned masked,armed,tx_irqs,rx_irqs,other_irqs;
 unsigned stage;size_t sent,received;uint64_t deadline;
 unsigned payload_bytes;uint8_t tx[32],rx[32],status[32],payload[144],digest[32],session_digest[32];
};
static struct uart_application uart;
static uint64_t clock_us;
static unsigned drop,corrupt,line_error,write_uncertain,retain_calls,service_writes,closing,close_failed,close_wait;
static unsigned cause,enabled,active,sync_wait,critical,rearm_failed;
static uint8_t fifo[32];static unsigned head,tail,task_notifies;
typedef unsigned UBaseType_t;typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define taskENTER_CRITICAL() do {assert(!critical);critical=1;} while(0)
#define taskEXIT_CRITICAL() do {assert(critical);critical=0;} while(0)
static UBaseType_t isr_enter(void){taskENTER_CRITICAL();return 0;}
#define taskENTER_CRITICAL_FROM_ISR() isr_enter()
#define taskEXIT_CRITICAL_FROM_ISR(saved) do {(void)(saved);taskEXIT_CRITICAL();} while(0)
static uint64_t a733_smp_counter(void){return clock_us*24;}
static int uart_live(void){return !atomic_load(&uart.failed);}
static void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *woken)
{assert(critical&&t==uart.handle&&woken);++task_notifies;*woken=pdTRUE;}
static void portYIELD_FROM_ISR(BaseType_t woken){assert(!critical);(void)woken;}
static uint32_t ulTaskNotifyTake(BaseType_t clear,unsigned ticks){(void)clear;(void)ticks;return 0;}
int gicv3_spi_mask(const struct gicv3_spi *s)
{assert(critical&&s==&uart.irq);enabled=0;return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{assert(critical&&s==&uart.irq);if(rearm_failed)return -1;enabled=1;return 1;}
#include "irq.c"
static int now(void *v,uint64_t *out){(void)v;*out=clock_us;return 1;}
static int rd(void *v,uintptr_t a,uint32_t *out)
{
 (void)v;assert(!enabled&&!active);unsigned off=(unsigned)(a-uart.platform.uart.plan.base);
 if(off==8)*out=cause;
 else if(off==0x14)*out=0x60|(tail!=head?1:0)|(line_error?2:0);
 else if(off==0x7c)*out=6;
 else {assert(off==0&&tail!=head);*out=fifo[head++];}
 return 1;
}
static int wr(void *v,uintptr_t a,uint32_t x)
{
 (void)v;assert(!enabled&&!active);
 if(a==uart.platform.uart.plan.base){++service_writes;
  if(!drop)fifo[tail++]=(uint8_t)(x^(corrupt?1:0));return !write_uncertain;}
 assert(a==uart.platform.uart.plan.base+4);return 1;
}
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *p)
{assert(p==&uart.platform&&!enabled);++retain_calls;return A733_UART_PLATFORM_RETAINED;}
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *p)
{assert(p==&uart.platform);return &p->uart;}
enum a733_uart_platform_result a733_uart_platform_irq_sync(struct a733_uart_platform *p,uint64_t d)
{assert(p==&uart.platform&&d==uart.deadline);if(enabled||active)return A733_UART_PLATFORM_PENDING;
 if(sync_wait){--sync_wait;return A733_UART_PLATFORM_PENDING;}return A733_UART_PLATFORM_READY;}
enum a733_uart_platform_result a733_uart_platform_close(struct a733_uart_platform *p,unsigned quiet,uint64_t d)
{assert(p==&uart.platform&&quiet&&d==uart.deadline&&!enabled&&!active&&!uart.armed&&uart.sent==32&&uart.received==32);closing=1;return A733_UART_PLATFORM_PENDING;}
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *p)
{
 assert(p==&uart.platform);if(!closing)return A733_UART_PLATFORM_READY;
 if(close_failed)return A733_UART_PLATFORM_RETAINED;
 if(close_wait){--close_wait;return A733_UART_PLATFORM_PENDING;}return A733_UART_PLATFORM_CLOSED;
}
#include "run.c"
static void setup(void)
{
 memset(&uart,0,sizeof(uart));clock_us=1;drop=corrupt=line_error=write_uncertain=retain_calls=service_writes=closing=close_failed=0;
 cause=enabled=active=sync_wait=critical=rearm_failed=0;uart.masked=1;uart.irq=(struct gicv3_spi){0x3400000,36};
 close_wait=2;head=tail=task_notifies=0;uart.handle=(TaskHandle_t)&uart;uart.session.tx_bytes=uart.session.rx_bytes=32;uart.deadline=10000;uart.platform.initialized=1;
 for(unsigned i=0;i<32;++i)uart.tx[i]=(uint8_t)(i*7);
 uart.platform.uart=(struct a733_uart){.plan={.base=0x2502000,.actual_baud=115384},.ops={rd,wr,now},.state=A733_UART_READY,.ier=5};
}
static int step(void){clock_us+=10;return uart_step();}
static void started(void)
{assert(step()==0&&step()==0&&enabled&&uart.armed&&!service_writes);assert(step()==0&&!service_writes);}
static void deliver(unsigned event)
{assert(enabled);cause=event;active=1;uart_application_irq();assert(!enabled&&atomic_load(&uart.pending));active=0;}
int main(int argc,char **argv)
{
 (void)argv;if(argc>1){
  setup();started();deliver(2);assert(uart_irq_rearm()==0&&!enabled&&!atomic_load(&uart.failed));
  assert(step()==0&&uart.sent==32&&enabled);
  puts("IRQ/task pending race: a newly delivered notification defers rearm without false failure and is serviced next.");return 0;
 }
 setup();started();deliver(2);active=1;assert(step()==0&&!service_writes&&atomic_load(&uart.pending));active=0;
 sync_wait=1;assert(step()==0&&!service_writes&&atomic_load(&uart.pending));
 assert(step()==0&&uart.sent==32&&!uart.received&&enabled);deliver(12);
 assert(step()==0&&uart.received==32&&!enabled&&!uart.armed);
 assert(step()==0&&closing&&!atomic_load(&uart.result));assert(step()==0&&step()==0&&!atomic_load(&uart.result));
 assert(step()==1&&atomic_load(&uart.result)==1);
 assert(!memcmp(uart.payload+32,uart.payload+64,32)&&!memcmp(uart.payload+96,(uint8_t[32]){0},32));
 assert(atomic_load(&uart.notifications)==2&&task_notifies==2&&uart.tx_irqs==1&&uart.rx_irqs==1);
 unsigned writes=service_writes;clock_us=uart.deadline+1;assert(uart_step()==1&&service_writes==writes);
 setup();started();clock_us=uart.deadline;assert(uart_step()==-1&&!service_writes&&!enabled&&retain_calls==1); /* Lost IRQ has no polling fallback. */
 setup();drop=1;started();deliver(2);assert(step()==0&&uart.sent==32);clock_us=uart.deadline;assert(uart_step()==-1&&!enabled);
 setup();corrupt=1;started();deliver(2);assert(step()==0);deliver(4);assert(step()==-1&&retain_calls==1&&!enabled);
 setup();line_error=1;started();deliver(6);assert(step()==-1&&retain_calls==1&&!enabled);
 setup();write_uncertain=1;started();deliver(2);assert(step()==-1&&service_writes==1&&!uart.sent&&retain_calls==1);
 assert(step()==-1&&service_writes==1); /* An uncertain accepted byte is never repeated. */
 setup();close_failed=1;started();deliver(2);assert(step()==0);deliver(4);assert(step()==0&&step()==0&&step()==-1);
 assert(atomic_load(&uart.result)==2&&retain_calls==1&&!enabled);
 setup();rearm_failed=1;assert(step()==0&&step()==-1&&!service_writes&&retain_calls==1);
 setup();started();atomic_store(&uart.notifications,128);deliver(2);assert(step()==-1&&!service_writes&&!enabled);
 setup();started();deliver(1);assert(step()==0&&uart.other_irqs==1&&enabled&&!service_writes);
 puts("IRQ UART application: actual mask-only ISR, notified IIR TX/RX, no polling fallback, active/RWP deferral, exact copied IRQ counts, line errors, uncertain TX, bounded storm and retained close/rearm failure passed.");
}
