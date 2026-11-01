/* Exercise the pinned kernel's actual runtime query with diagnostic flags.
 * Scheduler transitions and the monotonic counter are modeled. */
#include "FreeRTOS.h"
#include "task.h"
__attribute__((noinline)) configRUN_TIME_COUNTER_TYPE ulTaskGetRunTimeCounter(const TaskHandle_t);
#include SUNSTONE_LATENCY_KERNEL
static TCB_t fixture_running,fixture_blocked,fixture_idle;
static uint64_t fixture_counter;
uint64_t fixture_counter_results[5];
static unsigned critical_depth;
uint64_t a733_smp_counter(void){return fixture_counter;}
void vPortEnterCritical(void){++critical_depth;}
void vPortExitCritical(void){--critical_depth;}
__attribute__((noinline)) _Noreturn void platform_panic(const char *name,unsigned line)
{(void)name;(void)line;for(;;){}}
__attribute__((noinline)) void fixture_counter_done(void){for(;;){}}
void fixture_counter_entry(void)
{
    _Static_assert(sizeof fixture_running.ulRunTimeCounter==8,"diagnostic TCB counter width");
    fixture_running.ulRunTimeCounter=700;fixture_blocked.ulRunTimeCounter=400;
    pxCurrentTCB=&fixture_running;ulTaskSwitchedInTime[0]=1000;fixture_counter=1600;
    fixture_counter_results[0]=ulTaskGetRunTimeCounter(NULL);
    fixture_counter_results[1]=ulTaskGetRunTimeCounter(&fixture_blocked);
    fixture_counter=2000;fixture_counter_results[2]=ulTaskGetRunTimeCounter(NULL);
    /* Switch away for a modeled hardware wait. Its time belongs to idle. */
    fixture_running.ulRunTimeCounter=1700;pxCurrentTCB=&fixture_idle;ulTaskSwitchedInTime[0]=2000;
    fixture_counter=6000;fixture_counter_results[3]=ulTaskGetRunTimeCounter(&fixture_running);
    pxCurrentTCB=&fixture_running;ulTaskSwitchedInTime[0]=6000;fixture_counter=6100;
    fixture_counter_results[4]=ulTaskGetRunTimeCounter(NULL);
    configASSERT(!critical_depth);fixture_counter_done();
}
