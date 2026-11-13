/* Production sleep and stop operations with the real host kernel. The CPU
 * stress burst is outside this wait test. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
enum {TASKS=1};
static TaskHandle_t task_handle[TASKS];
static atomic_uint stop;
static _Atomic(TaskHandle_t) stop_waiter;
static unsigned waits,completed;
unsigned sunstone_deadline_notify_index(void)
{return xTaskGetApplicationTaskTag(NULL)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN;}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "background-events.inc"
static void observe(void *unused)
{(void)unused;assert(!completed&&waits==1&&eTaskGetState(task_handle[0])==eBlocked);}
static void finish(void *unused)
{(void)unused;vSunstoneBgStop();}
static BaseType_t tag(void *unused){(void)unused;return pdPASS;}
static void owner(void *argument)
{
    task_handle[0]=xTaskGetCurrentTaskHandle();
    if(argument)vTaskSetApplicationTaskTag(NULL,tag);
    assert(sunstone_deadline_register());
    host_run_at(5000,observe,NULL);host_run_at(10000,finish,NULL);
    sleep_until(20000);
    assert(atomic_load(&stop)&&host_time_us()==10000&&waits==1);completed=1;
}
int main(int argc,char **argv)
{
    (void)argv;host_timer_handler(sunstone_deadline_irq);
    host_rtos_run(owner,(void *)(uintptr_t)(argc>1),4,1000000);
    puts("PASS background wait stays blocked without an event and stop notifies the selected index");
}
