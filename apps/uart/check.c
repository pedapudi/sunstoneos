/* Host model of the finite application; the FIFO controller is the real driver.
 * Platform ownership/drain behavior is covered by a733-uart-platform.c. */
#define SUNSTONE_UART_PRIVATE_H
#define SUNSTONE_A733_SMP_BOOT_H
#include "a733_uart_platform.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define UART_OUTPUT_BYTES 128u
struct uart_application {
 struct {unsigned tx_bytes,rx_bytes;} session;
 struct a733_uart_platform platform;
 atomic_uint result,failed;
 unsigned stage;size_t sent,received;uint64_t deadline;
 uint8_t tx[32],rx[32],status[32],payload[128],digest[32],session_digest[32];
};
static struct uart_application uart;
static uint64_t clock_us;
static unsigned drop,corrupt,line_error,write_uncertain,retain_calls,service_writes,closing,close_failed,close_wait;
static uint8_t fifo[32];static unsigned head,tail;
static uint64_t a733_smp_counter(void){return clock_us*24;}
static int uart_live(void){return !atomic_load(&uart.failed);}
static void vTaskDelay(unsigned n){(void)n;}
static int now(void *v,uint64_t *out){(void)v;*out=clock_us;return 1;}
static int rd(void *v,uintptr_t a,uint32_t *out)
{
 (void)v;unsigned off=(unsigned)(a-uart.platform.uart.plan.base);
 if(off==0x14)*out=0x60|(tail!=head?1:0)|(line_error?2:0);
 else if(off==0x7c)*out=6;
 else {assert(off==0&&tail!=head);*out=fifo[head++];}
 return 1;
}
static int wr(void *v,uintptr_t a,uint32_t x)
{
 (void)v;if(a==uart.platform.uart.plan.base){++service_writes;
  if(!drop)fifo[tail++]=(uint8_t)(x^(corrupt?1:0));
  return !write_uncertain;}
 assert(a==uart.platform.uart.plan.base+4);return 1;
}
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *p)
{assert(p==&uart.platform);++retain_calls;return A733_UART_PLATFORM_RETAINED;}
enum a733_uart_platform_result a733_uart_platform_live(struct a733_uart_platform *p,uint64_t d)
{assert(p==&uart.platform&&d==uart.deadline&&!closing);return A733_UART_PLATFORM_READY;}
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *p)
{assert(p==&uart.platform);return &p->uart;}
enum a733_uart_platform_result a733_uart_platform_close(struct a733_uart_platform *p,unsigned quiet,uint64_t d)
{assert(p==&uart.platform&&quiet&&d==uart.deadline&&uart.sent==32&&uart.received==32);closing=1;return A733_UART_PLATFORM_PENDING;}
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *p)
{
 assert(p==&uart.platform);
 if(!closing)return A733_UART_PLATFORM_READY;
 if(close_failed)return A733_UART_PLATFORM_RETAINED;
 if(close_wait){--close_wait;return A733_UART_PLATFORM_PENDING;}
 return A733_UART_PLATFORM_CLOSED;
}
#include "run.c"
static void setup(void)
{
 memset(&uart,0,sizeof(uart));clock_us=1;drop=corrupt=line_error=write_uncertain=retain_calls=service_writes=closing=close_failed=0;
 close_wait=2;head=tail=0;uart.session.tx_bytes=uart.session.rx_bytes=32;uart.deadline=10000;uart.platform.initialized=1;
 for(unsigned i=0;i<32;++i)uart.tx[i]=(uint8_t)(i*7);
 uart.platform.uart=(struct a733_uart){.plan={.base=0x2502000,.actual_baud=115384},.ops={rd,wr,now},.state=A733_UART_READY};
}
static int step(void){clock_us+=10;return uart_step();}
int main(void)
{
 setup();assert(step()==0);assert(step()==0);assert(uart.sent==32&&!uart.received);
 assert(step()==0&&uart.received==32);assert(step()==0&&closing&&!atomic_load(&uart.result));
 assert(step()==0&&step()==0&&!atomic_load(&uart.result));assert(step()==1&&atomic_load(&uart.result)==1);
 assert(!memcmp(uart.payload+32,uart.payload+64,32)&&!memcmp(uart.payload+96,(uint8_t[32]){0},32));
 unsigned writes=service_writes;clock_us=uart.deadline+1;assert(uart_step()==1&&service_writes==writes);
 setup();drop=1;assert(step()==0&&step()==0);clock_us=uart.deadline;assert(uart_step()==-1&&retain_calls==1);
 writes=service_writes;assert(uart_step()==-1&&service_writes==writes&&atomic_load(&uart.result)==2);
 setup();corrupt=1;assert(step()==0&&step()==0&&step()==-1&&retain_calls==1);
 setup();line_error=1;assert(step()==0&&step()==-1&&retain_calls==1);
 setup();write_uncertain=1;assert(step()==0&&step()==-1&&service_writes==1&&!uart.sent&&retain_calls==1);
 assert(step()==-1&&service_writes==1); /* An uncertain accepted byte is never repeated. */
 setup();close_failed=1;assert(step()==0&&step()==0&&step()==0&&step()==0&&step()==-1);
 assert(atomic_load(&uart.result)==2&&retain_calls==1);
 puts("Finite UART application: real FIFO echo, copied completion after close, absent/corrupt peer, line errors, uncertain write and retained drain failure passed.");
}
