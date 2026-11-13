/* The production periodic owner runs against the host kernel and deadline
 * runtime; the invalidation instruction is absent in its control form. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "sunstone_tlbi.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
enum {SUNSTONE_TLBI_PERIOD_TICKS=10};
#define SUNSTONE_TLBI_ISSUER 0
struct os_tlbi_stats {atomic_uint wakes,issued,core;atomic_uint_fast64_t max_ticks,total_ticks;};
static struct os_tlbi_stats os_tlbi_stats;
static unsigned finished,done,waits;
static TaskHandle_t task;
static int os_application_finished(void){return finished;}
static _Noreturn void ss_fail(uint32_t site){(void)site;abort();}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "tlbi-events.inc"
static void inspect(void *argument)
{
    unsigned sample=(unsigned)(uintptr_t)argument;
    if(sample==1){assert(!atomic_load(&os_tlbi_stats.wakes)&&waits==1);assert(eTaskGetState(task)==eBlocked);}
    else if(sample==2){struct sunstone_tlbi_sample status;
        assert(sunstone_tlbi_sample(&status)&&status.wakes==2&&!status.issued&&!status.form&&status.period_us==10000);
        finished=1;}
    else {assert(finished&&atomic_load(&os_tlbi_stats.wakes)==2);done=1;}
}
static void owner(void *unused)
{
    (void)unused;
    task=host_task_create("tlbi",issuer,NULL,3);
    host_run_at(5000,inspect,(void *)1);
    host_run_at(21000,inspect,(void *)2);
    host_run_at(35000,inspect,(void *)3);
    while(!done)vTaskDelay(1);
}
int main(int argc,char **argv)
{
    (void)argv;
    if(argc>1)host_timer_hold_until(13500);
    host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,NULL,4,1000000);
    puts("PASS periodic invalidation owner sleeps to its declared deadline without intermediate wakes");
}
