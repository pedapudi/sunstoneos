/* Actual reset/hold functions, with modeled register writes and a real host
 * scheduler/timer. This checks timing and sleep, not physical USB behavior. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define COUNTER_HZ UINT64_C(24000000)
#define USB UINT64_C(0x04100000)
#define CCU UINT64_C(0x02002000)
static unsigned waits,writes;
static uint64_t written_at[5];
static TaskHandle_t owner_task;
static uint64_t counter(void){return host_counter();}
static _Noreturn void platform_panic(const char *message,unsigned line)
{fprintf(stderr,"%s:%u\n",message,line);abort();}
static uint32_t mmio_read(uintptr_t address)
{(void)address;return 0;}
static void a733_mmio_dsb_sy(void){}
static void a733_mmio_write32_dsb_sy_after(uintptr_t address,uint32_t value)
{
    static const uintptr_t addresses[5]={CCU+0x1300,CCU+0x1304,CCU+0x1304,USB+0x420,USB+0x410};
    static const uint32_t values[5]={1u<<30,1u<<8,(1u<<8)|(1u<<24),1,1u<<5};
    assert(writes<5&&address==addresses[writes]&&value==values[writes]);written_at[writes++]=host_time_us();
}
static void platform_puts(const char *text){(void)text;}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "usb-delay-events.inc"
static void observe(void *unused)
{(void)unused;assert(!writes&&waits==1&&eTaskGetState(owner_task)==eBlocked);}
static void hint(void *unused)
{(void)unused;xTaskNotifyGive(owner_task);}
static void owner(void *argument)
{
    owner_task=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    host_run_at(75000,observe,NULL);
    if(argument)host_run_at(100000,hint,NULL);
    a733_usb_prepare();
    assert(writes==5&&waits==(argument?4u:3u));
    assert(written_at[0]==150001&&written_at[1]==150001&&written_at[2]==151002);
    assert(written_at[3]==151043&&written_at[4]==151043);
    tusb_time_delay_ms_api(1);assert(host_time_us()==152044);
}
int main(int argc,char **argv)
{
    (void)argv;host_timer_handler(sunstone_deadline_irq);
    host_rtos_run(owner,(void *)(uintptr_t)(argc>1),4,1000000);
    puts("PASS USB detach/reset holds block to exact minimums and survive an early notification");
}
