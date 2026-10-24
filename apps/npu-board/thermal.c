/* NPU thermal supervision reuses the native THS adapters and fixed policy
 * from apps/os/thermal.c. Sensor/IRQ faults close NPU admission first; the
 * bootstrap independently bounds draining before the board is failed. */
#include "npu-board.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"
#include "a733_mapping_check.h"
#include "gicv3_spi.h"
#include "boot.h"
#include <string.h>
static struct npu_board_thermal {
    struct a733_thermal driver;
    struct a733_thermal_config config;
    struct a733_clock_token token;
    struct gicv3_spi irq;
    struct a733_thermal_policy policy;
    StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
    atomic_uint failed,pending;
    unsigned active,retaining,masked,rearm_wait;
    uint64_t next_read,observed_now;
    unsigned stopped;
} thermal;
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
        c->alarm_mask==thermal.config.alarm_mask&&c->critical_mask==thermal.config.critical_mask&&!memcmp(c->limits,thermal.config.limits,sizeof(c->limits))&&
        token->id==thermal.token.id&&token->owner==thermal.token.owner&&token->shared==thermal.token.shared&&
        token->exclusive==thermal.token.exclusive&&
        a733_clock_token_status(&providers.shared.clocks,&thermal.token)==A733_CLOCK_OK;
}
static void cancel(void)
{
    npu_board_client_thermal(&npu_board.client,0,1,0,thermal.observed_now);
    atomic_store_explicit(&thermal.failed,1,memory_order_release);
}
static int policy(void *v,enum a733_thermal_level level)
{
    if(v!=&thermal)return -1;
    if(level==A733_THERMAL_CRITICAL)
        npu_board_client_thermal(&npu_board.client,0,1,0,thermal.observed_now);
    return 1;
}
/* IRQ callbacks share the SMP kernel critical section with task rearm. They
 * never inspect driver fields, touch THS status or enter the platform mutex. */
void npu_board_thermal_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    if(!thermal.masked){thermal.masked=1;if(!gicv3_spi_mask(&thermal.irq))atomic_store_explicit(&thermal.failed,1,memory_order_release);}
    atomic_store_explicit(&thermal.pending,1,memory_order_release);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    BaseType_t wake=pdFALSE;
    if(thermal.handle)vTaskNotifyGiveFromISR(thermal.handle,&wake);
    portYIELD_FROM_ISR(wake);
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
/* Each read publishes a coherent freshness deadline even when the policy
 * level did not change. Initial NORMAL with no samples cannot admit work. */
static void publish(void)
{
    uint64_t until=0;
    if(a733_thermal_policy_seen(&thermal.policy)==31) {
        until=UINT64_MAX;
        for(unsigned i=0;i<5;++i)if(thermal.policy.seen_us[i]<until)until=thermal.policy.seen_us[i];
        until+=1000000;
    } else until=thermal.policy.start_us+1000000;
    const enum a733_thermal_level level=a733_thermal_policy_level(&thermal.policy);
    npu_board_client_thermal(&npu_board.client,level==A733_THERMAL_NORMAL&&
        a733_thermal_policy_seen(&thermal.policy)==31,level==A733_THERMAL_CRITICAL,until,thermal.observed_now);
}
int npu_board_thermal_step(void)
{
    if(!a733_counter_clock_now(&providers.shared.counter,&thermal.observed_now)) {
        thermal.observed_now=a733_smp_counter()/24;return fault();
    }
    if(!owner()&&!thermal.retaining){cancel();thermal.retaining=1;}
    if(thermal.retaining) {
        (void)mask();
        if(thermal.retaining==1&&a733_clock_retain(&providers.shared.clocks,&thermal.token)!=A733_CLOCK_BUSY)
            thermal.retaining=2;
        return -1;
    }
    if(npu_board_client_stopping(&npu_board.client)) {
        if(!mask())return fault();
        if(thermal.active&&!thermal.stopped) {
            enum a733_thermal_result r=a733_thermal_stop(&thermal.driver,deadline());
            if(r==A733_THERMAL_STOPPED)thermal.stopped=1;
            else if(r!=A733_THERMAL_BUSY||thermal.driver.fault)return fault();
        }
        return 1;
    }
    if(!thermal.active) {
        enum a733_thermal_result r=a733_thermal_poll(&thermal.driver);
        if(thermal.driver.fault||(r!=A733_THERMAL_READY&&r!=A733_THERMAL_PENDING&&r!=A733_THERMAL_BUSY))return fault();
        if(r!=A733_THERMAL_READY)return 1;
        if(!a733_counter_clock_now(&providers.shared.counter,&thermal.observed_now)||
           !a733_thermal_policy_init(&thermal.policy,thermal.observed_now,policy,&thermal))return fault();
        thermal.active=1;
    }
    unsigned notified=atomic_exchange_explicit(&thermal.pending,0,memory_order_acq_rel);
    if(notified||thermal.observed_now>=thermal.next_read) {
        struct a733_thermal_sample sample;
        enum a733_thermal_result r=a733_thermal_read(&thermal.driver,deadline(),&sample);
        if(thermal.driver.fault||!a733_counter_clock_now(&providers.shared.counter,&thermal.observed_now))return fault();
        (void)a733_thermal_policy_update(&thermal.policy,thermal.observed_now,r,&sample);
        publish();thermal.next_read=thermal.observed_now+25000;
        if(npu_board_client_stopping(&npu_board.client)){(void)mask();return 1;}
    }
    if(!rearm())return fault();
    return 1;
}
static void task(void *unused)
{
    (void)unused;
    for(;;) {
        (void)npu_board_thermal_step();
        /* Interrupts notify this task; startup and a busy rearm retry on the
         * next tick. Normal idle monitoring wakes once per 25 ms. */
        (void)ulTaskNotifyTake(pdTRUE,thermal.active&&!thermal.rearm_wait?pdMS_TO_TICKS(25):1);
    }
}
static int construct(const struct npu_board_session *s)
{
    if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
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
int npu_board_thermal_privileged(void (*entry)(void *),void *context)
{return !context&&entry==task;}
int npu_board_thermal_start(void)
{
    if(!construct(&npu_board.session))return 0;
    thermal.handle=xTaskCreateStaticAffinitySet(task,"npu-thermal",2048,NULL,4,thermal.stack,&thermal.task,1);
    return thermal.handle!=NULL;
}
