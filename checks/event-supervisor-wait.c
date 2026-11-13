/* Real FreeRTOS, notification waits and precise timer; no supervisor policy
 * or watchdog operations are substituted for the wait being tested. */
#include "host_rtos.h"
#include "../apps/os/supervisor-wait.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct os_supervisor_wait schedule;
static TaskHandle_t owner;
static const char *scenario;
static uint64_t events_at[64],untils[64];
static unsigned events,waits,registrations,owner_registrations,refused;
static unsigned fail_wait;
int __real_sunstone_deadline_register(void);
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_register(void)
{
    ++registrations;
    if(owner&&owner==xTaskGetCurrentTaskHandle())++owner_registrations;
    return __real_sunstone_deadline_register();
}
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{
    if(xTaskGetCurrentTaskHandle()==owner){
        assert(waits<64);untils[waits++]=until;
        if(fail_wait)return SUNSTONE_DEADLINE_ERROR;
    }
    return __real_sunstone_deadline_wait_until(until);
}
static void fill_slot(void *unused)
{(void)unused;assert(sunstone_deadline_register());vTaskSuspend(NULL);}
static void supervisor(void *unused)
{
    (void)unused;owner=xTaskGetCurrentTaskHandle();
    int opened=os_supervisor_wait_init(&schedule);
    if(!strcmp(scenario,"registration-failure")){
        assert(!opened&&schedule.owner==owner&&schedule.failed);
        assert(!os_supervisor_wait_init(&schedule)&&owner_registrations==1);
        assert(os_supervisor_wait_next(&schedule)==SUNSTONE_DEADLINE_ERROR&&!waits);
        ++refused;vTaskSuspend(NULL);return;
    }
    assert(opened&&owner_registrations==1);
    assert(!os_supervisor_wait_init(&schedule)&&owner_registrations==1);
    if(!strcmp(scenario,"wait-failure")){
        fail_wait=1;
        assert(os_supervisor_wait_next(&schedule)==SUNSTONE_DEADLINE_ERROR&&schedule.failed);
        assert(os_supervisor_wait_next(&schedule)==SUNSTONE_DEADLINE_ERROR&&waits==1);
        ++refused;vTaskSuspend(NULL);return;
    }
    for(;;){
        int event=os_supervisor_wait_next(&schedule);
        assert(event>=0&&events<64);events_at[events++]=host_time_us();
        if(!strcmp(scenario,"late-service")&&events==1)host_advance_us(45000);
    }
}
static void hint(void *unused)
{
    (void)unused;BaseType_t woken=pdFALSE;
    vTaskNotifyGiveFromISR(owner,&woken);portYIELD_FROM_ISR(woken);
}
static void wait_for(uint64_t until)
{while(host_time_us()<until)assert(sunstone_deadline_wait_until(until)>=0);}
static void test(void *unused)
{
    (void)unused;host_advance_us(123);host_timer_handler(sunstone_deadline_irq);
    if(!strcmp(scenario,"registration-failure")){
        assert(host_task_create("slot one",fill_slot,NULL,3));
        assert(host_task_create("slot two",fill_slot,NULL,3));
    }
    assert(host_task_create("supervisor",supervisor,NULL,3));
    if(!strcmp(scenario,"registration-failure")||!strcmp(scenario,"wait-failure")){
        assert(refused==1&&schedule.failed&&owner_registrations==1);
        puts("PASS registration/wait failure refuses once with retained owner identity");return;
    }
    assert(schedule.owner==owner&&schedule.next_us==20123&&!events);
    if(!strcmp(scenario,"other-owner")){
        assert(os_supervisor_wait_next(&schedule)==SUNSTONE_DEADLINE_ERROR);
        assert(!os_supervisor_wait_init(&schedule)&&schedule.owner==owner&&!schedule.failed);
        assert(owner_registrations==1&&waits==1&&schedule.next_us==20123);
    }
    assert(sunstone_deadline_register());
    if(!strcmp(scenario,"early-hints")){
        host_irq_at(5007,hint,NULL);host_irq_at(19007,hint,NULL);
        wait_for(5006);assert(!events);
        wait_for(5010);assert(events==1&&events_at[0]==5007&&schedule.next_us==20123);
        wait_for(19010);assert(events==2&&events_at[1]==19007&&schedule.next_us==20123);
        wait_for(20123);assert(events==3&&events_at[2]==20123);
        puts("PASS explicit hints wake checks without deferring the 20ms phase");return;
    }
    wait_for(20122);assert(!events);
    if(!strcmp(scenario,"late-service")){
        wait_for(20124);assert(host_time_us()==65123&&events==1&&schedule.next_us==80123);
        assert(waits==2&&untils[1]==80123);
        wait_for(80122);assert(events==1);
        wait_for(80123);assert(events==2&&events_at[1]==80123);
        puts("PASS delayed checks skip elapsed phases and block on the first future deadline");return;
    }
    wait_for(1000123);assert(events==50&&owner_registrations==1);
    for(unsigned i=0;i<events;++i)assert(events_at[i]==123+(i+1)*UINT64_C(20000));
    puts("PASS no eventless wake and exactly 50 deadline wakes per idle second");
}
int main(int argc,char **argv)
{assert(argc==2);scenario=argv[1];host_rtos_run(test,NULL,2,2000000);return 0;}
