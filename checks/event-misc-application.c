/* Real application wait functions, with FreeRTOS task switching and the real
 * deadline runtime. Device producers and the session are modeled here. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
static TaskHandle_t main_task;
static atomic_uint main_wake_pending;
const unsigned os_wake_capacity=1;
TaskHandle_t os_wake_task[1];
unsigned os_wake_gave[1];
static struct {struct {uint64_t deadline_us;} session;} board;
static struct {uint64_t window_us;} os_services[1]={{1000000}};
enum {OS_SERVICE_APPLICATION};
static atomic_uint_fast64_t finish_by;
static unsigned complete,progresses,waits,race;
static void os_wake(TaskHandle_t);
static int board_service_blocked_until(unsigned service,uint64_t until)
{
    assert(service==OS_SERVICE_APPLICATION&&until==5000000);
    if(race){complete=1;os_wake(main_task);}
    return 1;
}
static int ss_live(void){return 1;}
static uint64_t a733_smp_counter(void){return host_counter();}
static void ss_progress(void){++progresses;}
static _Noreturn void ss_fail(uint32_t site){fprintf(stderr,"failure %u\n",site);abort();}
unsigned sunstone_deadline_notify_index(void){return SS_NOTIFY_DEADLINE;}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "application-events.inc"
static int done(void){return complete;}
static void observe(void *unused)
{
    (void)unused;
    assert(!complete&&progresses==1&&waits==1);
    assert(eTaskGetState(main_task)==eBlocked);
}
static void publish(void *unused)
{(void)unused;complete=1;os_wake(main_task);}
static void owner(void *unused)
{
    (void)unused;main_task=xTaskGetCurrentTaskHandle();board.session.deadline_us=5000000;
    atomic_store(&finish_by,4000000);
    assert(sunstone_deadline_register());
    if(!race){host_run_at(1000000,observe,NULL);host_run_at(2000000,publish,NULL);}
    application_wait_for(done,1);
    assert(complete&&progresses==1&&waits==(race?0u:1u));
    assert(host_time_us()==(race?0u:2000000u));
    assert(!os_wake_task[0]&&!os_wake_gave[0]);
}
int main(int argc,char **argv)
{
    (void)argv;race=argc>1;
    host_timer_handler(sunstone_deadline_irq);
    host_rtos_run(owner,NULL,4,6000000);
    puts("PASS application blocks without an event and observes publication across wait registration");
}
