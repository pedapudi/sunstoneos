/* RTC class service (include/sunstone_rtc.h): absolute time and one alarm for an application that declares "rtc".
 * One owner task on core 0 performs every RTC operation, as the driver (drivers/rtc/a733_rtc.c) requires. The alarm
 * interrupt (INTID 228, level) is routed to core 0: its handler only masks the line, stamps the counter and wakes the
 * owner; the owner acknowledges the RTC, clears the pending state, restores the alarm registers and wakes the
 * application. The RTC runs from the inherited internal RC oscillator and no clock-source register is written. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_rtc.h"
#include "a733_platform_locks.h"
#include "a733_mmio.h"
#include "gicv3_spi.h"
#include "quiesce.h"
#include "sunstone_deadline.h"
#include "sunstone_rtc.h"
#include "sunstone_crumb.h"
#include <stdatomic.h>
#include <string.h>

extern const unsigned ss_rtc_declared;
_Static_assert(sizeof(struct ss_rtc_switch)==sizeof(struct a733_rtc_switch),"switch record layouts");
_Static_assert(sizeof(struct ss_rtc_time)==sizeof(struct a733_rtc_time),"time layouts");
#define RTC UINT32_C(0x07090000)
#define GICD UINT32_C(0x03400000)
enum phase {CLOSED,OPENING,OPEN,CLOSING,FAULT};
enum op {NONE,OPEN_OP,SET_OP,READ_OP,ARM_OP,CLOSE_OP,SELECT_OP,MEASURE_OP};
enum {POSTED=1,DONE=2};
static struct {
    struct a733_rtc rtc;
    struct a733_clock_token clock;
    struct gicv3_spi gic;
    atomic_uint phase,request,live,irq_flag,irq_count,alarm_state,quiesce_wanted,quiesced;
    unsigned op;int result;
    uint32_t a,b;
    struct ss_rtc_info info;struct ss_rtc_time time;uint64_t time_ticks;
    struct ss_rtc_alarm alarm;
    struct ss_rtc_switch sw;struct ss_rtc_rate rate;atomic_uint switching;
    uint64_t irq_ticks,alarm_deadline_us;
    uint32_t fault;
    TaskHandle_t waiter;
} rtc;
static TaskHandle_t owner;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];

static uint64_t now_us(void){return sunstone_deadline_now_us();}
static uint64_t lease_now(void){uint64_t t=0;return a733_counter_clock_now(&providers.shared.counter,&t)==1?t:0;}
static int ops_now(void *c,uint64_t *n){(void)c;uint64_t t=lease_now();if(!t)return 0;*n=t;return 1;}
static int ops_read(void *c,uint32_t o,uint32_t *v)
{
    (void)c;
    if(o>0x160||(o&3)||a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,lease_now()+20000)!=1)return 0;
    *v=a733_mmio_read32_dsb_sy_both(RTC+o);
    a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;
}
/* Only the calendar and the alarm words are writable; the clock-source registers are not reachable. */
static int ops_write(void *c,uint32_t o,uint32_t v)
{
    (void)c;
    /* The control and status words are writable only while the owner runs the crystal selection. */
    const int source_word=(o==0||o==4)&&atomic_load_explicit(&rtc.switching,memory_order_acquire);
    if((!source_word&&o!=0x10&&o!=0x14&&(o<0x20||o>0x30||(o&3)))||a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,lease_now()+20000)!=1)return 0;
    a733_mmio_write32_dsb_sy_both(RTC+o,v);
    a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;
}
static int pause(void *c,uint64_t until)
{
    (void)c;uint64_t n=lease_now();
    if(n<until){uint64_t t=now_us()+(until-n);while(now_us()<t)(void)sunstone_deadline_wait_until(t);}
    return 1;
}
static void fail(unsigned step){if(!rtc.fault)rtc.fault=step;atomic_store(&rtc.live,0);atomic_store(&rtc.phase,FAULT);}

/* --- interrupt ------------------------------------------------------------------------------------------------------ */
void os_rtc_irq(void)
{
    if(!atomic_load_explicit(&rtc.live,memory_order_acquire)){(void)gicv3_spi_mask(&rtc.gic);return;}
    (void)gicv3_spi_mask(&rtc.gic);
    rtc.irq_ticks=ss_counter_ticks();
    atomic_fetch_add_explicit(&rtc.irq_count,1,memory_order_relaxed);
    atomic_store_explicit(&rtc.irq_flag,1,memory_order_release);
    BaseType_t woken=pdFALSE;
    vTaskNotifyGiveFromISR(owner,&woken);
    portYIELD_FROM_ISR(woken);
}
static int gic_masked(void)
{
    int r=gicv3_spi_mask(&rtc.gic);
    for(unsigned i=0;r==1&&i<200;++i){r=gicv3_spi_masked(&rtc.gic);if(r==1)break;if(r==0)(void)sunstone_deadline_wait_until(now_us()+10);}
    return r==1;
}
static uint32_t gic_state(void){/* enabled, pending, active bits of INTID 228 */
    uint32_t b=1u<<(228%32),w=228/32*4;
    return (!!(a733_mmio_read32_dsb_sy_both(GICD+0x100+w)&b))|(!!(a733_mmio_read32_dsb_sy_both(GICD+0x200+w)&b))<<1|
           (!!(a733_mmio_read32_dsb_sy_both(GICD+0x300+w)&b))<<2;
}
/* The alarm fired or timed out: acknowledge, restore, publish. */
static void end_alarm(int timed_out)
{
    atomic_store_explicit(&rtc.live,0,memory_order_release);
    if(!gic_masked()){fail(20);return;}
    int ok;
    if(!timed_out){
        ok=a733_rtc_irq(&rtc.rtc);
        /* The level interrupt may still be pending in the distributor; clear it now that the source is acknowledged. */
        a733_mmio_write32_dsb_sy_both(GICD+0x280+228/32*4,1u<<(228%32));
        if(!ok||(gic_state()&6)){fail(21);return;}
    }
    if(!a733_rtc_finish(&rtc.rtc,timed_out)){fail(22);return;}
    rtc.alarm.status=timed_out?SS_RTC_ALARM_TIMEOUT:SS_RTC_ALARM_FIRED;
    rtc.alarm.terminal=*(struct ss_rtc_time *)&rtc.rtc.snapshot.terminal;
    rtc.alarm.irq_ticks=rtc.irq_ticks;rtc.alarm.irq_count=atomic_load(&rtc.irq_count);
    rtc.alarm.done_ticks=ss_counter_ticks();
    atomic_store_explicit(&rtc.alarm_state,DONE,memory_order_release);
    os_application_wake_task(rtc.waiter);
}

/* --- requests ------------------------------------------------------------------------------------------------------- */
static int do_select(void)
{
    struct a733_rtc_switch s;
    atomic_store(&rtc.switching,1);
    const int ok=a733_rtc_select_crystal(&rtc.rtc,pause,&s);
    atomic_store(&rtc.switching,0);
    if(!ok){fail(10);return -1;}
    memcpy(&rtc.sw,&s,sizeof(s));
    rtc.info.source=rtc.rtc.read_source;
    rtc.info.clock_control=rtc.rtc.snapshot.clock_control;rtc.info.clock_status=rtc.rtc.snapshot.clock_status;
    return 1;
}
static int read_seconds(uint32_t *s)
{uint32_t v;if(!ops_read(NULL,0x14,&v))return 0;*s=v&63;return 1;}
/* Sleep in 20 us steps until the seconds field changes; the counter is read right after the read that saw it. */
static int next_edge(uint32_t *seconds,uint64_t *ticks,unsigned *polls,uint64_t limit_us)
{
    uint32_t s;
    for(;;){
        (void)sunstone_deadline_wait_until(now_us()+20);
        if(!read_seconds(&s))return 0;
        ++*polls;
        if(s!=*seconds){*ticks=ss_counter_ticks();*seconds=s;return 1;}
        if(now_us()>limit_us)return 0;
    }
}
static int measure(unsigned n)
{
    memset(&rtc.rate,0,sizeof(rtc.rate));
    rtc.rate.seconds=n;rtc.rate.source=rtc.rtc.read_source;
    uint32_t s;uint64_t a,b;
    if(!read_seconds(&s)||!next_edge(&s,&a,&rtc.rate.polls,now_us()+3000000))return -1;
    /* An RTC second lasts at least 0.9 s on either source, so sleeping 0.85 s for each of the first n - 1 seconds
     * cannot pass the n-th edge. The seconds field then says how many edges have gone by. */
    const uint64_t guard=now_us()+(uint64_t)(n-1)*850000;
    while(now_us()<guard)(void)sunstone_deadline_wait_until(guard);
    uint32_t last;
    if(!read_seconds(&last))return -1;
    unsigned edges=(last+60-s)%60;
    if(edges>=n)return -1;
    while(edges<n){
        if(!next_edge(&last,&b,&rtc.rate.polls,now_us()+4000000))return -1;
        ++edges;
    }
    if(edges!=n)return -1;
    rtc.rate.edge_a_ticks=a;rtc.rate.edge_b_ticks=b;rtc.rate.status=1;
    return 1;
}
static int do_open(void)
{
    atomic_store(&rtc.phase,OPENING);
    const uint64_t limit=lease_now()+40000000;
    enum a733_clock_result c=A733_CLOCK_BUSY;
    while(c==A733_CLOCK_BUSY&&lease_now()<limit-39900000){
        c=a733_clock_acquire(&providers.shared.clocks,UINT64_C(0x5254430000000001),0,A733_CLOCK_RTC32K,limit,&rtc.clock);
        if(c==A733_CLOCK_BUSY)(void)sunstone_deadline_wait_until(now_us()+50);
    }
    if(c!=A733_CLOCK_OK){atomic_store(&rtc.phase,CLOSED);return -1;}
    /* The alarm line stays masked and inactive until an alarm is armed. */
    if(!gic_masked()||(gic_state()&6)){fail(1);return -1;}
    struct a733_rtc_ops ops={ops_now,ops_read,ops_write};
    if(!a733_rtc_open_rc(&rtc.rtc,&ops,&rtc,limit)){fail(2);return -1;}
    rtc.info.initial=*(struct ss_rtc_time *)&rtc.rtc.snapshot.initial;
    rtc.info.clock_control=rtc.rtc.snapshot.clock_control;rtc.info.clock_status=rtc.rtc.snapshot.clock_status;
    rtc.info.prescaler=rtc.rtc.prescaler;rtc.info.calibration=rtc.rtc.calibration;rtc.info.oscillator=rtc.rtc.oscillator;
    rtc.info.source=rtc.rtc.read_source;rtc.info.opened_ticks=ss_counter_ticks();
    memset(&rtc.sw,0,sizeof(rtc.sw));
    atomic_store(&rtc.phase,OPEN);
    if(!(rtc.a&SS_RTC_KEEP_RC)&&rtc.rtc.read_source==A733_RTC_READ_RC_NORMAL&&do_select()<0)return -1;
    memcpy(&rtc.info.crystal,&rtc.sw,sizeof(rtc.sw));
    return 1;
}
static void serve(void)
{
    if(atomic_load_explicit(&rtc.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&rtc.phase);
    int r=-1;
    switch(rtc.op){
    case OPEN_OP:if(phase==CLOSED)r=do_open();else if(phase==OPEN)r=1;break;
    case SELECT_OP:
        if(phase==OPEN&&rtc.rtc.state==A733_RTC_READY&&rtc.rtc.read_source==A733_RTC_READ_RC_NORMAL)r=do_select();
        break;
    case MEASURE_OP:
        if(phase==OPEN&&rtc.rtc.state==A733_RTC_READY&&rtc.a>=1&&rtc.a<=20)r=measure(rtc.a);
        break;
    case SET_OP:
        if(phase==OPEN&&rtc.rtc.state==A733_RTC_READY)r=a733_rtc_set_time(&rtc.rtc,rtc.a,rtc.b,pause)?1:(fail(3),-1);
        break;
    case READ_OP:
        if(phase==OPEN&&(rtc.rtc.state==A733_RTC_READY||rtc.rtc.state==A733_RTC_ARMED||
                         rtc.rtc.state==A733_RTC_FIRED||rtc.rtc.state==A733_RTC_CANCELLED)){
            struct a733_rtc_time t;
            if(a733_rtc_read(&rtc.rtc,&t)){memcpy(&rtc.time,&t,sizeof(t));rtc.time_ticks=ss_counter_ticks();r=1;}else fail(4);
        }
        break;
    case ARM_OP:
        if(phase==OPEN&&rtc.rtc.state==A733_RTC_READY&&atomic_load(&rtc.alarm_state)==0){
            struct a733_rtc_time t;
            memset(&rtc.alarm,0,sizeof(rtc.alarm));atomic_store(&rtc.irq_flag,0);
            rtc.alarm.seconds=rtc.a;
            if(!a733_rtc_read(&rtc.rtc,&t)){fail(5);break;}
            rtc.alarm.armed=*(struct ss_rtc_time *)&t;
            atomic_store_explicit(&rtc.live,1,memory_order_release);
            rtc.alarm.arm_ticks=ss_counter_ticks();
            if(!a733_rtc_arm(&rtc.rtc,rtc.a)){fail(6);break;}
            rtc.alarm.arm_ticks=ss_counter_ticks();
            rtc.alarm.target_day=rtc.rtc.snapshot.target_day;rtc.alarm.target_hms=rtc.rtc.snapshot.target_hms;
            /* Generous bound: the RC second is about 1.1 s; allow 2 s per second plus 2 s. */
            rtc.alarm_deadline_us=now_us()+(uint64_t)(rtc.a*2+2)*1000000;
            int g=0;for(unsigned i=0;i<100&&!g;++i){g=gicv3_spi_rearm(&rtc.gic);if(g<0)break;if(!g)(void)sunstone_deadline_wait_until(now_us()+10);}
            if(g<=0){fail(7);break;}
            atomic_store(&rtc.alarm_state,POSTED);r=1;
        }
        break;
    case CLOSE_OP:
        if(phase==OPEN){
            atomic_store(&rtc.phase,CLOSING);
            if(rtc.rtc.state==A733_RTC_ARMED){end_alarm(1);if(atomic_load(&rtc.phase)==FAULT){r=-1;break;}}
            atomic_store(&rtc.live,0);
            if(!gic_masked()){fail(8);r=-1;break;}
            if(a733_clock_release(&providers.shared.clocks,&rtc.clock,lease_now()+50000)!=A733_CLOCK_OK){fail(9);r=-1;break;}
            rtc.clock=(struct a733_clock_token){0};
            memset(&rtc.rtc,0,sizeof(rtc.rtc));
            atomic_store(&rtc.alarm_state,0);
            atomic_store(&rtc.phase,CLOSED);r=1;
        }else if(phase==CLOSED)r=1;
        break;
    default:break;
    }
    rtc.result=r;
    atomic_store_explicit(&rtc.request,DONE,memory_order_release);
    os_application_wake_task(rtc.waiter);
}
void os_rtc_task(void *unused)
{
    (void)unused;
    if(sunstone_deadline_register()!=1)for(;;)vTaskSuspend(NULL);
    for(;;){
        uint64_t next=now_us()+10000000;
        serve();
        if(atomic_load_explicit(&rtc.irq_flag,memory_order_acquire)&&atomic_load(&rtc.alarm_state)==POSTED){
            atomic_store(&rtc.irq_flag,0);end_alarm(0);
        }else if(atomic_load(&rtc.alarm_state)==POSTED){
            if(now_us()>=rtc.alarm_deadline_us)end_alarm(1);
            else if(rtc.alarm_deadline_us<next)next=rtc.alarm_deadline_us;
        }
        if(atomic_load(&rtc.quiesce_wanted)&&atomic_load(&rtc.request)!=POSTED){
            if(atomic_load(&rtc.phase)==OPEN){rtc.op=CLOSE_OP;atomic_store(&rtc.request,POSTED);continue;}
            if(atomic_load(&rtc.phase)==CLOSED)atomic_store(&rtc.quiesced,1);
        }
        if(now_us()<next)(void)sunstone_deadline_wait_until(next);
    }
}
int os_rtc_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_rtc_task;}
int os_rtc_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
static enum os_quiesce_result quiesce_step(struct os_quiesce *q)
{
    if(atomic_load(&rtc.phase)==FAULT){
        (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x921,rtc.fault,SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
    if(atomic_load(&rtc.quiesced))return OS_QUIESCE_DONE;
    atomic_store(&rtc.quiesce_wanted,1);
    if(owner)xTaskNotifyGive(owner);
    return OS_QUIESCE_PENDING;
}
static struct os_quiesce rtc_quiesce={.owner=OS_QUIESCE_RTC,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_rtc_start(void)
{
    if(!ss_rtc_declared)return 1;
    if(!os_session_admitted()||!os_quiesce_register(&rtc_quiesce))return 0;
    rtc.gic=(struct gicv3_spi){GICD,228};
    owner=xTaskCreateStaticAffinitySet(os_rtc_task,"os-rtc",sizeof(owner_stack)/sizeof(owner_stack[0]),NULL,4,owner_stack,&owner_tcb,1);
    return owner!=NULL;
}

/* --- application calls ---------------------------------------------------------------------------------------------- */
static int call(unsigned op,uint32_t a,uint32_t b)
{
    if(!ss_rtc_declared||!ss_grant()||atomic_load(&rtc.quiesce_wanted))return -1;
    const unsigned state=atomic_load_explicit(&rtc.request,memory_order_acquire);
    if(state==DONE){
        if(rtc.op!=op)return -1;
        const int r=rtc.result;
        atomic_store_explicit(&rtc.request,0,memory_order_release);
        return r;
    }
    if(state==POSTED)return rtc.op==op?0:-1;
    rtc.op=op;rtc.a=a;rtc.b=b;rtc.waiter=xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&rtc.request,POSTED,memory_order_release);
    xTaskNotifyGive(owner);
    return 0;
}
int ss_rtc_open(struct ss_rtc_info *info)
{return ss_rtc_open_with(info,0);}
int ss_rtc_open_with(struct ss_rtc_info *info,uint32_t flags)
{int r=call(OPEN_OP,flags,0);if(r==1&&info)*info=rtc.info;return r;}
int ss_rtc_select_crystal(struct ss_rtc_switch *record)
{int r=call(SELECT_OP,0,0);if(r==1&&record)*record=*(struct ss_rtc_switch *)&rtc.sw;return r;}
int ss_rtc_measure(unsigned seconds,struct ss_rtc_rate *rate)
{int r=call(MEASURE_OP,seconds,0);if(r==1&&rate)*rate=rtc.rate;return r;}
int ss_rtc_set(uint32_t day,uint32_t hms){return call(SET_OP,day,hms);}
int ss_rtc_read(struct ss_rtc_time *t,uint64_t *ticks)
{int r=call(READ_OP,0,0);if(r==1&&t){*t=rtc.time;if(ticks)*ticks=rtc.time_ticks;}return r;}
int ss_rtc_arm(unsigned seconds){return seconds<1||seconds>10?-1:call(ARM_OP,seconds,0);}
int ss_rtc_alarm(struct ss_rtc_alarm *a)
{
    if(!ss_rtc_declared||!ss_grant()||!a)return -1;
    const unsigned s=atomic_load_explicit(&rtc.alarm_state,memory_order_acquire);
    if(s==DONE){*a=rtc.alarm;return 1;}
    return atomic_load(&rtc.phase)==FAULT?-1:0;
}
static int any_ready(void)
{return atomic_load(&rtc.request)==DONE||atomic_load(&rtc.alarm_state)==DONE||atomic_load(&rtc.phase)==FAULT;}
void ss_rtc_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,any_ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x759);
}
int ss_rtc_close(void){return call(CLOSE_OP,0,0);}
