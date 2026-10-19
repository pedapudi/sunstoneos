#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_rtc.h"
#include "gicv3_spi.h"
#include "monitor.h"

static StaticTask_t task;
static StackType_t stack[2048];
static struct a733_counter_clock clock;
static struct a733_rtc rtc;
static const struct gicv3_spi spi={0x03400000,228};
static atomic_uint armed,pending,irq_count,irq_fault;
static uint64_t deadline;
static unsigned writes,invariant,step,phase,clean;
extern void __real_vApplicationFPUSafeIRQHandler(uint64_t);
static void report(const char *name,uint64_t value)
{platform_puts(name);platform_hex(value);platform_puts("\n");}
static int now(void *c,uint64_t *value){return c==&rtc&&a733_counter_clock_now(&clock,value)==1;}
static int timely(void){uint64_t value;return now(&rtc,&value)&&value<deadline;}
static int quiet(unsigned check_pending)
{
    if(mmio_read(0x03400000)&(1u<<31))return 0;
    if((mmio_read(0x0340011c)|mmio_read(0x0340031c))&16)return 0;
    return !check_pending||!(mmio_read(0x0340021c)&16);
}
static _Noreturn void done(unsigned outcome,const char *reason)
{
    report("RTC_ALARM_OUTCOME=",outcome);platform_puts("RTC_ALARM_REASON=");platform_puts(reason);platform_puts("\n");
    report("RTC_ALARM_STEP=",step);report("RTC_ALARM_WRITES=",writes);report("RTC_ALARM_STATE=",rtc.state);
    report("RTC_ALARM_IRQ_COUNT=",atomic_load_explicit(&irq_count,memory_order_acquire));
    report("RTC_ALARM_SEEN=",rtc.snapshot.alarm_seen);report("RTC_ALARM_CANCELLED=",rtc.snapshot.cancelled);
    report("RTC_ALARM_TARGET_DAY=",rtc.snapshot.target_day);report("RTC_ALARM_TARGET_HMS=",rtc.snapshot.target_hms);
    report("RTC_ALARM_TERMINAL_DAY=",rtc.snapshot.terminal.day);
    report("RTC_ALARM_TERMINAL_HMS=",(rtc.snapshot.terminal.hour<<16)|(rtc.snapshot.terminal.minute<<8)|rtc.snapshot.terminal.second);
    report("RTC_ALARM_ROUTE_REASON=",rtc_alarm_route.reason);report("RTC_ALARM_CLEAN=",clean);
    if(invariant||rtc_alarm_route.retained||(writes&&!clean)){
        platform_puts("FAIL native RTC alarm retained\n");platform_exit(1);
    }
    platform_puts("COMPLETE native RTC alarm result\n");platform_exit(0);
}
static _Noreturn void failed(const char *reason){done(1,reason);}
#define REQUIRE(v,r) do{if(!(v))failed(r);}while(0)
static int read_reg(void *c,uint32_t offset,uint32_t *value)
{
    if(c!=&rtc||!value||offset>0x50||(offset&3)||!timely()||!quiet(phase!=2))return 0;
    *value=mmio_read(0x07090000+offset);__asm__ volatile("dsb sy":::"memory");return timely();
}
static int write_reg(void *c,uint32_t offset,uint32_t value)
{
    if(c!=&rtc||!timely()||!quiet(phase!=2))return 0;
    if((offset!=0x20&&offset!=0x24&&offset!=0x28&&offset!=0x2c&&offset!=0x30)||(offset==0x30&&value!=1)){
        invariant=1;return 0;
    }
    ++writes;*(volatile uint32_t *)(uintptr_t)(0x07090000+offset)=value;
    __asm__ volatile("dsb sy":::"memory");return timely();
}
void __wrap_vApplicationFPUSafeIRQHandler(uint64_t interrupt)
{
    if(interrupt!=228){__real_vApplicationFPUSafeIRQHandler(interrupt);return;}
    /* Existing FreeRTOS wrapper owns IAR/EOIR and saved FP/context. RTC MMIO
     * stays with the owner task, after the port has deactivated this IRQ. */
    if(!gicv3_spi_mask(&spi)||!atomic_load_explicit(&armed,memory_order_acquire)||counter()/24>=deadline)
        atomic_store_explicit(&irq_fault,1,memory_order_release);
    atomic_fetch_add_explicit(&irq_count,1,memory_order_relaxed);
    atomic_store_explicit(&pending,1,memory_order_release);
}
static void probe(void *unused)
{
    (void)unused;REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start,value;REQUIRE(now(&rtc,&start)&&start<=UINT64_MAX-1500000,"counter sample");deadline=start+1500000;
    step=1;REQUIRE(rtc_alarm_route.ready&&!rtc_alarm_route.retained,"EL3 route not admitted");
    REQUIRE((mmio_read(0x0701020c)&0x10001)==0x10001&&timely()&&quiet(1),"RTC bus/IRQ preflight");
    const struct a733_rtc_ops ops={now,read_reg,write_reg};
    REQUIRE(a733_rtc_open(&rtc,&ops,&rtc,deadline),"strict external source/calendar adoption");
    REQUIRE(now(&rtc,&value)&&value-start<100000,"setup budget");
    step=2;REQUIRE(a733_rtc_arm(&rtc,1),"alarm arm");
    atomic_store_explicit(&armed,1,memory_order_release);phase=1;
    int enabled;
    do{
        REQUIRE(now(&rtc,&value)&&value-start<200000,"enable budget");taskENTER_CRITICAL();enabled=gicv3_spi_rearm(&spi);taskEXIT_CRITICAL();
        if(!enabled)vTaskDelay(1);
    }while(!enabled);
    REQUIRE(enabled==1,"IRQ228 enable");step=3;
    while(!atomic_load_explicit(&pending,memory_order_acquire)){
        REQUIRE(now(&rtc,&value)&&value<deadline,"wait deadline");
        if(value-start>=1200000)break;
        REQUIRE(!atomic_load_explicit(&irq_fault,memory_order_acquire),"IRQ fault");vTaskDelay(1);
    }
    step=4;REQUIRE(gicv3_spi_mask(&spi),"completion mask");
    while(!quiet(0)){REQUIRE(timely(),"deactivation deadline");vTaskDelay(1);}
    phase=2;
    unsigned notified=atomic_load_explicit(&pending,memory_order_acquire);
    REQUIRE(!atomic_load_explicit(&irq_fault,memory_order_acquire),"IRQ fault");
    if(notified)REQUIRE(a733_rtc_irq(&rtc),"owned RTC acknowledgment");
    REQUIRE(a733_rtc_finish(&rtc,!notified),"alarm disable/target restore");
    atomic_store_explicit(&armed,0,memory_order_release);
    while(!quiet(1)){REQUIRE(timely(),"pending drain deadline");vTaskDelay(1);}
    phase=3;REQUIRE(timely()&&!((mmio_read(0x07090028)|mmio_read(0x0709002c)|mmio_read(0x07090030)|mmio_read(0x07090050))&1),"final RTC quiescence");
    REQUIRE(mmio_read(0x07090020)==rtc.snapshot.saved_day&&mmio_read(0x07090024)==rtc.snapshot.saved_hms,"saved target readback");
    clean=1;step=5;
    if(notified&&atomic_load_explicit(&irq_count,memory_order_acquire)==1&&rtc.snapshot.alarm_seen&&!rtc.snapshot.cancelled&&rtc.state==A733_RTC_FIRED)
        done(0,"IRQ228 delivered; alarm drained; target restored");
    done(1,"clean cancellation without qualified IRQ delivery");
}
void vApplicationStackOverflowHook(TaskHandle_t h,char *name){(void)h;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();configASSERT(xTaskCreateStatic(probe,"rtc-alarm",2048,NULL,3,stack,&task));
    vTaskStartScheduler();platform_panic("alarm scheduler returned",0);
}
