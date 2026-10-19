/* Exercise the actual application callback wrappers with health changes during
 * register access. MMIO is modeled; the FIFO service is the actual driver. */
#define SUNSTONE_UART_PRIVATE_H
#define SUNSTONE_A733_SMP_BOOT_H
#include "uart_session.h"
#include "a733_uart_platform_native.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
struct a733_platform_locks {atomic_uint retained;};
static struct {struct a733_board_session session;} board;
static struct {struct {struct a733_clock_leases clocks;struct a733_pins pins;struct a733_platform_locks locks;} shared;} providers;
struct uart_application {
 struct uart_session session;struct a733_uart_platform platform;
 struct a733_uart_platform_native native;struct a733_uart_platform_config config;
 struct a733_uart_platform_ops operations;void *handle;
 atomic_uint result,failed;unsigned stage;size_t sent,received;uint64_t deadline;
 uint8_t tx[32],rx[32],status[32],payload[128],digest[32],session_digest[32];
};
static unsigned healthy,raw_writes,raw_reads,lose_on_write,lose_clock_on_read;
static int board_live(void *v){(void)v;return healthy;}
static uint64_t a733_smp_counter(void){return 24;}
#include "native.c"
static int raw_current(void *v){assert(v==&uart.native);return 1;}
static int raw_now(void *v,uint64_t *t){assert(v==&uart.native);*t=1;return 1;}
static int raw_read(void *v,uintptr_t a,uint32_t *x)
{assert(v==&uart.native);++raw_reads;*x=a==0x250207c?6:0x60;
 if(lose_clock_on_read)atomic_store(&providers.shared.clocks.failed,1);return 1;}
static int raw_write(void *v,uintptr_t a,uint32_t x)
{assert(v==&uart.native&&a==0x2502000&&x==7);++raw_writes;if(lose_on_write)healthy=0;return 1;}
static void setup(void)
{
 memset(&uart,0,sizeof(uart));memset(&providers,0,sizeof(providers));healthy=1;raw_writes=raw_reads=lose_on_write=lose_clock_on_read=0;
 board.session.deadline_us=100000;uart.native.operations=(struct a733_uart_platform_ops){.current=raw_current,.now_us=raw_now,.read=raw_read,.write=raw_write};
 uart.platform.uart=(struct a733_uart){.plan={.base=0x2502000},.ops={read_register,write_register,now},.context=&uart,.state=A733_UART_READY};
}
int main(void)
{
 struct a733_uart_result r;uint8_t tx[]={7,7};
 setup();lose_on_write=1;
 assert(a733_uart_exchange(&uart.platform.uart,NULL,0,tx,2,32,100,&r)==A733_UART_FAULT);
 assert(raw_writes==1&&r.transmitted==0);unsigned reads=raw_reads;uint32_t v;
 assert(!write_register(&uart,0x2502000,7)&&!read_register(&uart,0x250207c,&v)&&raw_reads==reads&&raw_writes==1);
 assert(current(&uart)==1); /* Metadata retention still authenticates its owner. */
 setup();lose_clock_on_read=1;
 assert(a733_uart_exchange(&uart.platform.uart,NULL,0,tx,2,32,100,&r)==A733_UART_FAULT);
 assert(raw_reads==1&&!raw_writes&&current(&uart)==1);
 puts("UART native callbacks: board/clock failure between FIFO accesses blocks further MMIO and preserves owner-only metadata retention.");
}
