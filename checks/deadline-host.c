/* Execute the common deadline implementation with deterministic task/IRQ
 * interleavings. This models RTOS notifications and recursive exclusion;
 * selected-port compilation is a separate check, not a timing measurement. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "deadline-host/task.h"
#include "sunstone_deadline.h"
#include "../runtime/deadline.c"
static unsigned self=1,depth,notifies[18],irq_notifies,mode,level;
static uint64_t clock_us=100,programmed;
static unsigned kick_pending,programs,wait_calls,owners_checked;
static void (*while_waiting)(void);
static struct sunstone_deadline_record record;
void deadline_host_enter(void){++depth;}
void deadline_host_exit(void){assert(depth);--depth;}
unsigned deadline_host_isr_enter(void){++depth;return depth;}
void deadline_host_isr_exit(unsigned old){assert(old==depth);--depth;}
void deadline_host_yield(int woken){assert(!depth);(void)woken;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void *)(uintptr_t)self;}
uint64_t sunstone_deadline_now_us(void){return clock_us;}
void sunstone_deadline_program(uint64_t when){programmed=when;++programs;}
void sunstone_deadline_kick(void){assert(depth);kick_pending=1;}
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *woken)
{
    assert(depth);assert(++irq_notifies==1);
    ++notifies[(uintptr_t)task];*woken=pdTRUE;
}
static void fire(void)
{
    irq_notifies=0;sunstone_deadline_irq();assert(irq_notifies<=1);
}
static void kick(void)
{assert(kick_pending);kick_pending=0;sunstone_deadline_rearm();}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks)
{
    assert(clear==pdTRUE);
    if(ticks){
        assert(!depth&&ticks==portMAX_DELAY);++wait_calls;
        if(mode==6){assert(while_waiting);while_waiting();}
        else if(mode==1){kick();assert(programmed==120);clock_us=120;fire();}
        else if(mode==2){kick();assert(programmed==140);clock_us=130;++notifies[self];}
        else if(mode==3){
            /* Two owners arm before CPU0 receives its coalesced SGI. Each
             * due IRQ must wake only one; migration changes no ownership. */
            unsigned original=self;
            if(level++==0){
                self=2;assert(sunstone_deadline_wait_until(160)==SUNSTONE_DEADLINE_EXPIRED);self=original;
            }else{
                kick();assert(programmed==160);clock_us=160;
                fire();assert(notifies[1]==1&&notifies[2]==0&&programmed==160);
                fire();assert(notifies[2]==1&&!programmed);++owners_checked;
            }
        }else if(mode==4){
            /* An external hint already woke the task when the timer wins
             * cancellation. The remaining hint must not leak into its next wait. */
            ++notifies[self];clock_us=180;fire();assert(notifies[self]==2);
        }else if(mode==5){
            /* The timer is armed before the notification-take call, while
             * an external completion hint is already pending. */
            kick();assert(programmed==220&&notifies[self]);
        }else assert(0);
    }
    uint32_t n=notifies[self];notifies[self]=0;return n;
}
static void runtime_cases(void)
{
    assert(sunstone_deadline_wait_until(120)==SUNSTONE_DEADLINE_ERROR);
    assert(sunstone_deadline_register()&&sunstone_deadline_register());
    mode=1;assert(sunstone_deadline_wait_until(120)==SUNSTONE_DEADLINE_EXPIRED);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&record.sequence==1&&record.woken_us==120&&!record.armed&&!record.failed&&!programmed);
    sunstone_deadline_owner_progress((void *)2);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&record.woken_us==120&&!record.failed);
    sunstone_deadline_owner_progress((void *)1);
    mode=2;assert(sunstone_deadline_wait_until(140)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&!record.armed&&record.woken_us==130);
    /* A cancelled compare may fire once; it never notifies an idle slot. */
    clock_us=140;fire();assert(!irq_notifies&&!programmed);
    self=2;assert(sunstone_deadline_register());self=1;
    mode=3;assert(sunstone_deadline_wait_until(160)==SUNSTONE_DEADLINE_EXPIRED);
    assert(owners_checked&&notifies[1]==0&&notifies[2]==0);
    sunstone_deadline_owner_progress((void *)1);
    mode=4;assert(sunstone_deadline_wait_until(180)==SUNSTONE_DEADLINE_EXPIRED);
    assert(!notifies[1]);sunstone_deadline_owner_progress((void *)1);
    clock_us=200;mode=5;notifies[1]=1;
    assert(sunstone_deadline_wait_until(220)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(!notifies[1]);clock_us=220;fire();assert(!irq_notifies);
    assert(sunstone_deadline_wait_until(UINT64_MAX)==SUNSTONE_DEADLINE_ERROR);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&record.failed);
    sunstone_deadline_owner_progress((void *)2);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&record.failed);
    sunstone_deadline_owner_progress((void *)1);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&!record.failed);
    assert(sunstone_deadline_wait_until(220)==SUNSTONE_DEADLINE_EXPIRED);
    for(self=3;self<=16;++self)assert(sunstone_deadline_register());
    self=17;assert(!sunstone_deadline_register());
    assert(!depth&&wait_calls==6&&programs>=8);
    puts("PASS deadline arm, notification-before-sleep, exact expiry, cancellation, simultaneous owners, task-handle identity, fixed admission and owner progress");
}

/* Execute the real service supervisor against the same runtime state. */
#define SUNSTONE_OS_H
#define SUNSTONE_OS_APPLICATION_H
#define OS_SERVICES 7
#define OS_SERVICE_APPLICATION 5
#define OS_RENEW_US 1000000u
struct os_service { const char *name; uint32_t window_us; };
const struct os_service os_services[OS_SERVICES]={
    {"pmic",1000000},{"usb0",100},{"typec",1000000},{"power",1000000},
    {"usb2",1000000},{"application",1000000},{"thermal",1000000}};
struct os_session { struct { unsigned workload; uint64_t deadline_us; } board; };
static const struct os_session test_session={{0,100000}};
static const struct os_session *os_session_admitted(void){return &test_session;}
static int a733_board_host_workload(unsigned workload){(void)workload;return 1;}
static int os_application_finished(void){return 0;}
static int os_watchdog_reload(void *owner){(void)owner;return 1;}
static uint64_t a733_smp_counter(void){return clock_us*24;}
static const struct { const void *manifest; } ss_application={(void *)1};
#include "../apps/os/services.c"
static void service_alarm(void)
{
    kick();assert(programmed==12000);
    assert(os_services_healthy(clock_us)==1); /* Actual future alarm. */
    clock_us=12000;fire();
    assert(os_services_healthy(clock_us)==1); /* Woken owner not scheduled. */
}
static void service_immediate(void)
{assert(0);}
static void services_cases(void)
{
    memset(slots,0,sizeof slots);memset(notifies,0,sizeof notifies);
    self=1;clock_us=100;assert(board_service_deadline_register(1));
    for(unsigned i=0;i<OS_SERVICES;++i)board_service_progress(i);
    assert(os_services_healthy(clock_us)==1);
    assert(!board_service_blocked_until(1,100001));
    self=2;assert(!board_service_blocked_until(1,20000));self=1;
    clock_us=10000;assert(os_services_healthy(clock_us)==0);
    assert(board_service_blocked_until(1,20000));
    assert(os_services_healthy(clock_us)==0); /* Declaration alone cannot renew. */
    mode=6;while_waiting=service_alarm;
    assert(sunstone_deadline_wait_until(12000)==SUNSTONE_DEADLINE_EXPIRED);
    assert(os_services_healthy(clock_us)==1); /* Cancelled, before device check. */
    clock_us=12101;assert(os_services_healthy(clock_us)==0); /* Observation bound. */
    self=2;board_service_progress(1);assert(os_services_healthy(clock_us)==0);self=1;
    board_service_progress(1);assert(os_services_healthy(clock_us)==1);
    /* A certificate for the next sequence cannot reuse an older completed wait. */
    assert(board_service_blocked_until(1,14000));
    clock_us=12202;assert(os_services_healthy(clock_us)==0);
    /* A matured hold supplies its own bounded observation certificate. */
    while_waiting=service_immediate;
    assert(sunstone_deadline_wait_until(12200)==SUNSTONE_DEADLINE_EXPIRED);
    assert(os_services_healthy(clock_us)==1);
    clock_us=12303;assert(os_services_healthy(clock_us)==0);
    board_service_progress(1);
    assert(board_service_blocked_until(1,12400));
    assert(sunstone_deadline_wait_until(12300)==SUNSTONE_DEADLINE_EXPIRED);
    clock_us=12400;assert(os_services_healthy(clock_us)==0); /* Operation cutoff. */
    board_service_progress(1);assert(os_services_healthy(clock_us)==1);
    assert(board_service_blocked_until(1,13000));
    assert(sunstone_deadline_wait_until(UINT64_MAX)==SUNSTONE_DEADLINE_ERROR);
    assert(os_services_healthy(clock_us)==0); /* Failed waiter is sticky. */
    board_service_progress(1);assert(os_services_healthy(clock_us)==1);
    assert(!depth);
    puts("PASS service owner binding, actual-wait sequence, mature holds, fired/observing bounds, operation expiry and failed-wait progress");
}
int main(void){runtime_cases();services_cases();}
