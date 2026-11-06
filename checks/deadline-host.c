/* Execute the common deadline implementation (runtime/deadline.c) and the service
 * supervisor (apps/os/services.c) on the real FreeRTOS kernel under the host
 * shim (checks/host/). Tasks, notifications, critical sections, the tick and the
 * compare timer are the kernel's and the shim's, not models written here; the
 * interleavings come from scheduled interrupts at exact virtual times.
 * Run as `deadline-host runtime` or `deadline-host services`: each scenario
 * needs the common code's fixed owner slots empty. Selected-port compilation is a
 * separate check, not a timing measurement. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_rtos.h"
#include "sunstone_deadline.h"

static struct sunstone_deadline_record record;
static TaskHandle_t t1, t2;
static unsigned timer_fires;
static void count_fire(void){++timer_fires;sunstone_deadline_irq();}

static void hint(void *task)
{
    BaseType_t woken=pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)task,&woken);
    portYIELD_FROM_ISR(woken);
}
static int pending_hint(void){return (int)ulTaskNotifyTake(pdTRUE,0);}

/* The second owner of the simultaneous-owner case. */
static volatile unsigned woke_order[2],woke_count;
static volatile int owner2_result=99, owner2_registered, owner2_leftover=99;
static void owner2(void *unused)
{
    (void)unused;
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    owner2_registered=sunstone_deadline_register();
    owner2_result=sunstone_deadline_wait_until(160);
    woke_order[woke_count++]=2;
    owner2_leftover=(int)ulTaskNotifyTake(pdTRUE,0);
    vTaskSuspend(NULL);
}
static void deliver_kick(void *unused){(void)unused;host_deadline_deliver_kick();}
static volatile int admitted[15];
static void admit(void *index){admitted[(uintptr_t)index]=sunstone_deadline_register();vTaskSuspend(NULL);}

static void runtime_cases(void *unused)
{
    (void)unused;
    t1=xTaskGetCurrentTaskHandle();
    assert(host_time_us()==100);
    /* Waiting before registration is refused. */
    assert(sunstone_deadline_wait_until(120)==SUNSTONE_DEADLINE_ERROR);
    assert(sunstone_deadline_register()&&sunstone_deadline_register());
    /* Exact expiry: the compare timer wakes the owner at its deadline. */
    assert(sunstone_deadline_wait_until(120)==SUNSTONE_DEADLINE_EXPIRED);
    assert(host_time_us()==120&&timer_fires==1);
    t2=host_task_create("owner2",owner2,NULL,2);
    assert(sunstone_deadline_owner_record(t1,&record)&&record.sequence==1&&record.woken_us==120&&!record.armed&&!record.failed&&!host_timer_compare_us());
    /* Another task's progress report cannot clear this owner's state. */
    sunstone_deadline_owner_progress(t2);
    assert(sunstone_deadline_owner_record(t1,&record)&&record.woken_us==120&&!record.failed);
    sunstone_deadline_owner_progress(t1);
    /* A device hint ends the wait before the deadline. */
    host_irq_at(130,hint,t1);
    assert(sunstone_deadline_wait_until(140)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(sunstone_deadline_owner_record(t1,&record)&&!record.armed&&record.woken_us==130);
    /* A cancelled compare fires once and notifies no idle slot. */
    {uint64_t gives=host_gives_total();host_advance_us(10);assert(host_time_us()==140&&host_gives_total()==gives&&!host_timer_compare_us());}
    /* Two owners arm before CPU0 receives its coalesced SGI. Each due IRQ wakes only
     * one owner, and migration changes no ownership. */
    host_deadline_defer_kicks(1);
    host_run_at(150,deliver_kick,NULL);
    xTaskNotifyGive(t2);
    {uint64_t before=timer_fires;
     assert(sunstone_deadline_wait_until(160)==SUNSTONE_DEADLINE_EXPIRED);
     woke_order[woke_count++]=1;
     vTaskDelay(1);
     /* Equal priorities run in wake order: the earlier slot is woken first. */
     assert(woke_count==2&&woke_order[0]==1&&woke_order[1]==2);
     assert(owner2_registered&&owner2_result==SUNSTONE_DEADLINE_EXPIRED&&owner2_leftover==0);
     assert(timer_fires-before==2&&host_max_gives_per_isr()==1&&!pending_hint()&&!host_timer_compare_us());}
    host_deadline_defer_kicks(0);
    sunstone_deadline_owner_progress(t1);
    /* An external hint wakes the task as the timer wins cancellation: the leftover
     * hint must not leak into the next wait. */
    {uint64_t at=host_time_us()+500;
     host_irq_at(at,hint,t1);
     assert(sunstone_deadline_wait_until(at)==SUNSTONE_DEADLINE_EXPIRED);
     assert(!pending_hint());}
    sunstone_deadline_owner_progress(t1);
    /* The timer is armed before the notification-take call while a completion hint is
     * already pending: the wait returns at once, notified. */
    {uint64_t at=host_time_us()+500;
     xTaskNotifyGive(t1);
     assert(sunstone_deadline_wait_until(at)==SUNSTONE_DEADLINE_NOTIFIED);
     assert(host_timer_compare_us()==at&&!pending_hint());
     {uint64_t gives=host_gives_total();host_advance_us(500);assert(host_time_us()==at&&host_gives_total()==gives);}
     assert(sunstone_deadline_wait_until(UINT64_MAX)==SUNSTONE_DEADLINE_ERROR);
     assert(sunstone_deadline_owner_record(t1,&record)&&record.failed);
     sunstone_deadline_owner_progress(t2);
     assert(sunstone_deadline_owner_record(t1,&record)&&record.failed);
     sunstone_deadline_owner_progress(t1);
     assert(sunstone_deadline_owner_record(t1,&record)&&!record.failed);
     assert(sunstone_deadline_wait_until(at)==SUNSTONE_DEADLINE_EXPIRED);}
    /* Fixed admission: sixteen owners, the seventeenth refused. */
    for(unsigned i=0;i<14;++i)host_task_create("owner",admit,(void *)(uintptr_t)i,3);
    host_task_create("owner",admit,(void *)(uintptr_t)14,3);
    for(unsigned i=0;i<14;++i)assert(admitted[i]==1);
    assert(admitted[14]==0);
    assert(!host_critical_depth());
    puts("PASS deadline arm, notification-before-sleep, exact expiry, cancellation, simultaneous owners, task-handle identity, fixed admission and owner progress");
}

/* Execute the real service supervisor against the same runtime state. The board
 * session that the OS sees has zero time fields: the absolute runtime deadline
 * exists only in board.session, so it is supplied by board_runtime_deadline_us(). */
#define SUNSTONE_OS_H
#define SUNSTONE_OS_APPLICATION_H
/* The grant bits and service ids come from the declaration (apps/os/os-services.h), which includes nothing. The services
 * after the thermal owner exist in every image and are required only when the session grants them: this session grants
 * none. The board's USB0 window is shortened for the model. */
#include "os-services.h"
#define OS_RENEW_US 1000000u
struct os_service { const char *name; uint32_t window_us; };
const struct os_service os_services[OS_SERVICES]={
    {"pmic",1000000},{"usb0",100},{"typec",1000000},{"power",1000000},
    {"usb2",1000000},{"application",1000000},OS_SERVICE_TABLE};
struct os_session { struct { unsigned workload; uint64_t deadline_us; } board; unsigned granted; };
static const struct os_session test_session={{0,0},0};
static uint64_t board_runtime_deadline_us_value=100000;
uint64_t board_runtime_deadline_us(void){return board_runtime_deadline_us_value;}
static const struct os_session *os_session_admitted(void){return &test_session;}
static int a733_board_host_workload(unsigned workload){(void)workload;return 1;}
static int os_application_finished(void){return 0;}
static int os_watchdog_reload(void *owner){(void)owner;return 1;}
/* The NPU service is in every image; this session does not grant it, so its finished state is never asked. */
static int os_npu_complete(void){return 0;}
static const struct { const void *manifest; } ss_application={(void *)1};
#ifndef SERVICES_SOURCE
#define SERVICES_SOURCE "../apps/os/services.c"
#endif
#include SERVICES_SOURCE

static void healthy_is(uint64_t at,int want)
{assert(host_time_us()==at);assert(os_services_healthy(at)==want);}
static void check_future_alarm(void *unused)
{(void)unused;assert(host_timer_compare_us()==12000);healthy_is(10000,1);}
static void check_woken_owner(void *unused){(void)unused;healthy_is(12000,1);}
static void check_lost_alarm_inside(void *unused){(void)unused;healthy_is(13100,1);}
static void check_lost_alarm_late(void *unused){(void)unused;healthy_is(13101,0);}
static void check_lost_alarm_delivered(void *unused){(void)unused;healthy_is(13102,0);}
static void other_task_progress(void *unused)
{
    (void)unused;
    assert(!board_service_blocked_until(1,20000));
    board_service_progress(1);
    assert(os_services_healthy(host_time_us())==0);
}
static void other_task_blocked(void *unused){(void)unused;assert(!board_service_blocked_until(1,20000));}

static void services_cases(void *unused)
{
    (void)unused;
    t1=xTaskGetCurrentTaskHandle();
    assert(host_time_us()==100);
    assert(board_service_deadline_register(1));
    for(unsigned i=0;i<OS_SERVICES;++i)board_service_progress(i);
    assert(os_services_healthy(host_time_us())==1);
    assert(!board_service_blocked_until(1,100001)); /* Beyond the runtime deadline. */
    host_run_now(other_task_blocked,NULL);
    host_advance_us(9900);
    assert(os_services_healthy(host_time_us())==0);
    assert(board_service_blocked_until(1,20000));
    assert(os_services_healthy(host_time_us())==0); /* Declaration alone cannot renew. */
    host_run_at(10000,check_future_alarm,NULL);   /* Actual future alarm. */
    host_run_at(12000,check_woken_owner,NULL);    /* Woken owner not yet scheduled. */
    assert(sunstone_deadline_wait_until(12000)==SUNSTONE_DEADLINE_EXPIRED);
    assert(os_services_healthy(host_time_us())==1); /* Cancelled, before device check. */
    host_advance_us(101);
    assert(os_services_healthy(host_time_us())==0); /* Observation bound. */
    host_run_now(other_task_progress,NULL);
    board_service_progress(1);assert(os_services_healthy(host_time_us())==1);
    /* A certificate for the next sequence cannot reuse an older completed wait. */
    assert(board_service_blocked_until(1,14000));
    host_advance_us(101);assert(host_time_us()==12202);
    assert(os_services_healthy(host_time_us())==0);
    /* A matured hold supplies its own bounded observation certificate. */
    assert(sunstone_deadline_wait_until(12200)==SUNSTONE_DEADLINE_EXPIRED);
    assert(os_services_healthy(host_time_us())==1);
    host_advance_us(101);assert(host_time_us()==12303);
    assert(os_services_healthy(host_time_us())==0);
    board_service_progress(1);
    assert(board_service_blocked_until(1,12400));
    assert(sunstone_deadline_wait_until(12300)==SUNSTONE_DEADLINE_EXPIRED);
    host_advance_us(97);assert(host_time_us()==12400);
    assert(os_services_healthy(host_time_us())==0); /* Operation cutoff. */
    board_service_progress(1);assert(os_services_healthy(host_time_us())==1);
    assert(board_service_blocked_until(1,13000));
    assert(sunstone_deadline_wait_until(UINT64_MAX)==SUNSTONE_DEADLINE_ERROR);
    assert(os_services_healthy(host_time_us())==0); /* Failed waiter is sticky. */
    board_service_progress(1);assert(os_services_healthy(host_time_us())==1);
    assert(board_service_blocked_until(1,20000));
    /* The alarm programmed for 13000 reaches the CPU only at 13102. */
    host_timer_hold_until(13102);
    host_run_at(13100,check_lost_alarm_inside,NULL);
    host_run_at(13101,check_lost_alarm_late,NULL);
    host_run_at(13102,check_lost_alarm_delivered,NULL);
    assert(sunstone_deadline_wait_until(13000)==SUNSTONE_DEADLINE_EXPIRED);
    assert(host_time_us()==13102);
    host_timer_hold_until(0);
    assert(os_services_healthy(host_time_us())==0);
    board_service_progress(1);assert(os_services_healthy(host_time_us())==1);
    assert(!host_critical_depth());
    puts("PASS service owner binding, actual-wait sequence, mature holds, lost/late alarm bounds, operation expiry and failed-wait progress");
}
int main(int argc,char **argv)
{
    int services=argc>1&&!strcmp(argv[1],"services");
    assert(argc>1&&(services||!strcmp(argv[1],"runtime")));
    host_timer_handler(count_fire);
    host_set_time_us(100);
    host_rtos_run(services?services_cases:runtime_cases,NULL,2,1000000);
    return 0;
}
