#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "a733_rtc_application.h"
#include "monitor.h"

static StaticTask_t task;
static StackType_t stack[2048];
static struct a733_counter_clock clock;
static struct a733_rtc before;
static struct a733_rtc_initialization initialized;
static uint64_t deadline;
static unsigned writes,invariant,step;
static void report(const char *name,uint64_t value)
{platform_puts(name);platform_hex(value);platform_puts("\n");}
static _Noreturn void done(unsigned outcome,const char *reason)
{
    report("RTC_SOURCE_OUTCOME=",outcome);platform_puts("RTC_SOURCE_REASON=");platform_puts(reason);platform_puts("\n");
    report("RTC_SOURCE_EL3_ADMISSION=",rtc_source_admission.ready);report("RTC_SOURCE_EL3_REASON=",rtc_source_admission.reason);
    report("RTC_SOURCE_ORIGINAL_IRQ_ENABLE=",rtc_source_admission.enabled);report("RTC_SOURCE_ORIGINAL_IRQ_PENDING=",rtc_source_admission.pending);report("RTC_SOURCE_ORIGINAL_IRQ_ACTIVE=",rtc_source_admission.active);
    report("RTC_SOURCE_STEP=",step);report("RTC_SOURCE_WRITES=",writes);
    report("RTC_SOURCE_STAGE=",initialized.stage);
    report("RTC_SOURCE_BEFORE_CONTROL=",initialized.before_control);report("RTC_SOURCE_BEFORE_STATUS=",initialized.before_status);
    report("RTC_SOURCE_BEFORE_DAY=",initialized.before_day);report("RTC_SOURCE_BEFORE_HMS=",initialized.before_hms);
    report("RTC_SOURCE_AFTER_CONTROL=",initialized.after_control);report("RTC_SOURCE_AFTER_STATUS=",initialized.after_status);
    report("RTC_SOURCE_AFTER_DAY=",initialized.after_day);report("RTC_SOURCE_AFTER_HMS=",initialized.after_hms);
    if(outcome==2||invariant){platform_puts("FAIL native RTC source retained\n");platform_exit(1);}
    platform_puts("COMPLETE native RTC source result\n");platform_exit(0);
}
static _Noreturn void failed(const char *reason){done(writes||invariant?2:1,reason);}
#define REQUIRE(v,r) do{if(!(v))failed(r);}while(0)
static int now(void *c,uint64_t *value)
{return c==&initialized&&a733_counter_clock_now(&clock,value)==1;}
static int timely(void){uint64_t value;return now(&initialized,&value)&&value<deadline;}
static int quiet(void)
{return !(mmio_read(0x03400000)&(1u<<31))&&
 !((mmio_read(0x0340011c)|mmio_read(0x0340021c)|mmio_read(0x0340031c))&16);}
static int read_reg(void *c,uint32_t offset,uint32_t *value)
{
    if(c!=&initialized||!value||(offset>0x50&&offset!=0x160)||(offset&3)||!timely()||!quiet())return 0;
    *value=a733_mmio_read32_dsb_sy_after(0x07090000+offset);return timely();
}
static int write_reg(void *c,uint32_t offset,uint32_t value)
{
    if(c!=&initialized||!timely()||!quiet())return 0;
    if((offset!=0&&offset!=4)||(offset==0&&(value>>16)!=0x16aa)||(offset==4&&value!=2)){
        invariant=1;return 0;
    }
    /* The SOURCE-only helper may change mux/gate/switch controls, never gain,
     * calendar, RC prescaler/calibration, DCXO or wake configuration. */
    if(offset==0&&((value^mmio_read(0x07090000))&0xffff&~UINT32_C(0xc013))){invariant=1;return 0;}
    ++writes;a733_mmio_write32_dsb_sy_after(0x07090000+offset,value);return timely();
}
static void probe(void *unused)
{
    (void)unused;REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start;REQUIRE(now(&initialized,&start)&&a733_deadline_after(start,1500000,&deadline),"counter sample");
    step=1;REQUIRE(rtc_source_admission.ready,"original IRQ228 not admitted by EL3");
    REQUIRE(timely()&&(mmio_read(0x0701020c)&0x10001)==0x10001,"RTC bus unavailable");
    REQUIRE(quiet(),"IRQ228 not quiescent");
    const struct a733_rtc_ops ops={now,read_reg,write_reg};
    REQUIRE(a733_rtc_open_readonly(&before,&ops,&initialized,deadline),"inherited calendar/source unavailable");
    step=2;const struct a733_rtc_setting setting={A733_RTC_INIT_SOURCE,0,0,0};
    REQUIRE(a733_rtc_initialize(&setting,&ops,&initialized,deadline,&initialized),"source initialization incomplete");
    REQUIRE(initialized.stage==6&&timely()&&quiet()&&!invariant,"source completion verification");
    step=3;done(0,"external source selected; coherent calendar preserved; advancement unmeasured");
}
void vApplicationStackOverflowHook(TaskHandle_t h,char *name){(void)h;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();configASSERT(xTaskCreateStatic(probe,"rtc-source",2048,NULL,3,stack,&task));
    vTaskStartScheduler();platform_panic("source scheduler returned",0);
}
