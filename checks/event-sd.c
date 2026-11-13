/* Production SD waits with the real FreeRTOS notification/deadline code.
 * Hardware completion is scripted; this does not qualify a card or GIC. */
#include "host_rtos.h"
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
static _Atomic(TaskHandle_t) ready_waiter;
static unsigned ready_index=SS_NOTIFY_DEADLINE;
static TaskHandle_t owner_task, test_task;
static _Atomic(TaskHandle_t) owner;
static atomic_uint posting;
static atomic_int closed;
static _Atomic uint32_t caller_core_mask;
static int os_cpu_return_requested(void){return 0;}
static struct { struct { atomic_uint pmic_ready; } shared; } providers;
static _Atomic(TaskHandle_t) pmic_waiter;
static struct a733_mmc_platform platform;
static uint64_t deadline;
static unsigned mode,polls,retained,events,served;
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
#define SD_OPEN 2u
#define SD_FAILED 4u
static int owner_open_allowed(void){return 1;}
static const struct os_sd_hw *os_sd_open_owner(void){return &hardware;}
static void board_fail_note(unsigned site,unsigned detail,const void *where)
{(void)site;(void)detail;(void)where;assert(0);}
#define SUNSTONE_CRUMB_FAIL_SITE(group) 1u
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
static unsigned diag_word(void){return 0;}
static int owner_quiesce_step(void){return 0;}
enum kind { K_READ, K_ASYNC_READ };
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
    if(mode==8){
        assert(sunstone_deadline_register());
        while(!completed)assert(sunstone_deadline_wait_until(now_us()+500000u)!=SUNSTONE_DEADLINE_ERROR);
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
#include "event-sd-production.h"

static void event(void *p)
{
    ++events;
    BaseType_t woken=pdFALSE;
    if(mode==1){atomic_store_explicit(&ready,1,memory_order_release);vTaskNotifyGiveIndexedFromISR(test_task,ready_index,&woken);}
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
static void test(void *unused)
{
    (void)unused;test_task=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    host_timer_handler(sunstone_deadline_irq);
    if(mode==0){
        owner_task=host_task_create("sd-idle",owner_main,NULL,2);
        assert(sunstone_deadline_wait_until(now_us()+1000000u)==SUNSTONE_DEADLINE_EXPIRED);
        assert(wake_count==0&&served==0&&ready==1);
        struct request r={0};atomic_store(&loader_slot,&r);xTaskNotifyGive(owner_task);
        assert(sunstone_deadline_wait_until(now_us()+1000u)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(wake_count==1&&served==1&&r.done==1);
        printf("PASS idle SD owner: 0 unrequested wakes/s; one request causes one wake\n");
    } else if(mode==1||mode==2){
        uint64_t initial=host_switches();
        if(mode==1)host_irq_at(now_us()+123456,event,NULL);
        int result=wait_ready(now_us()+500000);
        assert(result==(mode==1));assert(now_us()==(mode==1?123456u:500000u));
        assert(host_switches()-initial<8); /* Finite tick waits exceed this without any publication. */
        puts("PASS ready publication or single absolute startup expiry");
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
        assert(host_switches()-initial<10);
        puts("PASS PMIC ready publication or one absolute startup expiry");
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
    } else if(mode==8){
        xTaskNotifyGive(test_task); /* The application's index zero remains owned by it. */
        owner=owner_task=host_task_create("sd-async",owner_main,NULL,2);
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
