#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
enum{TASKS=2};
static TaskHandle_t task_handle[TASKS];
static atomic_uint stopped,stop;
static _Atomic(TaskHandle_t) stop_waiter;
unsigned sunstone_deadline_notify_index(void)
{return xTaskGetApplicationTaskTag(NULL)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN;}
#include "background-stop-events.inc"
static void worker(void *unused)
{(void)unused;(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);assert(atomic_load(&stop));stopped_one();vTaskSuspend(NULL);}
static BaseType_t tag(void *unused){(void)unused;return pdPASS;}
static void owner(void *argument)
{
    if(argument)vTaskSetApplicationTaskTag(NULL,tag);
    assert(sunstone_deadline_register());
    for(unsigned i=0;i<TASKS;++i)task_handle[i]=host_task_create("background",worker,NULL,3);
    vSunstoneBgStop();assert(!ulSunstoneBgStopped());
    assert(sunstone_deadline_wait_until(1000000)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(ulSunstoneBgStopped()==TASKS&&host_time_us()==0);
}
int main(int argc,char **argv)
{
    (void)argv;host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,(void *)(uintptr_t)(argc>1),4,2000000);
    puts("PASS final background stop acknowledgment wakes its retained caller");
}
