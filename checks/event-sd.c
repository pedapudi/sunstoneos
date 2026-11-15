/* Production SD waits with the real FreeRTOS notification/deadline code.
 * Hardware completion is scripted; this does not qualify a card or GIC. */
#include "host_rtos.h"
#include "event_groups.h"
#include "sunstone_deadline.h"
#include "a733_mmc_platform.h"
#include "sunstone_notify.h"
#include "sunstone_storage.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WAIT_INDEX SS_NOTIFY_REQUEST_WAIT
#define REGISTER_RECHECK_US 100u
static atomic_int ready;
static TaskHandle_t owner_task, test_task;
static _Atomic(TaskHandle_t) owner;
static atomic_uint posting;
static atomic_int closed;
static _Atomic uint32_t caller_core_mask;
static unsigned mode;
/* The refused-open case (mode 14) requests the CPU return after the first storage call has passed its own check and
 * before the owner's open: the first check answers no, every later one yes. */
static unsigned return_checks;
static int os_cpu_return_requested(void){return mode==14&&++return_checks>1;}
static struct { struct { atomic_uint pmic_ready; } shared; } providers;
static _Atomic(TaskHandle_t) pmic_waiter;
static struct a733_mmc_platform platform;
static uint64_t deadline;
static unsigned polls,retained,events,served;
static int completed;
static uint64_t now_us(void){return host_time_us();}
static int board_live(void *p){(void)p;return 1;}
static int sd_time_left(uint64_t until){return now_us()<until;}
unsigned sunstone_deadline_notify_index(void)
{return xTaskGetCurrentTaskHandle()==test_task?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN;}
enum a733_mmc_platform_result a733_mmc_platform_poll(struct a733_mmc_platform *p)
{
    assert(p==&platform);++polls;
    return completed?A733_MMC_COMPLETE:A733_MMC_PENDING;
}
enum a733_mmc_platform_result a733_mmc_platform_retain(struct a733_mmc_platform *p)
{assert(p==&platform);++retained;return A733_MMC_RETAINED;}
enum sunxi_mmc_wait sunxi_mmc_wait_hint(const struct sunxi_mmc *p,uint32_t *us)
{assert(p==&platform.core);if(us)*us=0;return mode==10?SUNXI_MMC_WAIT_NONE:SUNXI_MMC_WAIT_IRQ;}
/* The owner loop receives actual request hints, with hardware actions supplied. */
struct os_sd_hw {int unused;};
static const struct os_sd_hw hardware;
static const struct os_sd_hw *hw;
static _Atomic uint64_t idle_us,wake_count,first_us;
static _Atomic uint32_t owner_core_mask;
static _Atomic uint64_t read_started_us,read_closed_us,read_sleep_us,read_wakes,read_irq_recheck;
static _Atomic uint32_t read_owner_cores,irq_entry_count[8],irq_stray_count;
static atomic_uint close_proven;
static atomic_uint storage_state;
static atomic_uint_fast64_t entry_ttbr0;
static volatile uint8_t entry_seen, entry_tagged;      /* sd_owner.c: the owner's address space at its first instruction */
struct os_quiesce { int unused; };
static struct os_quiesce storage_quiesce;
static unsigned quiesce_completed,fail_notes;
static void os_quiesce_complete(struct os_quiesce *q){assert(q==&storage_quiesce);++quiesce_completed;}
static unsigned opens;
/* The owner's open. In the concurrent-start and failed-open cases it registers the owner's own deadline slot and waits on
 * it for 200 ms, as the real open's bounded waits do (apps/os/large_sd.c os_sd_open_owner); the failed open then returns
 * no device. */
static const struct os_sd_hw *os_sd_open_owner(void)
{
    ++opens;
    if(mode==12||mode==13||mode==15||mode==16)assert(sunstone_deadline_register());
    if(mode==12||mode==13||mode==15){
        uint64_t until=now_us()+(mode==15?3000000u:200000u);
        while(now_us()<until)assert(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
    }
    return mode==13||mode==15?NULL:&hardware;
}
static void board_fail_note(unsigned site,unsigned detail,const void *where)
{(void)site;(void)detail;(void)where;assert((mode==13||mode==15)&&!ready);++fail_notes;}
#define SUNSTONE_CRUMB_FAIL_SITE(group) 1u
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
static unsigned diag_word(void){return 0;}
static int owner_quiesce_step(void){return 0;}
enum kind { K_READ, K_ASYNC_READ, K_ENABLE, K_UNIT };
static uint32_t unit_sectors;
struct request {
    enum kind kind;uint64_t sector;uint32_t count;uint8_t *buffer;
    const uint8_t *source;size_t bytes;TaskHandle_t waiter;uint32_t value,token;
    unsigned event,operation,phase;atomic_int done;
};
static _Atomic(struct request *) loader_slot,writer_slot;
static struct request loader_async;
static void finish(struct request *,int);
static void serve(struct request *r)
{
    ++served;
    if(r->kind==K_UNIT)r->value=8u;
    if(mode==8){
        assert(sunstone_deadline_register());
        while(!completed)assert(sunstone_deadline_wait_until(now_us()+500000u)!=SUNSTONE_DEADLINE_ERROR);
    }
    if(mode==16){                     /* a request that takes 2 s: longer than the progress window, inside the piece bound */
        uint64_t until=now_us()+2000000u;
        while(now_us()<until)assert(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
    }
    finish(r,1);
    if(mode==0)xTaskNotifyGiveIndexed(test_task,SS_NOTIFY_DEADLINE);
}
#define CORE_STEPS 8u
enum { OPEN, IDENTIFY, READY };
static unsigned core_polls;
static int presence(struct a733_mmc_platform *p){(void)p;return 1;}
static int lock(struct a733_mmc_platform *p){(void)p;return 1;}
static int running(struct a733_mmc_platform *p){(void)p;return 1;}
static int timely(struct a733_mmc_platform *p){(void)p;return 1;}
static enum a733_mmc_platform_result fail(struct a733_mmc_platform *p){(void)p;assert(0);return A733_MMC_RETAINED;}
int sunxi_mmc_open(struct sunxi_mmc *p,uint64_t until){(void)p;(void)until;assert(0);return 0;}
void sunxi_mmc_poll(struct sunxi_mmc *p){(void)p;++core_polls;}
int sunxi_mmc_recoverable(const struct sunxi_mmc *p){(void)p;return 0;}
/* The service supervisor (apps/os/services.c) for the progress cases (modes 15 and 16), as checks/deadline-host.c runs
 * it. The application's progress window is 1 s. */
#define SUNSTONE_OS_H
#define SUNSTONE_OS_APPLICATION_H
#include "../apps/os/os-services.h"
#define OS_RENEW_US 1000000u
struct os_service { const char *name; uint64_t window_us; };
const struct os_service os_services[OS_SERVICES]={
    {"pmic",1000000},{"usb0",1000000},{"typec",1000000},{"power",1000000},
    {"usb2",1000000},{"application",1000000},OS_SERVICE_TABLE};
struct os_session { struct { unsigned workload; uint64_t deadline_us; } board; unsigned granted; };
static const struct os_session test_session={{0,0},OS_GRANT_USB0};
uint64_t board_runtime_deadline_us(void){return 100000000u;}
static const struct os_session *os_session_admitted(void){return &test_session;}
static int a733_board_host_workload(unsigned workload){(void)workload;return 1;}
static int os_application_finished(void){return 0;}
static int os_watchdog_reload(void *owner){(void)owner;return 1;}
static int os_npu_complete(void){return 0;}
static const struct { const void *manifest; } ss_application={(void *)1};
#include "../apps/os/services.c"
/* The state of the main task's OS wait (apps/os/application.c), whose functions event-sd.py extracts. The main task is
 * the test task in the progress cases and no task in the others. */
static TaskHandle_t main_task;
static atomic_uint main_wake_pending;
const unsigned os_wake_capacity=4;
TaskHandle_t os_wake_task[4];
unsigned os_wake_gave[4];
static struct { struct { uint64_t deadline_us; } session; } board={{100000000u}};
#include "event-sd-production.h"
/* The supervisor's verdict on the application while its main task waits: a sample in the shim's script task, where every
 * other service reports progress first. */
static unsigned healthy_samples;
static void healthy_now(void *unused)
{
    (void)unused;unsigned first=OS_SERVICES;
    for(unsigned i=0;i<OS_SERVICES;++i)if(i!=OS_SERVICE_APPLICATION)board_service_progress(i);
    const int state=services_state(now_us(),&first);
    if(state!=1)fprintf(stderr,"supervisor at %llu us: %d, first stale service %u\n",(unsigned long long)now_us(),state,first);
    assert(state==1);++healthy_samples;
}

static void event(void *p)
{
    ++events;
    BaseType_t woken=pdFALSE;
    if(mode==2)vTaskNotifyGiveIndexedFromISR(test_task,WAIT_INDEX,&woken);   /* a stray hint on the waiting index */
    else if(mode==3||mode==5){completed=1;vTaskNotifyGiveFromISR(owner_task,&woken);}
    else if(mode==6){atomic_store_explicit(&providers.shared.pmic_ready,1,memory_order_release);os_sd_pmic_ready();}
    else if(mode==10)completed=1;
    else if(mode==8){completed=1;vTaskNotifyGiveFromISR(owner_task,&woken);}
    (void)p;portYIELD_FROM_ISR(woken);
}
static void worker(void *unused)
{
    (void)unused;owner_task=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    deadline=now_us()+500000u;
    enum a733_mmc_platform_result r=read_drive(deadline);
    if(mode==3||mode==5||mode==10)assert(r==A733_MMC_COMPLETE&&polls==(mode==5?1u:mode==10?4u:2u)&&!retained);
    else assert(r==A733_MMC_RETAINED&&retained==1&&polls==2);
    assert(read_wakes==(mode==5?0u:mode==10?3u:1u));
    assert(read_irq_recheck==(mode==4?1u:0u));
    xTaskNotifyGiveIndexed(test_task,SS_NOTIFY_DEADLINE);vTaskSuspend(NULL);
}
/* The owner's publication of the open's result, as a task at a scripted time (modes 1 and 2). */
static void publisher(void *unused)
{
    (void)unused;
    assert(sunstone_deadline_register());
    assert(sunstone_deadline_wait_until(123456u)==SUNSTONE_DEADLINE_EXPIRED);
    publish_ready(1);
    vTaskSuspend(NULL);
}
/* Application tasks that hold no deadline slot and start storage (modes 12 to 14): a worker first, then a second task
 * while the owner is still opening the card, then a third after the open's result. In mode 12 they make the kit's calls:
 * the worker and the third ss_storage_open_read, the second ss_storage_enable; in modes 13 and 14 they call start(). */
static int started[3];
static void storage_caller(void *which)
{
    unsigned i=(unsigned)(uintptr_t)which;
    static const uint8_t bounds[16];
    started[i]=(mode!=12?start():i==1?ss_storage_enable(bounds,sizeof bounds):ss_storage_open_read())+1;
    xTaskNotifyGiveIndexed(test_task,SS_NOTIFY_DEADLINE);
    vTaskSuspend(NULL);
}
/* The context switches a case may make: each task it creates switches in and out once, the test task sleeps and wakes
 * once, and each scripted interrupt adds one wake of its target. A step scheduled with host_run_at runs in the shim's
 * script task, so it counts as a task. A finite tick wait in an idle phase adds a switch per expiry, 20 a second at 50 ms. */
static unsigned switch_bound(unsigned tasks,unsigned interrupts){return 2u*tasks+2u+interrupts;}
static void test(void *unused)
{
    (void)unused;test_task=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    host_timer_handler(sunstone_deadline_irq);
    ready_group=xEventGroupCreateStatic(&ready_storage);assert(ready_group);
    if(mode==0){
        uint64_t initial=host_switches();
        owner=owner_task=host_task_create("sd-idle",owner_main,NULL,2);
        /* Until a storage call asks, the owner leaves the card unopened and stays asleep. */
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(!opens&&!hw&&ready==0);
        assert(host_switches()-initial<=switch_bound(1,0));
        assert(start()==1&&opens==1&&ready==1);
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(wake_count==0&&served==0);
        struct request r={0};atomic_store(&loader_slot,&r);xTaskNotifyGive(owner_task);
        assert(sunstone_deadline_wait_until(now_us()+1000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(wake_count==1&&served==1&&r.done==1);
        printf("PASS idle SD owner: asleep before the first call, 0 unrequested wakes/s after the open; one request causes one wake\n");
    } else if(mode==1||mode==2){
        uint64_t initial=host_switches();
        host_task_create("sd-publish",publisher,NULL,2);
        if(mode==2)host_irq_at(now_us()+50000,event,NULL);
        assert(wait_ready()==1);assert(now_us()==123456u);
        assert(host_switches()-initial<=switch_bound(1,mode==2)); /* Finite tick waits exceed this before the publication. */
        puts(mode==1?"PASS ready publication wakes its waiter once":"PASS a stray hint on the waiting index leaves the wait for the open");
    } else if(mode==3||mode==4||mode==5||mode==10){
        owner_task=host_task_create("sd-read",worker,NULL,3);
        if(mode==10)host_irq_at(now_us()+250,event,NULL);
        else if(mode!=4)host_irq_at(now_us()+123456,event,NULL);
        if(mode==5){/* A hint precedes the owner's predicate check. */completed=1;}
        assert(sunstone_deadline_wait_until(now_us()+600000)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(mode==4?now_us()==500000:mode==5?now_us()==0:mode==10?now_us()==300:now_us()==123456);
        if(mode==5)assert(polls==1);
        puts("PASS read IRQ completion or retained absolute-deadline fault");
    } else if(mode==6||mode==7){
        owner_task=test_task;
        if(mode==6)host_run_at(now_us()+123456,event,NULL);
        uint64_t initial=host_switches();
        assert(read_pmic_ready()==(mode==6));assert(!pmic_waiter);
        uint64_t switched=host_switches()-initial;
        printf("PASS PMIC ready publication or one absolute startup expiry: %llu switches\n",(unsigned long long)switched);fflush(stdout);
        assert(switched<=switch_bound(mode==6,0));   /* mode 6's publication is a script step */
    } else if(mode==11){
        struct ss_storage_owner_stats before={0},after={0};
        assert(!ss_storage_owner_stats(&before));
        atomic_store(&close_proven,1);atomic_store(&read_owner_cores,1);
        assert(ss_storage_owner_stats(&before));
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(ss_storage_owner_stats(&after));
        assert(after.wakes==before.wakes&&after.elapsed_us-before.elapsed_us==1000000u);
        assert(after.asleep_us-before.asleep_us==1000000u&&!after.irq_recheck);
        puts("PASS retired SD loader: zero owner wakes in one measured idle second");
    } else if(mode==9){
        platform.stage=READY;platform.core.state=SUNXI_MMC_READING;
        assert(controller(&platform)==A733_MMC_PENDING&&core_polls==1);
        puts("PASS platform stops its core batch at the IRQ wait hint");
    } else if(mode==12){
        owner=owner_task=host_task_create("sd-owner",owner_main,NULL,2);
        host_task_create("worker",storage_caller,(void *)(uintptr_t)0,3);
        assert(sunstone_deadline_wait_until(now_us()+1000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(storage_state==SD_STARTING&&!started[0]);
        host_task_create("second",storage_caller,(void *)(uintptr_t)1,3);
        while(!started[0]||!started[1])
            assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        /* Both hear the one open. The writer mailbox holds one request, so the worker's request is served and the second
         * task's post is refused cleanly: its own request, and nothing of the worker's, is given back. */
        assert(started[0]==2&&started[1]==1&&opens==1&&ready==1&&now_us()==200000u);
        assert(served==1&&unit_sectors==8u);
        /* A call after the open returns from the ready wait at once. */
        host_task_create("late",storage_caller,(void *)(uintptr_t)2,3);
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(started[2]==2&&served==2&&opens==1&&now_us()==200000u);
        puts("PASS two kit calls during the open: both hear it, one is served and one refused cleanly; a later call returns at once");
    } else if(mode==13){
        owner=owner_task=host_task_create("sd-owner",owner_main,NULL,2);
        host_task_create("worker",storage_caller,(void *)(uintptr_t)0,3);
        assert(sunstone_deadline_wait_until(now_us()+1000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(storage_state==SD_STARTING&&!started[0]);
        host_task_create("second",storage_caller,(void *)(uintptr_t)1,3);
        while(!started[0]||!started[1])
            assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        /* The callers outrank the owner, so the note is already written when they hear the failure. */
        assert(started[0]==1&&started[1]==1&&opens==1&&ready==-1&&storage_state==SD_FAILED&&fail_notes==1);
        assert(now_us()==200000u);
        host_task_create("late",storage_caller,(void *)(uintptr_t)2,3);
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(started[2]==1&&opens==1&&now_us()==200000u);
        puts("PASS a failed open answers every waiting task and every later one");
    } else if(mode==14){
        owner=owner_task=host_task_create("sd-owner",owner_main,NULL,2);
        host_task_create("worker",storage_caller,(void *)(uintptr_t)0,3);
        while(!started[0])
            assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(started[0]==1&&!opens&&ready==-1&&storage_state==SD_CLOSED&&quiesce_completed==1);
        host_task_create("late",storage_caller,(void *)(uintptr_t)2,3);
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(started[2]==1&&!opens&&now_us()==0u);
        puts("PASS a CPU return before the open answers the waiting task without touching the card");
    } else if(mode==15||mode==16){
        /* The main task waits longer than its 1 s progress window, inside the bound: for an open that takes 3 s and fails
         * (15), or for a request that takes 2 s (16). The supervisor's verdict stays healthy throughout. */
        main_task=test_task;
        assert(board_service_deadline_register(OS_SERVICE_APPLICATION));
        for(unsigned i=0;i<OS_SERVICES;++i)board_service_progress(i);
        owner=owner_task=host_task_create("sd-owner",owner_main,NULL,2);
        const uint64_t from=now_us();
        const unsigned samples=mode==15?5u:3u;
        for(unsigned i=1;i<=samples;++i)host_run_at(from+i*500000u+250000u,healthy_now,NULL);
        if(mode==15)assert(start()==0&&ready==-1&&fail_notes==1&&now_us()==from+3000000u);
        else assert(ss_storage_open_read()==1&&served==1&&unit_sectors==8u&&now_us()==from+2000000u);
        assert(healthy_samples==samples&&os_services_healthy(now_us())==1);
        puts(mode==15?"PASS the main task waits 3 s for an open that fails; it hears the failure at once and the supervisor renews throughout":
                      "PASS the main task waits 2 s for its request; the supervisor renews throughout");
    } else if(mode==8){
        xTaskNotifyGive(test_task); /* The application's index zero remains owned by it. */
        owner=owner_task=host_task_create("sd-async",owner_main,NULL,2);
        assert(start()==1);
        host_irq_at(now_us()+123456,event,NULL);
        loader_async.kind=K_ASYNC_READ;
        assert(post(&loader_slot,&loader_async,0));
        assert(proxy_poll(NULL)==1&&now_us()==123456);
        assert(ulTaskNotifyTake(pdTRUE,0)==1);
        puts("PASS asynchronous loader waits for its completion index");
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)atoi(argv[1]);
    host_rtos_run(test,NULL,4,11000000u);return 0;
}
