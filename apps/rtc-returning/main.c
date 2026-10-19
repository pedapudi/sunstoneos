#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_rtc.h"

/* One finite owner. The coordinator admits exclusive RTC/source/calendar
 * ownership; this client neither changes that state nor takes over USB. */
static StaticTask_t task;
static StackType_t stack[2048];
static struct a733_counter_clock clock;
static struct a733_rtc rtc;
static uint64_t deadline;
static unsigned step;
static void report(const char *name,uint64_t value)
{platform_puts(name);platform_hex(value);platform_puts("\n");}
static _Noreturn void failed(const char *name)
{
    platform_puts("INCOMPLETE RTC read: ");platform_puts(name);platform_puts("\n");
    report("step=",step);report("driver state=",rtc.state);platform_exit(1);
}
#define REQUIRE(value,name) do {if(!(value))failed(name);} while(0)
static int now(void *context,uint64_t *value)
{return context==&rtc&&a733_counter_clock_now(&clock,value)==1;}
static int timely(void)
{uint64_t value;return now(&rtc,&value)&&value<deadline;}
static int quiet(void)
{
    /* INTID228: distributor bank7, bit4. Never acknowledge inherited events. */
    return !(mmio_read(0x03400000)&(1u<<31))&&
        !((mmio_read(0x0340011c)|mmio_read(0x0340021c)|mmio_read(0x0340031c))&16);
}
static int read_reg(void *context,uint32_t offset,uint32_t *value)
{
    if(context!=&rtc||!value||offset>0x50||(offset&3)||!timely()||!quiet())return 0;
    *value=mmio_read(0x07090000+offset);
    return timely();
}
static int write_reg(void *context,uint32_t offset,uint32_t value)
{(void)context;(void)offset;(void)value;return 0;}
static uint64_t seconds(const struct a733_rtc_time *time)
{return (uint64_t)time->day*86400+time->hour*3600+time->minute*60+time->second;}
static void reading(const char *name,const struct a733_rtc_time *time)
{
    platform_puts(name);platform_puts("\n");
    report("day=",time->day);report("hour=",time->hour);
    report("minute=",time->minute);report("second=",time->second);
    report("year=",time->year);report("month=",time->month);report("date=",time->date);
}
static void probe(void *unused)
{
    (void)unused;
    REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start,first_counter,second_counter;
    REQUIRE(now(&rtc,&start)&&start<=UINT64_MAX-1500000,"counter sample");
    deadline=start+1500000;
    step=1;
    /* Gate status is accessible independently of the RTC aperture. Never
     * release reset, enable the gate, or read a gated RTC block. */
    REQUIRE(timely(),"preflight deadline");
    uint32_t bus=mmio_read(0x0701020c);
    REQUIRE((bus&0x10001)==0x10001,"RTC bus unavailable");
    REQUIRE(timely()&&quiet(),"IRQ228 not quiescent");
    step=2;
    const struct a733_rtc_ops ops={now,read_reg,write_reg};
    REQUIRE(a733_rtc_open(&rtc,&ops,&rtc,deadline),"source/calendar/event adoption");
    struct a733_rtc_time first,second;
    REQUIRE(a733_rtc_read(&rtc,&first)&&now(&rtc,&first_counter),"first coherent read");
    step=3;
    uint64_t value;
    do {
        REQUIRE(now(&rtc,&value)&&value<deadline,"wait deadline");
        if(value-first_counter>=1100000)break;
        /* Keep scheduler interrupts and secure monitor recovery live. */
        vTaskDelay(1);
    }while(1);
    step=4;
    REQUIRE(a733_rtc_read(&rtc,&second)&&now(&rtc,&second_counter),"second coherent read");
    REQUIRE(second_counter<deadline&&quiet(),"completion deadline/IRQ");
    uint64_t before=seconds(&first),after=seconds(&second);
    REQUIRE(after>=before&&(after-before==1||after-before==2),"calendar progression");
    report("RTC bus=",bus);report("source control=",rtc.snapshot.clock_control);
    report("source status=",rtc.snapshot.clock_status);
    report("saved alarm day=",rtc.snapshot.saved_day);report("saved alarm HMS=",rtc.snapshot.saved_hms);
    report("first counter us=",first_counter);reading("first",&first);
    report("second counter us=",second_counter);reading("second",&second);
    report("calendar advance seconds=",after-before);
    REQUIRE(timely()&&quiet(),"publication deadline/IRQ");
    platform_puts("PASS native RTC coherent read progression\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{(void)handle;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe,"rtc-read",2048,NULL,3,stack,&task));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
