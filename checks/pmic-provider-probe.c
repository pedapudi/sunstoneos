#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"

/* The accepted returning platform supplies startup and recovery. All provider
 * callbacks below are production implementations; only this finite client is
 * new. DCDC1 is the first rail requested by native USB startup. */
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

static void report(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); platform_puts("\n"); }

static _Noreturn void failed(const char *name)
{
    platform_puts("INCOMPLETE native PMIC: "); platform_puts(name); platform_puts("\n");
    report("client step=",step); report("provider stage=",provider.stage);
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
    platform_puts("BEGIN native PMIC provider\n");
    REQUIRE(a733_counter_clock_init(&clock,24000000),"counter init");
    uint64_t start;
    REQUIRE(now(NULL,&start) && start<=UINT64_MAX-500000,"counter sample");
    deadline=start+500000;
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
    REQUIRE(a733_rail_acquire(&rails,3,A733_RAIL_BIT(A733_RAIL_DCDC1),0,deadline,&rail)==A733_RAIL_PENDING,"rail acquire");
    enum a733_rail_result rr;
    do { rr=a733_rail_poll(&rails,&rail); REQUIRE(rr==A733_RAIL_PENDING || rr==A733_RAIL_BUSY || rr==A733_RAIL_READY,"rail poll"); if(rr!=A733_RAIL_READY) pause(); } while(rr!=A733_RAIL_READY);
    REQUIRE(a733_pmic_submit(&provider,3,&rail,A733_RAIL_DCDC1,deadline,&request)==A733_PMIC_PENDING,"snapshot submit");
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
    REQUIRE(snapshot.rail==A733_RAIL_DCDC1 && !snapshot.page_before && !snapshot.page_after &&
            snapshot.chip==3 && !snapshot.revision && snapshot.decoded && snapshot.enabled,"snapshot identity/decode");
    step=4;
    REQUIRE(a733_pmic_reap(&provider,&request)==A733_PMIC_COMPLETE,"snapshot reap");
    REQUIRE(a733_rail_release(&rails,&rail,deadline)==A733_RAIL_READY,"client rail release");
    REQUIRE(a733_pmic_live(&provider,&lifetime)==A733_PMIC_READY && !provider.held &&
            !provider.retained && !provider.failed && provider.active==A733_PMIC_REQUESTS &&
            !atomic_load_explicit(&locks.retained,memory_order_acquire),"provider quiescence");
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        REQUIRE(!provider.slots[i].state && !provider.slots[i].borrow.id,"request/borrow cleanup");
    REQUIRE(!xSemaphoreGetMutexHolder(locks.mutex[0]) && !xSemaphoreGetMutexHolder(locks.mutex[1]),"mutex cleanup");
    for(unsigned i=0;i<sizeof(before)/sizeof(before[0]);++i) {
        uint32_t value=mmio_read(observed[i]); report("final address=",observed[i]); report("final value=",value);
        /* CNTR's hardware interrupt flag may be set after a completed STOP. */
        uint32_t mask=i==3 ? ~UINT32_C(8) : UINT32_MAX;
        REQUIRE((value&mask)==(before[i]&mask),"inherited state restoration");
    }
    REQUIRE((mmio_read(0x0708300c)&0xf4)==0x40 && mmio_read(0x07083010)==0xf8 &&
            (mmio_read(0x07083020)&0x35)==0x30,"TWI idle");
    uint64_t end;
    REQUIRE(now(NULL,&end) && end<deadline,"completion deadline");
    report("elapsed microseconds=",end-start);
    platform_puts("PASS native PMIC provider snapshot and request cleanup\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{ (void)handle; platform_panic(name,0); }
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe,"pmic-provider",4096,NULL,3,stack,&task));
    vTaskStartScheduler();
    platform_panic("scheduler returned",0);
}
