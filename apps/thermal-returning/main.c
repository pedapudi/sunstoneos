#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_range.h"
#include "a733_platform_locks.h"
#include "a733_adc_native.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"

/* This fixed, supervised image requires coordinator admission of supplies,
 * security, exclusive THS/GPADC24M and no remote writers. A runtime token
 * records that admitted ownership; it cannot establish electrical facts. */
static StaticTask_t task;
static StackType_t stack[4096];
static TaskHandle_t owner;
static struct a733_counter_clock clock;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_clock_token token;
static struct a733_adc_native binding;
static struct a733_thermal thermal;
static struct a733_thermal_config config;
static struct a733_thermal_ops operations;
static struct a733_thermal_sample sets[2];
static uint64_t deadline,sample_deadline;
static unsigned step,completed,started;
static void report(const char *name,uint64_t value)
{platform_puts(name);platform_hex(value);platform_puts("\n");}
static int now(void *unused,uint64_t *out)
{(void)unused;return a733_counter_clock_now(&clock,out);}
static int quiet(void)
{
    /* Both analog consumers are excluded before any clock write. Never clear
     * an inherited pending event or alter distributor configuration. */
    return !(mmio_read(0x03400000)&(1u<<31))&&
        !((mmio_read(0x03400108)|mmio_read(0x03400208)|mmio_read(0x03400308))&0xc0);
}
static int current(void)
{return binding.operations.current(&binding)==1;}
static int enter(void *unused,uint64_t end)
{
    (void)unused;
    int held=binding.operations.enter(&binding,end);
    /* A negative result tells the driver to retain an actually held mutex. */
    return held==1?(quiet()?1:-1):held;
}
static void leave(void *unused)
{(void)unused;binding.operations.leave(&binding);}
static void retain(void *unused)
{(void)unused;binding.operations.retain(&binding);}
static int accessible(void)
{return current()&&quiet()&&thermal.held&&
    !(atomic_load_explicit(&locks.retained,memory_order_acquire)&1)&&
    xSemaphoreGetMutexHolder(locks.mutex[A733_PLATFORM_REGISTER_LOCK])==owner;}
static int read_reg(void *unused,uintptr_t a,uint32_t *out)
{(void)unused;return accessible()&&a733_thermal_native_read(NULL,a,out)==1;}
static int write_reg(void *unused,uintptr_t a,uint32_t value)
{(void)unused;return accessible()&&a733_thermal_native_write(NULL,a,value)==1;}
static int unused_admit(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{(void)v;(void)c;(void)t;return 0;} /* Binding only; no GPADC driver instance. */
static int admit(void *unused,const struct a733_thermal_config *c,const struct a733_clock_token *t)
{
    (void)unused;
    return current()&&quiet()&&c==&thermal.config&&c->owner==config.owner&&
        c->handoff==config.handoff&&c->startup_deadline_us==config.startup_deadline_us&&
        c->apb1==config.apb1&&c->apb1_hz==config.apb1_hz&&
        c->configure_adc_clock==config.configure_adc_clock&&!c->alarm_mask&&!c->critical_mask&&
        t->id==token.id&&t->owner==token.owner&&t->shared==token.shared&&t->exclusive==token.exclusive;
}
static _Noreturn void failed(const char *reason)
{
    platform_puts("THERMAL_REASON=");platform_puts(reason);platform_puts("\n");
    report("THERMAL_STEP=",step);report("THERMAL_STAGE=",thermal.stage);
    report("THERMAL_FAULT=",thermal.fault);report("THERMAL_SETS=",completed);
    report("THERMAL_LOCK_RETAINED=",atomic_load_explicit(&locks.retained,memory_order_acquire));
    /* No synthetic stop or unlock after driver uncertainty. The monitor can
     * return the CPU while analog ownership remains retained for recovery. */
    platform_puts("THERMAL_OUTCOME=INCOMPLETE_RETAINED\n");platform_exit(1);
}
#define REQUIRE(v,r) do{if(!(v))failed(r);}while(0)
static int before(uint64_t end)
{uint64_t value;return now(NULL,&value)&&value<end;}
static void pause(uint64_t end)
{REQUIRE(before(end),"deadline");vTaskDelay(1);}
static void stop(void)
{
    enum a733_thermal_result r;
    do{r=a733_thermal_stop(&thermal,deadline);
        REQUIRE(r==A733_THERMAL_BUSY||r==A733_THERMAL_STOPPED,"configured disable");
        REQUIRE(!thermal.fault,"disable uncertainty");
        if(r!=A733_THERMAL_STOPPED)pause(deadline);
    }while(r!=A733_THERMAL_STOPPED);
    REQUIRE(quiet()&&!thermal.held&&!xSemaphoreGetMutexHolder(locks.mutex[0])&&
        !atomic_load_explicit(&locks.retained,memory_order_acquire),"disable completion");
    /* No reset, gate removal, lease release or analog-drain claim follows. */
    report("THERMAL_CONFIGURED_DISABLED=",1);
}
static void probe(void *unused)
{
    (void)unused;platform_puts("BEGIN native calibrated thermal returning\n");
    REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start;REQUIRE(now(NULL,&start)&&a733_deadline_after(start,1500000,&deadline),"counter sample");
    sample_deadline=start+1200000;
    REQUIRE(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock),"locks init");
    REQUIRE(a733_clock_leases_init(&clocks,a733_counter_clock_now,&clock),"clock registry");
    owner=xTaskGetCurrentTaskHandle();
    REQUIRE(a733_adc_native_init(&binding,&locks,owner,unused_admit,NULL),"native binding");
    REQUIRE(current()&&quiet(),"owner/IRQ70/71 quiescence");
    step=1;
    uint32_t apb1=mmio_read(0x02002518)&0x0300001f;
    unsigned parent=apb1>>24,div=(apb1&31)+1;
    uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
    REQUIRE(hz&&!(hz%div),"APB1 selection");
    a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC;
    if(parent==3)shared|=A733_CLOCK_PERI0;
    REQUIRE(a733_clock_acquire(&clocks,1,shared,A733_CLOCK_THS|A733_CLOCK_GPADC24M,
        deadline,&token)==A733_CLOCK_OK,"exclusive analog lease");
    uint32_t source=mmio_read(0x02002fc0);
    config=(struct a733_thermal_config){.owner=1,.handoff=1,.startup_deadline_us=sample_deadline,
        .apb1=apb1,.apb1_hz=hz/div,.configure_adc_clock=!(source&(1u<<31))};
    operations=(struct a733_thermal_ops){now,admit,enter,leave,retain,read_reg,write_reg};
    REQUIRE(a733_thermal_init(&thermal,&config,&operations,NULL,&clocks,&token),"thermal init");
    report("THERMAL_APB1=",apb1);report("THERMAL_APB1_HZ=",hz/div);
    report("THERMAL_INITIAL_PARENT=",source);report("THERMAL_COLD_PARENT=",config.configure_adc_clock);
    step=2;
    enum a733_thermal_result r;
    do{r=a733_thermal_poll(&thermal);
        REQUIRE(r==A733_THERMAL_PENDING||r==A733_THERMAL_BUSY||r==A733_THERMAL_READY,"thermal startup");
        REQUIRE(!thermal.fault,"startup uncertainty");
        if(r!=A733_THERMAL_READY)pause(sample_deadline);
    }while(r!=A733_THERMAL_READY);
    started=1;step=3;
    struct a733_thermal_sample aggregate={0};
    while(completed<2&&before(sample_deadline-100000)){
        struct a733_thermal_sample sample;
        r=a733_thermal_read(&thermal,sample_deadline,&sample);
        REQUIRE(r==A733_THERMAL_BUSY||r==A733_THERMAL_AGAIN||r==A733_THERMAL_SAMPLE,"thermal read");
        REQUIRE(!thermal.fault,"read uncertainty");
        if(r==A733_THERMAL_SAMPLE){
            REQUIRE(!sample.alarm&&!sample.alarm_off&&!sample.critical,"unexpected thermal events");
            for(unsigned i=0;i<5;++i)if(sample.valid&(1u<<i)){
                aggregate.code[i]=sample.code[i];aggregate.millicelsius[i]=sample.millicelsius[i];
            }
            aggregate.valid|=sample.valid;aggregate.observed_us=sample.observed_us;aggregate.sequence=sample.sequence;
            if(aggregate.valid==31){
                if(completed)REQUIRE(aggregate.sequence>sets[0].sequence&&aggregate.observed_us>sets[0].observed_us,"fresh second set");
                sets[completed++]=aggregate;aggregate=(struct a733_thermal_sample){0};
            }
        }
        if(completed<2&&before(sample_deadline-100000))vTaskDelay(1);
    }
    step=4;stop();
    REQUIRE(started&&before(deadline),"publication deadline");
    report("THERMAL_FACTORY_TENTH_C=",thermal.calibration.factory_tenth_c);
    for(unsigned i=0;i<5;++i){report("THERMAL_FACTORY_CODE=",thermal.calibration.factory_code[i]);report("THERMAL_CORRECTION=",thermal.calibration.correction[i]);}
    for(unsigned n=0;n<completed;++n){
        report("THERMAL_SET=",n);report("THERMAL_OBSERVED_US=",sets[n].observed_us);report("THERMAL_SEQUENCE=",sets[n].sequence);
        for(unsigned i=0;i<5;++i){report("THERMAL_CHANNEL=",i);report("THERMAL_CODE=",sets[n].code[i]);
            /* Two's-complement int64 field preserves negative temperatures. */
            report("THERMAL_MILLICELSIUS=",(uint64_t)(int64_t)sets[n].millicelsius[i]);}
    }
    report("THERMAL_SETS=",completed);
    report("THERMAL_OUTCOME=",completed==2?0:1);
    platform_puts("THERMAL_RESOURCES=RETAINED_CLOCKS_LEASES_CONFIGURATION\n");
    if(completed==2)platform_puts("PASS native calibrated thermal observations\n");
    platform_puts("COMPLETE native thermal configured disable\n");platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{(void)handle;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe,"thermal-read",4096,NULL,3,stack,&task));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
