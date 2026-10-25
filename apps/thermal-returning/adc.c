#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_range.h"
#include "a733_platform_locks.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"
#include "a733_adc_native.h"

/* Fixed supervised passive strap observation. Root must establish input range,
 * impedance, stable AVCC/reference, security and remote-writer exclusion. */
static StaticTask_t task;
static StackType_t stack[4096];
static struct a733_counter_clock clock;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_pins_native pin_native;
static struct a733_pins pins;
static struct a733_gpu_resource_native gpu_native;
static struct a733_gpu_resource_manager gpu;
static struct a733_rail_leases rails;
static struct a733_pmic_native native;
static struct a733_pmic provider;
static struct a733_pmic_config config;
static struct a733_pin_handoff handoff;
static struct a733_rail_token rail;
static struct a733_pmic_lifetime lifetime;
static struct a733_pmic_request request;
static struct a733_pmic_snapshot snapshot;
static uint64_t deadline;
static unsigned step;
static struct a733_adc adc;
static struct a733_adc_native adc_native;
static struct a733_adc_config adc_config;
static struct a733_clock_token adc_token;
static struct a733_adc_sample samples[2];
static unsigned sample_count;
static uint64_t sample_deadline;
static int quiet(void)
{return !(mmio_read(0x03400000)&(1u<<31))&&
 !((mmio_read(0x03400108)|mmio_read(0x03400208)|mmio_read(0x03400308))&0xc0);}
/* Pending INTID70 is expected while its owned level source signals data.
 * Keep delivery masked and refuse active or enabled IRQ70/71 throughout. */
static int owned_irqs(void)
{return !(mmio_read(0x03400000)&(1u<<31))&&
 !((mmio_read(0x03400108)|mmio_read(0x03400308))&0xc0)&&
 !(mmio_read(0x03400208)&0x80);}
static int adc_admit(void *unused,const struct a733_adc_config *c,const struct a733_clock_token *t)
{
 (void)unused;
 return quiet()&&c==&adc.config&&c->owner==adc_config.owner&&c->handoff==adc_config.handoff&&
 c->startup_deadline_us==adc_config.startup_deadline_us&&c->apb1==adc_config.apb1&&
 c->apb1_hz==adc_config.apb1_hz&&c->configure_adc_clock==adc_config.configure_adc_clock&&
 t->id==adc_token.id&&t->owner==adc_token.owner&&t->shared==adc_token.shared&&t->exclusive==adc_token.exclusive;
}

static void report(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); platform_puts("\n"); }

static _Noreturn void failed(const char *name)
{
    platform_puts("INCOMPLETE native ADC: "); platform_puts(name); platform_puts("\n");
    report("client step=",step); report("ADC_STAGE=",adc.stage); report("ADC_FAULT=",adc.fault); report("provider stage=",provider.stage);
    report("transfer phase=",provider.phase); report("transfer byte=",provider.byte_index);
    report("last status=",provider.last_status); report("provider held=",provider.held);
    report("provider retained=",provider.retained); report("provider failed=",provider.failed);
    report("locks retained=",atomic_load_explicit(&locks.retained,memory_order_acquire));
    /* A failed transfer has no synthetic STOP, lease release or close. Its
     * nonzero report cannot admit another warm operation. */
    platform_exit(1);
}
#define REQUIRE(value, name) do { if (!(value)) failed(name); } while (0)

static int now(void *unused,uint64_t *out)
{ (void)unused; return a733_counter_clock_now(&clock,out); }
static int pin_enter(void *unused,uint64_t end)
{ (void)unused; return a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,end); }
static void pin_leave(void *unused)
{ (void)unused; a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK); }
static void pause(void)
{
    uint64_t value;
    REQUIRE(now(NULL,&value) && value<deadline,"client deadline");
    vTaskDelay(1);
}
static void poll(void)
{
    enum a733_pmic_status result=a733_pmic_poll(&provider);
    REQUIRE(result==A733_PMIC_PENDING || result==A733_PMIC_BUSY ||
            result==A733_PMIC_READY || result==A733_PMIC_COMPLETE,"provider poll");
    pause();
}

static void probe(void *unused)
{
    (void)unused;
    platform_puts("BEGIN native GPADC returning\n");
    REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start;
    REQUIRE(now(NULL,&start) && a733_deadline_after(start,1500000,&deadline),"counter sample");
    sample_deadline=start+1200000;
    REQUIRE(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock),"locks init");
    REQUIRE(a733_clock_leases_init(&clocks,a733_counter_clock_now,&clock),"clock leases init");
    struct a733_pins_locks pin_locks={pin_enter,pin_leave,now,NULL};
    REQUIRE(a733_pins_native_init(&pin_native,&pin_locks),"native pins init");
    handoff.authorized[A733_PIN_L]=3;
    REQUIRE(a733_pins_init_banks(&pins,&pin_native.operations,&pin_native,&handoff),"pins init");
    REQUIRE(a733_gpu_resources_native_init(&gpu_native,&locks.callbacks),"GPU metadata adapter init");
    gpu_native.operations.now=now;
    REQUIRE(a733_gpu_resources_init(&gpu,&gpu_native.operations,&gpu_native),"GPU metadata init");
    struct a733_rail_binding binding={&clocks,&gpu,1,a733_counter_clock_now,&clock};
    REQUIRE(a733_rail_leases_init(&rails,&binding),"rail ledger init");
    REQUIRE(a733_pmic_native_init(&native,&locks,xTaskGetCurrentTaskHandle()),"native PMIC init");
    config.owner=2; config.identity=1; config.startup_deadline=deadline;
    const uintptr_t observed[]={0x07025000,0x07025014,0x07025024,0x0708300c,
        0x07083010,0x07083014,0x07083020};
    uint32_t before[sizeof(observed)/sizeof(observed[0])];
    for(unsigned i=0;i<sizeof(before)/sizeof(before[0]);++i) {
        before[i]=mmio_read(observed[i]); report("inherited address=",observed[i]);
        report("inherited value=",before[i]);
    }
    REQUIRE((before[0]&0xff)==0x22,"PL0/PL1 function");
    for(unsigned i=0;i<2;++i)
        config.pins[i]=(struct a733_pin_config){2,(before[2]>>(2*i))&3,(before[1]>>(4*i))&15};
    REQUIRE(a733_pmic_native_bind(&provider,&config,&native,&clocks,&rails,&pins),"PMIC init");
    REQUIRE(a733_gpu_resources_native_use_pmic(&gpu_native,&gpu,&provider),"PMIC metadata bind");
    REQUIRE(a733_pmic_identity(&provider,&lifetime)==A733_PMIC_READY,"PMIC identity");
    step=1;
    unsigned previous=UINT32_MAX;
    while(a733_pmic_live(&provider,&lifetime)!=A733_PMIC_READY) {
        if(provider.stage!=previous) { previous=provider.stage; report("startup stage=",previous); }
        poll();
    }
    platform_puts("READY native PMIC provider\n");
    step=2;
    REQUIRE(a733_rail_acquire(&rails,3,A733_RAIL_BIT(A733_RAIL_CLDO3),0,deadline,&rail)==A733_RAIL_PENDING,"rail acquire");
    enum a733_rail_result rr;
    do { rr=a733_rail_poll(&rails,&rail); REQUIRE(rr==A733_RAIL_PENDING || rr==A733_RAIL_BUSY || rr==A733_RAIL_READY,"rail poll"); if(rr!=A733_RAIL_READY) pause(); } while(rr!=A733_RAIL_READY);
    REQUIRE(a733_pmic_submit(&provider,3,&rail,A733_RAIL_CLDO3,deadline,&request)==A733_PMIC_PENDING,"snapshot submit");
    step=3;
    enum a733_pmic_status result;
    do {
        result=a733_pmic_result(&provider,&request,&snapshot);
        REQUIRE(result==A733_PMIC_PENDING || result==A733_PMIC_BUSY || result==A733_PMIC_COMPLETE,"snapshot result");
        if(result!=A733_PMIC_COMPLETE) poll();
    } while(result!=A733_PMIC_COMPLETE);
    report("page before=",snapshot.page_before); report("chip=",snapshot.chip);
    report("revision=",snapshot.revision); report("enable=",snapshot.enable);
    report("selector=",snapshot.selector); report("page after=",snapshot.page_after);
    report("decoded=",snapshot.decoded); report("enabled=",snapshot.enabled);
    report("programmed microvolts=",snapshot.programmed_uv);
    REQUIRE(snapshot.rail==A733_RAIL_CLDO3 && !snapshot.page_before && !snapshot.page_after &&
            snapshot.chip==3 && !snapshot.revision && snapshot.decoded && snapshot.enabled,"snapshot identity/decode");
    REQUIRE(snapshot.programmed_uv==1800000&&snapshot.selector==13,"CLDO3 nominal 1.8V profile");
    REQUIRE(a733_pmic_reap(&provider,&request)==A733_PMIC_COMPLETE,"snapshot reap");
    /* The actual rail token remains live while the converter samples. */
    step=4;
    REQUIRE(quiet(),"IRQ70/71 quiescence");
    uint32_t apb1=mmio_read(0x02002518)&0x0300001f;
    unsigned parent=apb1>>24,div=(apb1&31)+1;
    uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
    REQUIRE(hz&&!(hz%div),"APB1 selection");
    a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC;
    if(parent==3)shared|=A733_CLOCK_PERI0;
    REQUIRE(a733_clock_acquire(&clocks,4,shared,A733_CLOCK_GPADC|A733_CLOCK_GPADC24M,
        deadline,&adc_token)==A733_CLOCK_OK,"exclusive ADC/parent lease");
    uint32_t source=mmio_read(0x02002fc0);
    adc_config=(struct a733_adc_config){4,1,sample_deadline,apb1,hz/div,!(source&(1u<<31))};
    REQUIRE(a733_adc_native_init(&adc_native,&locks,xTaskGetCurrentTaskHandle(),adc_admit,NULL),"ADC native binding");
    REQUIRE(a733_adc_init(&adc,&adc_config,&adc_native.operations,&adc_native,&clocks,&adc_token),"ADC init");
    enum a733_adc_result ar;
    do{ar=a733_adc_poll(&adc);
        REQUIRE(ar==A733_ADC_PENDING||ar==A733_ADC_BUSY||ar==A733_ADC_READY,"ADC startup");
        REQUIRE(!adc.fault,"ADC startup uncertainty");if(ar!=A733_ADC_READY)pause();
    }while(ar!=A733_ADC_READY);
    step=5;
    uint64_t value;
    struct a733_adc_sample aggregate={0};
    while(sample_count<2&&now(NULL,&value)&&value<sample_deadline-100000){
        REQUIRE(owned_irqs()&&a733_rail_status(&rails,&rail)==A733_RAIL_READY,"ADC/rail lifetime");
        struct a733_adc_sample s;
        ar=a733_adc_read(&adc,sample_deadline,&s);
        REQUIRE(ar==A733_ADC_BUSY||ar==A733_ADC_AGAIN||ar==A733_ADC_SAMPLE,"ADC read");
        REQUIRE(!adc.fault,"ADC read uncertainty");
        if(ar==A733_ADC_SAMPLE){
            for(unsigned i=0;i<4;++i)if(s.valid&(1u<<i)){
                aggregate.code[i]=s.code[i];aggregate.observed_us[i]=s.observed_us[i];
            }
            aggregate.valid|=s.valid;aggregate.sequence=s.sequence;
            if(aggregate.valid==15){
                if(sample_count)for(unsigned i=0;i<4;++i)
                    REQUIRE(aggregate.sequence>samples[0].sequence&&aggregate.observed_us[i]>samples[0].observed_us[i],"fresh second set");
                samples[sample_count++]=aggregate;aggregate=(struct a733_adc_sample){0};
            }
        }
        if(sample_count<2)vTaskDelay(1);
    }
    step=6;
    REQUIRE(owned_irqs(),"ADC disable IRQ ownership");
    do{ar=a733_adc_stop(&adc,deadline);
        REQUIRE(ar==A733_ADC_BUSY||ar==A733_ADC_STOPPED,"ADC configured disable");
        REQUIRE(!adc.fault,"ADC disable uncertainty");if(ar!=A733_ADC_STOPPED)pause();
    }while(ar!=A733_ADC_STOPPED);
    REQUIRE(!adc.held&&!provider.held&&!provider.retained&&!provider.failed&&
        !xSemaphoreGetMutexHolder(locks.mutex[0])&&!xSemaphoreGetMutexHolder(locks.mutex[1])&&
        !atomic_load_explicit(&locks.retained,memory_order_acquire)&&owned_irqs(),"ADC configured-disable completion");
    REQUIRE(a733_rail_status(&rails,&rail)==A733_RAIL_READY,"retained CLDO3 lease");
    report("ADC_RETAINED_GIC_PENDING=",mmio_read(0x03400208)&0x40);
    report("ADC_CONFIGURED_DISABLED=",1);report("ADC_APB1=",apb1);report("ADC_APB1_HZ=",hz/div);
    report("ADC_INITIAL_PARENT=",source);report("ADC_COLD_PARENT=",adc_config.configure_adc_clock);
    for(unsigned n=0;n<sample_count;++n){report("ADC_SET=",n);report("ADC_SEQUENCE=",samples[n].sequence);
        for(unsigned i=0;i<4;++i){report("ADC_CHANNEL=",i);report("ADC_RAW=",samples[n].code[i]);report("ADC_OBSERVED_US=",samples[n].observed_us[i]);}}
    REQUIRE(now(NULL,&value)&&value<deadline,"ADC publication deadline");
    report("ADC_SETS=",sample_count);report("ADC_OUTCOME=",sample_count==2?0:1);
    platform_puts("ADC_RESOURCES=RETAINED_CLOCKS_CLDO3_PMIC_PINS_CONFIGURATION\n");
    if(sample_count==2)platform_puts("PASS native GPADC raw observations\n");
    platform_puts("COMPLETE native ADC configured disable\n");platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{ (void)handle; platform_panic(name,0); }
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe,"adc-read",4096,NULL,3,stack,&task));
    vTaskStartScheduler();
    platform_panic("scheduler returned",0);
}
