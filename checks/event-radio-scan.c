/* Actual application, producer/status and public/private wait entries.
 * Device/report/entropy effects are model boundaries, not board evidence. */
#include "host_rtos.h"
#include "sunstone_app.h"
#include "sunstone_notify.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LIMIT UINT64_C(1000000)
static unsigned scenario,finished,detail,wait_calls,wait_returns,live=1;
static uint64_t finished_at;
static jmp_buf app_return;
static TaskHandle_t main_task;
static atomic_uint main_wake_pending;
static atomic_uint_fast64_t finish_by;
const unsigned os_wake_capacity=1;
TaskHandle_t os_wake_task[1];unsigned os_wake_gave[1];
enum {OS_SERVICE_APPLICATION};
static struct {uint64_t window_us;} os_services[1]={{1000000}};
static struct {struct {uint64_t deadline_us;} session;} board={.session={3000000}};
static int board_service_blocked_until(unsigned service,uint64_t until)
{assert(service==OS_SERVICE_APPLICATION&&until<=board.session.deadline_us);return 1;}
static uint64_t a733_smp_counter(void){return host_time_us()*24;}
unsigned sunstone_deadline_notify_index(void){return SS_NOTIFY_DEADLINE;}
_Noreturn void ss_fail(uint32_t code)
{detail=code;finished_at=host_time_us();finished=1;longjmp(app_return,1);}
#define ss_wait_until actual_ss_wait_until
#define ss_usb_wait actual_ss_usb_wait
#include "event-radio-runtime.h"
#undef ss_wait_until
#undef ss_usb_wait

struct a733_radio_report {struct {uint32_t stage;} head;};
struct board_radio {
    struct {unsigned entropy_only;} config;
    struct {uint32_t fail_line,fail_stage;} platform,session;
};
static struct board_radio radio;
static struct a733_radio_report report;
static unsigned entropy_done,entropy_failed,quiescing,publications,faults;
static const struct a733_radio_report *board_radio_report(struct board_radio *r)
{assert(r==&radio);return &report;}
static void publish(struct board_radio *r){assert(r==&radio);++publications;}
static int os_radio_quiesce_requested(void){return quiescing;}
static int os_radio_quiesce_fault(uint32_t value,const void *here)
{(void)value;(void)here;++faults;return 1;}
static void board_fail_note(uint32_t site,uint32_t value,const void *here)
{(void)site;(void)value;(void)here;assert(0);}
static void os_ce_probe_start(void){}
static void os_ce_probe_step(void){}
static int os_ce_probe_done(void){return entropy_done;}
static int os_ce_probe_failed(void){return entropy_failed;}
#define SUNSTONE_CRUMB_FAIL_SITE(group) 0u
#define SUNSTONE_CRUMB_FAIL_RETURN 1u
#include "event-radio-producer.h"

static struct ss_radio_status output;
int ss_live(void){return live?1:-1;}
void ss_progress(void){}
uint64_t ss_counter_ticks(void){return a733_smp_counter();}
static void inject(void)
{
    if(!wait_calls&&scenario==2){report.head.stage=7;os_radio_scan_step(&radio,BOARD_RADIO_SCANNED);}
    if(!wait_calls&&(scenario==5||scenario==6))host_advance_us(250000);
    if(!wait_calls&&scenario==13)host_advance_us(LIMIT+1);
}
void ss_wait_until(uint64_t until)
{inject();++wait_calls;actual_ss_wait_until(until);++wait_returns;}
void ss_usb_wait(uint32_t relative)
{inject();++wait_calls;actual_ss_usb_wait(relative);++wait_returns;}
_Noreturn void ss_finish(const void *data,size_t bytes)
{assert(bytes==16);memcpy(&output,data,bytes);finished_at=host_time_us();finished=1;longjmp(app_return,1);}
void radio_scan_main(const struct ss_grant *);
static void app_entry(void *unused)
{
    (void)unused;main_task=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    atomic_store(&finish_by,scenario==11?0:LIMIT);
    if(!setjmp(app_return)){
        if(scenario==6)ss_usb_wait((uint32_t)LIMIT);
        else if(scenario==7)(void)application_wait_until(LIMIT+250000);
        else if(scenario==8)actual_ss_wait_until(100000);
        else if(scenario==10)ss_usb_wait(1000);
        else if(scenario==11)actual_ss_wait_until(LIMIT+250000);
        else if(scenario==12)actual_ss_wait_until(LIMIT+1000);
        else if(scenario==14)actual_ss_wait_until(LIMIT+250000);
        else radio_scan_main(&(struct ss_grant){.finish_by_us=LIMIT});
        finished_at=host_time_us();finished=1;
    }
    for(;;)vTaskSuspend(NULL);
}
static void armed_at(uint64_t expected)
{
    struct sunstone_deadline_record record;
    assert(sunstone_deadline_owner_record(main_task,&record));
    assert(record.armed&&record.deadline_us==expected);
}
static void producer_cases(void)
{
    main_task=xTaskGetCurrentTaskHandle();
    for(unsigned i=0;i<10;++i)os_radio_scan_step(&radio,BOARD_RADIO_PENDING);
    assert(!atomic_load(&main_wake_pending));
    report.head.stage=7;os_radio_scan_step(&radio,BOARD_RADIO_SCANNED);
    struct ss_radio_status status;ss_radio_status(&status);
    assert(status.stage==7&&status.done&&!status.failed&&atomic_exchange(&main_wake_pending,0));
    os_radio_scan_step(&radio,BOARD_RADIO_SCANNED);assert(!atomic_load(&main_wake_pending));
    os_radio_scan_step(&radio,BOARD_RADIO_DOWN);
    assert(publications==1&&atomic_exchange(&main_wake_pending,0));
    os_radio_scan_step(&radio,BOARD_RADIO_DOWN);assert(!atomic_load(&main_wake_pending));
    atomic_store(&state,RUNNING);radio.platform.fail_line=3;
    os_radio_scan_step(&radio,BOARD_RADIO_RETAINED);ss_radio_status(&status);
    assert(status.failed&&status.done&&faults==1&&atomic_exchange(&main_wake_pending,0));
    os_radio_scan_step(&radio,BOARD_RADIO_RETAINED);assert(!atomic_load(&main_wake_pending)&&faults==1);
    radio.config.entropy_only=1;entropy_done=1;
    for(unsigned failed=0;failed<2;++failed){
        atomic_store(&state,RUNNING);entropy_failed=failed;
        os_radio_scan_step(&radio,BOARD_RADIO_PENDING);ss_radio_status(&status);
        assert(status.done&&status.failed==failed&&atomic_exchange(&main_wake_pending,0));
        os_radio_scan_step(&radio,BOARD_RADIO_PENDING);assert(!atomic_load(&main_wake_pending));
    }
    atomic_store(&state,RUNNING);quiescing=1;
    os_radio_scan_step(&radio,BOARD_RADIO_INVALID);ss_radio_status(&status);
    assert(status.done&&status.failed&&atomic_exchange(&main_wake_pending,0));
    os_radio_scan_step(&radio,BOARD_RADIO_INVALID);assert(!atomic_load(&main_wake_pending));
}
static void test(void *unused)
{
    (void)unused;
    host_timer_handler(sunstone_deadline_irq);
    if(scenario==9){producer_cases();puts("PASS all changed scan/entropy/failure/down states publish once");return;}
    if(scenario==4){host_advance_us(LIMIT);report.head.stage=7;atomic_store(&state,SCANNED);}
    if(scenario==10||scenario==12)host_advance_us(LIMIT);
    (void)host_task_create("scan-app",app_entry,NULL,3);
    if(scenario==2){assert(finished&&!detail&&finished_at==0&&wait_calls==1);return;}
    if(scenario==4){assert(finished&&detail==2&&!wait_calls);return;}
    if(scenario==12){assert(finished&&detail==0x757&&finished_at==LIMIT&&!wait_calls);return;}
    if(scenario==13){assert(finished&&detail==0x757&&finished_at==LIMIT+1&&wait_calls==1);return;}
    assert(!finished&&wait_calls==(scenario==7||scenario==8||scenario==11||scenario==14?0u:1u));
    if(scenario==6||scenario==7||scenario==11){armed_at(LIMIT+250000);host_advance_us(LIMIT+250000-host_time_us());assert(finished&&!detail&&finished_at==LIMIT+250000);return;}
    if(scenario==10){armed_at(LIMIT+1000);assert(wait_calls==1&&!finished);host_advance_us(1000);assert(finished&&!detail&&wait_returns==1&&finished_at==LIMIT+1000);return;}
    if(scenario==8){armed_at(100000);host_advance_us(100000);assert(finished&&!detail&&finished_at==100000);return;}
    armed_at(LIMIT);
    if(scenario==0){
        for(unsigned i=0;i<10;++i){report.head.stage=i;os_radio_scan_step(&radio,BOARD_RADIO_PENDING);}
        host_advance_us(250000);assert(!finished&&wait_calls==1&&!wait_returns);
    }else if(scenario==1)host_advance_us(123);
    if(scenario==15){live=0;os_application_wake_main();assert(finished&&detail==1);return;}
    if(scenario==0||scenario==1){
        report.head.stage=7;os_radio_scan_step(&radio,BOARD_RADIO_SCANNED);
        assert(finished&&!detail&&output.stage==7&&output.done&&!output.failed);
        assert(finished_at==(scenario==1?123u:250000u)&&wait_returns==1);
        os_radio_scan_step(&radio,BOARD_RADIO_SCANNED);assert(!atomic_load(&main_wake_pending));
    }else{
        if(scenario==3){
            host_advance_us(250000);os_application_wake_main();armed_at(LIMIT);
            host_advance_us(250000);os_application_wake_main();armed_at(LIMIT);
            assert(wait_calls==3&&wait_returns==2);
        }
        host_advance_us(LIMIT-host_time_us());
        if(!(finished&&detail==0x757&&finished_at==LIMIT))
            fprintf(stderr,"expiry detail=%x finished=%u at=%llu now=%llu waits=%u returns=%u\n",
                detail,finished,(unsigned long long)finished_at,(unsigned long long)host_time_us(),wait_calls,wait_returns);
        assert(finished&&detail==0x757&&finished_at==LIMIT);
    }
}
int main(int argc,char **argv)
{assert(argc==2);scenario=(unsigned)strtoul(argv[1],NULL,10);host_rtos_run(test,NULL,2,2000000);puts("PASS radio/public-wait case");return 0;}
