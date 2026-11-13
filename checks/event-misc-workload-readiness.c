#include "host_rtos.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static TaskHandle_t main_task;
static struct {unsigned waits;} result;
static unsigned clock_ready,thermal_ready,progresses,race;
static uint64_t finish_by;
static uint64_t now(void){return host_time_us();}
static int ss_live(void){return 1;}
static void ss_progress(void){++progresses;}
static _Noreturn void ss_fail(unsigned code){fprintf(stderr,"failure %x\n",code);abort();}
static int ss_clock_ready(void)
{
    unsigned ready=clock_ready;
    if(race&&!ready){clock_ready=thermal_ready=1;xTaskNotifyGive(main_task);}
    return ready;
}
static int ss_throttled(void){return !thermal_ready;}
static void ss_wait_until(uint64_t until)
{
    assert(until==finish_by);
    host_advance_us(250); /* Preemption after the caller observed its clock. */
    assert(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
}
#include "workload-readiness.inc"
static void observe(void *unused)
{(void)unused;assert(result.waits==1&&progresses==1&&eTaskGetState(main_task)==eBlocked);}
static void clock_event(void *unused)
{(void)unused;clock_ready=1;xTaskNotifyGive(main_task);}
static void thermal_event(void *unused)
{(void)unused;thermal_ready=1;xTaskNotifyGive(main_task);}
static void owner(void *unused)
{
    (void)unused;main_task=xTaskGetCurrentTaskHandle();finish_by=5000000;
    assert(sunstone_deadline_register());
    if(!race){host_run_at(1000000,observe,NULL);host_run_at(2000000,clock_event,NULL);host_run_at(3000000,thermal_event,NULL);}
    wait_ready();
    assert(clock_ready&&thermal_ready&&result.waits==(race?1u:2u));
    assert(host_time_us()==(race?250u:3000000u));
}
int main(int argc,char **argv)
{
    (void)argv;race=argc>1;host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,NULL,4,6000000);
    puts("PASS workload readiness blocks until clock and thermal publications");
}
