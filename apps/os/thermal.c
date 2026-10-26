/* Continuous thermal service for the integrated OS image. Its driver
 * adapters follow the bounded thermal application (apps/thermal/native.c).
 * It reads the five channels every 25 ms or on the THS interrupt for the
 * lifetime of the session and feeds each result to the fixed board policy. A CRITICAL level, a driver
 * fault or a lost owner ends the session through board_fail: supervised
 * images stop, resident images stop renewing the watchdog. Faults retain
 * the clock token, the register mutex and the masked interrupt. */
#include "os.h"
#include "../board/application.h"
#include "../board/session.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"
#include "gicv3_spi.h"
#include "a733_mapping_check.h"
#include "boot.h"
#include <string.h>
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
#include "thermal-report.h"
#endif
static struct os_thermal_service {
    struct a733_thermal driver;
    struct a733_thermal_config config;
    struct a733_clock_token token;
    struct gicv3_spi irq;
    struct a733_thermal_policy policy;
    StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
    atomic_uint failed,pending;
    unsigned active,retaining,masked,rearm_wait;
    uint64_t next_read;
    uint32_t cpu_critical_millicelsius;
} thermal;
/* Recognition may run before the thermal task or the first complete sensor
 * set. Keep load reduced until all channels have valid, nonstale readings. */
/* One publication binds level to the inclusive freshness deadline. The
 * architectural counter divided by 24 fits in the remaining 62 bits. */
static _Atomic uint64_t load_state=A733_THERMAL_THROTTLE;
int os_thermal_throttled(void)
{
    uint64_t state=atomic_load_explicit(&load_state,memory_order_acquire);
    return (state&3)!=A733_THERMAL_NORMAL||a733_smp_counter()/24>(state>>2);
}
static uint64_t fresh_until(void)
{
    uint64_t until=thermal.policy.start_us;
    if(a733_thermal_policy_seen(&thermal.policy)==31){
        until=UINT64_MAX;
        for(unsigned i=0;i<5;++i)if(thermal.policy.seen_us[i]<until)until=thermal.policy.seen_us[i];
    }
    return until+1000000;
}
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
/* The supervised lifetime is 60 s. One owner appends at most 10 rows/s plus
 * an initial and terminal row. Readers acquire the count; published rows
 * never change. No allocation, I/O, wait or additional THS read occurs here. */
#define THERMAL_RECORDS (A733_BOARD_SUPERVISED_SECONDS*10u+2u)
static struct os_thermal_readings {
    struct os_thermal_reading rows[THERMAL_RECORDS];
    atomic_uint count,overflow;
    uint64_t next;
} readings;
static void record_reading(uint64_t now,enum a733_thermal_level level)
{
    if(now<readings.next&&level!=A733_THERMAL_CRITICAL)return;
    unsigned n=atomic_load_explicit(&readings.count,memory_order_relaxed);
    if(n==THERMAL_RECORDS){atomic_store_explicit(&readings.overflow,1,memory_order_release);return;}
    struct os_thermal_reading *r=&readings.rows[n];
    r->time_us=now;r->sequence=thermal.policy.sequence;r->seen=thermal.policy.seen;
    r->stale=thermal.policy.stale;r->level=(uint8_t)level;r->sensor=thermal.policy.sensor;
    for(unsigned i=0;i<5;++i){
        r->millicelsius[i]=thermal.policy.millicelsius[i];
        uint64_t age=now-((r->seen&(1u<<i))?thermal.policy.seen_us[i]:thermal.policy.start_us);
        r->age_us[i]=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
    }
    readings.next=now+100000;
    atomic_store_explicit(&readings.count,n+1,memory_order_release);
}
/* Only the recognizer writes the first window. Publish once; the USB owner
 * acquires it before copying any header/row bytes. Later recognitions leave
 * it intact, so acknowledgments cannot change a paged diagnostic response. */
static struct os_thermal_window {
    struct os_thermal_report header;
    unsigned first,collecting;
    atomic_uint ready;
} window;
extern unsigned mimus_board_generation(void);
void os_thermal_record_begin(void)
{
    if(atomic_load_explicit(&window.ready,memory_order_relaxed)||window.collecting)return;
    unsigned n=atomic_load_explicit(&readings.count,memory_order_acquire);
    window.first=n?n-1:0;window.collecting=1;
    window.header.begin_us=a733_smp_counter()/24;
    window.header.recognition_generation=mimus_board_generation();
}
void os_thermal_record_end(int success)
{
    if(!window.collecting||atomic_load_explicit(&window.ready,memory_order_relaxed))return;
    unsigned n=atomic_load_explicit(&readings.count,memory_order_acquire);
    struct os_thermal_report *h=&window.header;
    h->end_us=a733_smp_counter()/24;h->magic=OS_THERMAL_REPORT_MAGIC;h->version=1;
    h->rows=n-window.first;h->row_bytes=sizeof(struct os_thermal_reading);
    h->bytes=sizeof(*h)+h->rows*h->row_bytes;
    h->epoch=board.session.epoch;h->generation=board.session.generation;
    h->overflow=atomic_load_explicit(&readings.overflow,memory_order_acquire);
    h->cpu_critical_millicelsius=thermal.policy.cpu_critical_millicelsius;h->recognition_ok=success!=0;
    atomic_store_explicit(&window.ready,1,memory_order_release);
}
int os_thermal_report_copy(uint64_t expected,uint32_t offset,size_t bytes,
    uint8_t *out,size_t *copied,uint32_t *total)
{
    if(!out||!copied||!total)return -1;
    *copied=0;*total=0;
    if(!bytes||bytes>1024)return -1;
    if(!atomic_load_explicit(&window.ready,memory_order_acquire))return 0;
    const struct os_thermal_report *h=&window.header;*total=h->bytes;
    if((expected&&expected!=h->recognition_generation)||offset>*total||bytes>*total-offset)return -1;
    size_t head=offset<sizeof(*h)?sizeof(*h)-offset:0;
    if(head>bytes)head=bytes;
    if(head)memcpy(out,(const uint8_t *)h+offset,head);
    if(bytes>head)memcpy(out+head,(const uint8_t *)&readings.rows[window.first]+(offset+head-sizeof(*h)),bytes-head);
    *copied=bytes;return 1;
}
#else
#define record_reading(now,level) ((void)0)
#endif
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int live(void)
{return board_live(NULL)&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
    !atomic_load_explicit(&thermal.failed,memory_order_acquire)&&a733_smp_counter()/24<deadline();}
static int owner(void)
{return thermal.handle&&xTaskGetCurrentTaskHandle()==thermal.handle&&live();}
static int now(void *v,uint64_t *n)
{return v==&thermal?a733_counter_clock_now(&providers.shared.counter,n):0;}
static int enter(void *v,uint64_t d)
{return v==&thermal&&owner()?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v)
{if(v==&thermal)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{if(v==&thermal)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int device(uintptr_t a)
{return a733_mapping_check(aarch64_device_translate,a,a,&device_rule);}
static int mappings(void)
{
    static const uintptr_t a[]={0x02002000,0x02002518,0x02002fc0,0x02002fe4,0x02522000,0x03006244,0x03006248,
        0x0300624c,0x07090160,0x03400000,0x03400108};
    for(unsigned i=0;i<sizeof(a)/sizeof(*a);++i)if(!device(a[i]))return 0;
#ifdef A733_A76_ADMITTED
    /* The CPU-L, CPU-B and DSU PLL pages that os_cpu_admit reads. */
    static const uintptr_t cpu[]={0x08871000,0x08872000,0x08873000};
    for(unsigned i=0;i<3;++i)if(!device(cpu[i]))return 0;
#endif
    return 1;
}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{return v==&thermal&&owner()&&thermal.driver.held&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
    a733_thermal_native_read(NULL,a,out);}
static int write_register(void *v,uintptr_t a,uint32_t value)
{return v==&thermal&&owner()&&thermal.driver.held&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
    a733_thermal_native_write(NULL,a,value);}
static int authorize(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *token)
{
    return v==&thermal&&owner()&&mappings()&&c->owner==thermal.config.owner&&c->handoff==thermal.config.handoff&&
        c->startup_deadline_us==thermal.config.startup_deadline_us&&c->apb1==thermal.config.apb1&&
        c->apb1_hz==thermal.config.apb1_hz&&c->configure_adc_clock==thermal.config.configure_adc_clock&&
        c->alarm_mask==31&&c->critical_mask==31&&!memcmp(c->limits,thermal.config.limits,sizeof(c->limits))&&
        token->id==thermal.token.id&&token->owner==thermal.token.owner&&token->shared==thermal.token.shared&&
        token->exclusive==thermal.token.exclusive&&
        a733_clock_token_status(&providers.shared.clocks,&thermal.token)==A733_CLOCK_OK;
}
static void cancel(void)
{
#ifdef SUNSTONE_A733_OS_NPU
 os_npu_thermal(0,1,0,a733_smp_counter()/24);
#endif
 atomic_store_explicit(&load_state,A733_THERMAL_CRITICAL,memory_order_release);
 atomic_store_explicit(&thermal.failed,1,memory_order_release);board_fail(NULL);}
static void publish_load(enum a733_thermal_level level)
{
    if(level==A733_THERMAL_NORMAL&&a733_thermal_policy_seen(&thermal.policy)!=31)
        level=A733_THERMAL_THROTTLE;
    atomic_store_explicit(&load_state,(fresh_until()<<2)|level,memory_order_release);
}
/* The recognizer backs off at its checkpoints during THROTTLE, and new GPU
 * placements use the CPU. CRITICAL closes admission through board_fail.
 * The policy calls this only from a733_thermal_policy_update, after which
 * step() records the final policy fields before closing the session. */
static int policy(void *v,enum a733_thermal_level level)
{
    if(v!=&thermal)return -1;
    return level==A733_THERMAL_CRITICAL?0:1;
}
/* IRQ callbacks share the SMP kernel critical section with task rearm. They
 * never inspect driver fields, touch THS status or enter the platform mutex. */
void os_thermal_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    if(!thermal.masked){thermal.masked=1;if(!gicv3_spi_mask(&thermal.irq))atomic_store_explicit(&thermal.failed,1,memory_order_release);}
    atomic_store_explicit(&thermal.pending,1,memory_order_release);
    taskEXIT_CRITICAL_FROM_ISR(saved);
}
static int mask(void)
{
    taskENTER_CRITICAL();int ok=1;
    if(!thermal.masked){thermal.masked=1;ok=gicv3_spi_mask(&thermal.irq);}
    taskEXIT_CRITICAL();
    return ok;
}
static int rearm(void)
{
    int ok=1;taskENTER_CRITICAL();
    if(live()&&!atomic_load_explicit(&thermal.pending,memory_order_acquire)&&thermal.masked){
        int r=gicv3_spi_rearm(&thermal.irq);
        if(r==1){thermal.masked=0;thermal.rearm_wait=0;}
        else if(r<0||++thermal.rearm_wait>=100)ok=0;
    }
    taskEXIT_CRITICAL();return ok;
}
static int fault(void){cancel();thermal.retaining=1;(void)mask();return -1;}
/* Owner task only. 1 after a completed bounded step, -1 once failed. */
static int step(void)
{
    if(!owner()&&!thermal.retaining){cancel();thermal.retaining=1;}
    if(thermal.retaining){
        (void)mask();
        if(thermal.retaining==1&&a733_clock_retain(&providers.shared.clocks,&thermal.token)!=A733_CLOCK_BUSY)thermal.retaining=2;
        return -1;
    }
    if(!thermal.active){
        enum a733_thermal_result r=a733_thermal_poll(&thermal.driver);
        uint64_t t;
        if(thermal.driver.fault||(r!=A733_THERMAL_READY&&r!=A733_THERMAL_PENDING&&r!=A733_THERMAL_BUSY))return fault();
        if(r!=A733_THERMAL_READY)return 1;
        if(!a733_counter_clock_now(&providers.shared.counter,&t)||!a733_thermal_policy_init(&thermal.policy,t,policy,&thermal)||
           !a733_thermal_policy_cpu_limit(&thermal.policy,thermal.cpu_critical_millicelsius))
            return fault();
        thermal.active=1;
    }
    unsigned notified=atomic_exchange_explicit(&thermal.pending,0,memory_order_acq_rel);
    uint64_t n=a733_smp_counter()/24;
    if(notified||n>=thermal.next_read){
        struct a733_thermal_sample sample;
        enum a733_thermal_result r=a733_thermal_read(&thermal.driver,deadline(),&sample);
        uint64_t t;
        enum a733_thermal_level level;
        if(!a733_counter_clock_now(&providers.shared.counter,&t))return fault();
        level=a733_thermal_policy_update(&thermal.policy,t,thermal.driver.fault?A733_THERMAL_INVALID:r,&sample);
        record_reading(t,level);
        if(level==A733_THERMAL_CRITICAL)return fault();
        /* The final missing channel may arrive without changing policy level,
         * so the transition callback alone cannot release startup throttling. */
        publish_load(level);
#ifdef SUNSTONE_A733_OS_NPU
        /* NPU admission uses an exclusive deadline; the policy's age==1 s
         * remains fresh, so its equivalent boundary is one microsecond later. */
        os_npu_thermal(level==A733_THERMAL_NORMAL&&a733_thermal_policy_seen(&thermal.policy)==31,
            level!=A733_THERMAL_NORMAL,fresh_until()+1,t);
#endif
        thermal.next_read=n+25000;
    }else{
        /* Expire clock admission between reads without extra THS accesses.
         * Checkpoints independently enforce this same inclusive deadline. */
        uint64_t state=atomic_load_explicit(&load_state,memory_order_relaxed);
        if((state&3)==A733_THERMAL_NORMAL&&n>(state>>2)){
            enum a733_thermal_level level=a733_thermal_policy_update(&thermal.policy,n,A733_THERMAL_AGAIN,NULL);
            record_reading(n,level);
            if(level==A733_THERMAL_CRITICAL)return fault();
            publish_load(level);
#ifdef SUNSTONE_A733_OS_NPU
            os_npu_thermal(0,1,0,n);
#endif
        }
    }
    if(!rearm())return fault();
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    /* After the thermal step, so a CRITICAL reading ends the session first. */
    if(os_cpu_clock_step(&thermal.policy)<0)return fault();
#endif
    return 1;
}
void os_thermal_task(void *unused)
{
    (void)unused;
    for(;;){if(step()==1)BOARD_PROGRESS(OS_SERVICE_THERMAL);vTaskDelay(1);}
}
static int construct(const struct os_session *s)
{
    if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
    thermal.cpu_critical_millicelsius=s->cpu_critical_millicelsius;
    thermal.irq=(struct gicv3_spi){0x03400000,A733_THERMAL_INTID};thermal.masked=1;
    atomic_init(&thermal.failed,0);atomic_init(&thermal.pending,0);
    thermal.config=(struct a733_thermal_config){.owner=s->thermal_owner,.handoff=s->thermal_authority,
        .startup_deadline_us=deadline(),.apb1=s->apb1,.apb1_hz=s->apb1_hz,
        .configure_adc_clock=s->configure_adc_clock,.alarm_mask=31,.critical_mask=31};
    for(unsigned i=0;i<5;++i)thermal.config.limits[i]=(struct a733_thermal_limits){s->limits[i][0],s->limits[i][1],s->limits[i][2]};
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    a733_clock_mask_t exclusive=A733_CLOCK_THS;
    if(thermal.config.configure_adc_clock)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
    if(thermal.config.apb1>>24==3)shared|=A733_CLOCK_PERI0;
    if(a733_clock_acquire(&providers.shared.clocks,s->thermal_owner,shared,exclusive,deadline(),&thermal.token)!=A733_CLOCK_OK)return 0;
    struct a733_thermal_ops ops={now,authorize,enter,leave,retain,read_register,write_register};
    return a733_thermal_init(&thermal.driver,&thermal.config,&ops,&thermal,&providers.shared.clocks,&thermal.token);
}
int os_thermal_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==os_thermal_task;}
int os_thermal_pins(struct a733_pin_handoff *pins){return pins&&os_session_admitted();}
int os_thermal_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(!s||!construct(s))return 0;
#ifdef A733_A76_ADMITTED
    if(!os_cpu_admit())return 0;
#endif
    thermal.handle=xTaskCreateStatic(os_thermal_task,"os-thermal",2048,NULL,4,thermal.stack,&thermal.task);
    return thermal.handle!=NULL;
}
#if defined(SUNSTONE_A733_OS_PERF) && defined(SUNSTONE_A733_OS_GPU_LINEAR)
TaskHandle_t os_thermal_task_handle(void){return thermal.handle;}
#endif
