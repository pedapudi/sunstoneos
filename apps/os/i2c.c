/* Header I2C class service (include/sunstone_i2c.h): the OS owner of the controllers TWI0 to TWI3 and S_TWI2 for an
 * application that declares them in its manifest.
 *
 * One owner task, on core 0 where the shared interrupts are routed, does every register operation that is not part of a
 * transfer: it takes the clock and pin leases, brings a controller out of reset, programs its rate, unmasks its interrupt,
 * starts transfers and, at a deadline, aborts one. The controller interrupt runs the engine (drivers/i2c/a733_i2c_bus.c)
 * and, when the last step of a transfer ends, publishes the result and wakes the application task: no task sits between
 * the interrupt and the application. The owner sleeps on its notification and on the precise timer (sunstone_deadline),
 * never on the tick. The owner and the interrupt handler share a bus only inside a kernel critical section, so the
 * engine needs no lock. A controller is held by this service from open until close, and never touched otherwise. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_i2c_bus.h"
#include "a733_clock_reference.h"
#include "a733_platform_locks.h"
#include "a733_mmio.h"
#include "gicv3_spi.h"
#include "quiesce.h"
#include "sunstone_board.h"
#include "sunstone_deadline.h"
#include "sunstone_i2c.h"
#include "sunstone_crumb.h"
#include <stdatomic.h>
#include <string.h>

/* The generated declarations (tools/sunstone-manifest.py). */
extern const struct os_i2c_declaration ss_i2c_declarations[];
extern const unsigned ss_i2c_declaration_count;

_Static_assert(sizeof(struct ss_i2c_message)==sizeof(struct a733_i2c_message)&&
    offsetof(struct ss_i2c_message,address)==offsetof(struct a733_i2c_message,address)&&
    offsetof(struct ss_i2c_message,flags)==offsetof(struct a733_i2c_message,flags)&&
    offsetof(struct ss_i2c_message,data)==offsetof(struct a733_i2c_message,data)&&
    offsetof(struct ss_i2c_message,length)==offsetof(struct a733_i2c_message,length),"message layouts");
_Static_assert((int)SS_I2C_OK==(int)A733_I2C_OK&&(int)SS_I2C_NACK_ADDRESS==(int)A733_I2C_NACK_ADDRESS&&
    (int)SS_I2C_NACK_DATA==(int)A733_I2C_NACK_DATA&&(int)SS_I2C_ARBITRATION==(int)A733_I2C_ARBITRATION&&
    (int)SS_I2C_BUS_ERROR==(int)A733_I2C_BUS_ERROR&&(int)SS_I2C_TIMEOUT==(int)A733_I2C_TIMEOUT&&
    (int)SS_I2C_UNEXPECTED==(int)A733_I2C_UNEXPECTED,"status numbers");

#define CCU UINT32_C(0x02002000)
#define GICD UINT32_C(0x03400000)
#define OWNER_BASE UINT64_C(0x4932430000000000)  /* "I2C" in the high bytes; the bus number is added */
enum phase { CLOSED, OPENING, OPEN, CLOSING, FAULT };
enum command { NO_COMMAND, OPEN_COMMAND, CLOSE_COMMAND };
enum transfer { FREE, SUBMITTED, ACTIVE, DONE };
/* First failure of a bus, for the evidence record: the step in the high half, a detail below. */
enum fault_step { F_NONE, F_PARENT=1, F_CLOCK, F_GATE, F_PINS, F_RESET, F_IDLE, F_GIC, F_START, F_CLOSE_GIC, F_CLOSE_IDLE,
    F_CLOSE_GATE, F_CLOSE_PINS, F_CLOSE_CLOCK, F_ABORT, F_TIME };

struct bus {
    const struct a733_i2c_bus_data *data;
    const struct sunstone_i2c_route *route;
    struct gicv3_spi gic;
    a733_clock_mask_t scope;
    uint32_t max_rate;
    struct a733_i2c_engine engine;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pins;
    struct a733_pin_request_banked requests[2];
    atomic_uint phase,command,transfer,live;
    uint32_t want_rate,rate,input_hz,word,fault,fault_detail;
    uint32_t inherited_bgr,inherited_parent,lines_open,lines_close,idle_starts,busy_starts;
    unsigned opened;
    const struct ss_i2c_message *messages;
    uint32_t count,timeout_us;
    uint64_t deadline_us,started,finished;
    struct ss_i2c_result result;
    TaskHandle_t waiter;
    atomic_uint strays;
};
static const uint32_t unsampled=SS_I2C_LINES_UNSAMPLED;
static struct bus buses[A733_I2C_BUS_COUNT];
static TaskHandle_t owner;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];
static atomic_uint quiesce_wanted,quiesced;
static unsigned declared_mask;

static uint64_t now_us(void){return sunstone_deadline_now_us();}
static uint64_t lease_now(void)
{
    uint64_t t=0;
    return a733_counter_clock_now(&providers.shared.counter,&t)==1?t:0;
}
static uint32_t rd(uintptr_t a){return a733_mmio_read32_dsb_sy_both(a);}
static void wr(uintptr_t a,uint32_t v){a733_mmio_write32_dsb_sy_both(a,v);}
static uint32_t engine_read(void *c,uint32_t o){return rd(((const struct bus *)c)->data->base+o);}
static void engine_write(void *c,uint32_t o,uint32_t v){wr(((const struct bus *)c)->data->base+o,v);}

/* --- the declaration ------------------------------------------------------------------------------------------------ */
static const struct sunstone_i2c_route *route_of(unsigned bus)
{
    const struct sunstone_board_description *board=sunstone_board_orangepi_zero3w();
    for(size_t i=0;i<board->i2c_route_count;++i)if(board->i2c_routes[i].bus==bus)return &board->i2c_routes[i];
    return NULL;
}
static a733_clock_mask_t scope_of(unsigned bus)
{
    switch(bus){
    case A733_I2C_TWI0:return A733_CLOCK_TWI0;
    case A733_I2C_TWI1:return A733_CLOCK_TWI1;
    case A733_I2C_TWI2:return A733_CLOCK_TWI2;
    case A733_I2C_TWI3:return A733_CLOCK_TWI3;
    case A733_I2C_S_TWI2:return A733_CLOCK_S_TWI2;
    default:return 0;
    }
}
/* Validate the declarations once: a header route exists for each bus, none repeats, the rate is positive. */
static int declarations_valid(void)
{
    unsigned seen=0;
    for(unsigned i=0;i<ss_i2c_declaration_count;++i){
        const struct os_i2c_declaration *d=&ss_i2c_declarations[i];
        if(d->bus>=A733_I2C_BUS_COUNT||(seen&(1u<<d->bus))||!d->max_rate_hz||!route_of(d->bus)||
           !a733_i2c_bus_data(d->bus)||!scope_of(d->bus))return 0;
        seen|=1u<<d->bus;
    }
    return 1;
}
int os_i2c_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==os_i2c_task;}
/* The pins every declared route needs, before the pin manager exists. */
int os_i2c_pins(struct a733_pin_handoff *pins)
{
    if(!pins||!declarations_valid())return 0;
    for(unsigned i=0;i<ss_i2c_declaration_count;++i){
        const struct sunstone_i2c_route *r=route_of(ss_i2c_declarations[i].bus);
        const uint32_t mask=(UINT32_C(1)<<r->scl_pin)|(UINT32_C(1)<<r->sda_pin);
        if(pins->authorized[r->bank]&mask)return 0;
        pins->authorized[r->bank]|=mask;
    }
    return 1;
}

/* --- failure and waiting -------------------------------------------------------------------------------------------- */
static void fail(struct bus *b,unsigned step,uint32_t detail)
{
    if(!b->fault){b->fault=step;b->fault_detail=detail;}
    atomic_store_explicit(&b->live,0,memory_order_release);
    atomic_store_explicit(&b->phase,FAULT,memory_order_release);
}
/* Sleep to an absolute time; 1 when the time has passed. */
static int sleep_until(uint64_t until)
{
    if(now_us()>=until)return 1;
    (void)sunstone_deadline_wait_until(until);
    return now_us()>=until;
}
static void delay_us(uint64_t d){uint64_t t=now_us()+d;while(!sleep_until(t));}
static int lock(uint64_t deadline)
{return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline)==1;}
static void unlock(void){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
/* Read-modify-write one register under the common register lock, with readback. */
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t next=(rd(a)&~mask)|(value&mask);
    wr(a,next);
    return (rd(a)&mask)==(value&mask);
}

/* --- interrupt ------------------------------------------------------------------------------------------------------ */
static void irq(unsigned n)
{
    struct bus *b=&buses[n];
    if(!atomic_load_explicit(&b->live,memory_order_acquire)){
        /* Not ours to serve: never leave a level line unserved. */
        atomic_fetch_add_explicit(&b->strays,1,memory_order_relaxed);
        (void)gicv3_spi_mask(&b->gic);
        return;
    }
    if(!a733_i2c_engine_irq(&b->engine))return;
    b->finished=ss_counter_ticks();
    b->result=(struct ss_i2c_result){(int32_t)b->engine.result,b->engine.last_status,b->engine.index,0,b->started,b->finished};
    atomic_store_explicit(&b->transfer,DONE,memory_order_release);
    BaseType_t woken=pdFALSE;
    os_application_wake_task_from_isr(b->waiter,&woken);
    portYIELD_FROM_ISR(woken);
}
void os_i2c_irq_twi0(void){irq(A733_I2C_TWI0);}
void os_i2c_irq_twi1(void){irq(A733_I2C_TWI1);}
void os_i2c_irq_twi2(void){irq(A733_I2C_TWI2);}
void os_i2c_irq_twi3(void){irq(A733_I2C_TWI3);}
void os_i2c_irq_s_twi2(void){irq(A733_I2C_S_TWI2);}

/* --- owner: open ---------------------------------------------------------------------------------------------------- */
/* The parent clock of the controller and its rate: APB1 (CCU+0x518) for the main controllers, APBS1 (R_CCU+0x10) for
 * S_TWI2. Both accept the 24 MHz oscillator; APB1 also accepts PERI0 (which the lease must then hold), APBS1 the
 * normalized PLL_REF. The register is read before the lease and compared after it. */
static int parent(struct bus *b,uint32_t *hz,a733_clock_mask_t *extra)
{
    const uint32_t v=rd(b->data->parent_register),xo=rd(0x07090160),ref=rd(CCU);
    *extra=0;
    b->inherited_parent=v;
    if(v&~UINT32_C(0x0700001f))return 0;
    const uint32_t source=v>>24,divider=(v&31)+1;
    uint32_t parent_hz;
    if(b->data->parent==A733_I2C_PARENT_APB1){
        if(source==0){if(!a733_clock_ref24_valid(xo,ref))return 0;parent_hz=24000000;}
        else if(source==3){
            if(!a733_clock_ref24_valid(xo,ref)||!a733_clock_peri0_valid(rd(CCU+0xa0)))return 0;
            parent_hz=600000000;*extra=A733_CLOCK_PERI0;
        }
        else return 0;
    }else{
        if(source==0){if((xo&UINT32_C(0x10000002))!=UINT32_C(0x10000002))return 0;parent_hz=a733_clock_dcxo_hz(xo);}
        else if(source==4){if(!a733_clock_ref24_valid(xo,ref))return 0;parent_hz=24000000;}
        else return 0;
    }
    if(parent_hz%divider)return 0;
    *hz=parent_hz/divider;return 1;
}
static struct a733_pin_config inherited_pin(unsigned bank,unsigned pin,int *ok)
{
    struct a733_pin_fields f;
    struct a733_pin_config c={0};
    if(!a733_pin_fields_banked(bank,pin,&f)){*ok=0;return c;}
    c.function=(rd(f.mux_address)>>f.mux_shift)&15;
    c.drive=(rd(f.drive_address)>>f.drive_shift)&15;
    c.pull=(rd(f.pull_address)>>f.pull_shift)&3;
    return c;
}
/* Poll a condition at short intervals on the precise timer, to a deadline. The conditions are controller bits that
 * settle within microseconds and raise no interrupt. */
static int settle(int (*condition)(struct bus *),struct bus *b,uint64_t deadline)
{
    while(!condition(b)){if(now_us()>=deadline)return 0;delay_us(20);}
    return 1;
}
static int reset_done(struct bus *b){return a733_i2c_engine_reset_done(&b->engine);}
static int idle(struct bus *b){return a733_i2c_engine_idle(&b->engine);}
static int controller_quiet(struct bus *b)
{
    const uintptr_t base=b->data->base;
    return !(rd(base+A733_I2C_CNTR)&0xfc)&&rd(base+A733_I2C_STAT)==A733_I2C_STAT_IDLE&&
        !(rd(base+A733_I2C_DRV_CTRL)&0x80000003)&&!(rd(base+A733_I2C_DRV_INT_CTRL)&3);
}
static void open_bus(struct bus *b,unsigned n)
{
    atomic_store_explicit(&b->phase,OPENING,memory_order_release);
    const uint64_t owner_id=OWNER_BASE+n+1,limit=lease_now()+50000,wall=now_us()+50000;
    b->lines_open=b->lines_close=unsampled;
    uint32_t input;a733_clock_mask_t extra;
    if(!parent(b,&input,&extra)){fail(b,F_PARENT,rd(b->data->parent_register));return;}
    uint32_t actual;
    if(!a733_i2c_clock_word(input,b->want_rate,&b->word,&actual)){fail(b,F_PARENT,input);return;}
    b->input_hz=input;b->rate=actual;
    a733_clock_mask_t shared=extra|(b->data->parent==A733_I2C_PARENT_APB1?A733_CLOCK_FABRIC:A733_CLOCK_APBS1);
    enum a733_clock_result c=A733_CLOCK_BUSY;
    while(c==A733_CLOCK_BUSY&&lease_now()<limit){
        c=a733_clock_acquire(&providers.shared.clocks,owner_id,shared,b->scope,limit,&b->clock);
        if(c==A733_CLOCK_BUSY)delay_us(50);
    }
    if(c!=A733_CLOCK_OK){fail(b,F_CLOCK,(uint32_t)c);return;}
    if(rd(b->data->parent_register)!=b->inherited_parent){fail(b,F_PARENT,rd(b->data->parent_register));return;}
    /* The controller must be unused: gate and reset closed, nothing running. An enabled controller is refused, not reset. */
    if(!lock(limit)){fail(b,F_GATE,1);return;}
    b->inherited_bgr=rd(b->data->bgr);
    const uint32_t both=b->data->gate|b->data->reset;
    int ok=!(b->inherited_bgr&both);
    unlock();
    if(!ok){fail(b,F_GATE,b->inherited_bgr);return;}
    /* The pins, with the inherited configuration as the expectation, then pull-up and the controller function. */
    int valid=1;
    const unsigned pin[2]={b->route->scl_pin,b->route->sda_pin};
    for(unsigned i=0;i<2;++i){
        struct a733_pin_config was=inherited_pin(b->route->bank,pin[i],&valid);
        b->requests[i]=(struct a733_pin_request_banked){.bank=b->route->bank,.pin=pin[i],.expected=was,
            .desired={.function=b->route->function,.pull=1,.drive=0}};
    }
    if(!valid){fail(b,F_PINS,0xffff);return;}
    enum a733_pins_result p=A733_PINS_BUSY;
    while(p==A733_PINS_BUSY&&lease_now()<limit){
        p=a733_pins_acquire_banked(&providers.shared.pins,owner_id,b->requests,2,&b->pins,limit);
        if(p==A733_PINS_BUSY)delay_us(50);
    }
    if(p!=A733_PINS_OK){fail(b,F_PINS,(uint32_t)p);return;}
    /* Manual 18.11.6.1: reset low then high, then the gate, with the pins already in function. */
    if(!lock(limit)){fail(b,F_GATE,2);return;}
    ok=update(b->data->bgr,b->data->reset,b->data->reset);
    unlock();
    if(!ok){fail(b,F_GATE,3);return;}
    delay_us(2);
    if(!lock(limit)){fail(b,F_GATE,4);return;}
    ok=update(b->data->bgr,b->data->gate,b->data->gate);
    unlock();
    if(!ok){fail(b,F_GATE,5);return;}
    /* Bring-up of the controller: quiet registers first, then soft reset and the rate. */
    if(!controller_quiet(b)){fail(b,F_IDLE,rd(b->data->base+A733_I2C_STAT));return;}
    a733_i2c_engine_init(&b->engine,&(struct a733_i2c_io){engine_read,engine_write,b});
    a733_i2c_engine_reset(&b->engine);
    if(!settle(reset_done,b,wall)){fail(b,F_RESET,rd(b->data->base+A733_I2C_SRST));return;}
    /* INT_EN stays set while idle: INT_FLAG, and so the interrupt line, rises only after a START request. */
    a733_i2c_engine_enable(&b->engine,b->word);
    if(!settle(idle,b,wall)){fail(b,F_IDLE,rd(b->data->base+A733_I2C_LCR)<<8|rd(b->data->base+A733_I2C_STAT));return;}
    b->lines_open=a733_i2c_engine_lines_high(&b->engine);b->opened=1;
    /* The interrupt: live first, then the line, then INT_EN. */
    atomic_store_explicit(&b->live,1,memory_order_release);
    int r=0;
    for(unsigned tries=0;tries<100&&!r;++tries){
        r=gicv3_spi_rearm(&b->gic);
        if(r<0)break;
        if(!r)delay_us(10);
    }
    if(r<=0){fail(b,F_GIC,(uint32_t)r);return;}
    wr(b->data->base+A733_I2C_CNTR,A733_I2C_CNTR_BUS_EN|A733_I2C_CNTR_INT_EN);
    atomic_store_explicit(&b->phase,OPEN,memory_order_release);
}

/* --- owner: close --------------------------------------------------------------------------------------------------- */
/* Abort an active transfer: inside the critical section no interrupt can end it at the same moment. */
static void finish_aborted(struct bus *b)
{
    taskENTER_CRITICAL();
    if(atomic_load_explicit(&b->transfer,memory_order_relaxed)==ACTIVE&&a733_i2c_engine_abort(&b->engine)){
        b->finished=ss_counter_ticks();
        b->result=(struct ss_i2c_result){(int32_t)b->engine.result,b->engine.last_status,b->engine.index,0,b->started,b->finished};
        atomic_store_explicit(&b->transfer,DONE,memory_order_release);
    }else if(atomic_load_explicit(&b->transfer,memory_order_relaxed)==ACTIVE)
        atomic_store_explicit(&b->transfer,DONE,memory_order_release);  /* the engine had ended it; the result is in place */
    taskEXIT_CRITICAL();
    os_application_wake_task(b->waiter);
}
static void close_bus(struct bus *b)
{
    atomic_store_explicit(&b->phase,CLOSING,memory_order_release);
    const uint64_t limit=lease_now()+50000,wall=now_us()+50000;
    if(atomic_load_explicit(&b->transfer,memory_order_acquire)==ACTIVE)finish_aborted(b);
    /* Interrupt source off at the controller, then at the distributor, and the settled distributor read. */
    wr(b->data->base+A733_I2C_CNTR,A733_I2C_CNTR_BUS_EN);
    atomic_store_explicit(&b->live,0,memory_order_release);
    int r=gicv3_spi_mask(&b->gic);
    for(unsigned tries=0;r==1&&tries<200;++tries){
        r=gicv3_spi_masked(&b->gic);
        if(r==1)break;
        if(r==0)delay_us(10);
    }
    if(r!=1){fail(b,F_CLOSE_GIC,(uint32_t)r);return;}
    if(!settle(idle,b,wall)){fail(b,F_CLOSE_IDLE,rd(b->data->base+A733_I2C_LCR)<<8|rd(b->data->base+A733_I2C_STAT));return;}
    b->lines_close=a733_i2c_engine_lines_high(&b->engine);
    wr(b->data->base+A733_I2C_CNTR,0);
    if(!lock(limit)){fail(b,F_CLOSE_GATE,1);return;}
    int ok=update(b->data->bgr,b->data->gate|b->data->reset,0);
    unlock();
    if(!ok){fail(b,F_CLOSE_GATE,2);return;}
    enum a733_pins_result p=a733_pins_release_banked(&providers.shared.pins,&b->pins,1,limit);
    if(p!=A733_PINS_OK){fail(b,F_CLOSE_PINS,(uint32_t)p);return;}
    if(a733_clock_release(&providers.shared.clocks,&b->clock,limit)!=A733_CLOCK_OK){fail(b,F_CLOSE_CLOCK,0);return;}
    b->clock=(struct a733_clock_token){0};
    atomic_store_explicit(&b->transfer,FREE,memory_order_release);
    atomic_store_explicit(&b->phase,CLOSED,memory_order_release);
}

/* --- owner: loop ---------------------------------------------------------------------------------------------------- */
static void start_transfer(struct bus *b)
{
    /* The previous STOP has long completed if the bus reads idle; a start before that is hardware-ordered, and counted. */
    if(a733_i2c_engine_idle(&b->engine))++b->idle_starts;else ++b->busy_starts;
    b->deadline_us=now_us()+b->timeout_us;
    b->started=ss_counter_ticks();
    atomic_store_explicit(&b->transfer,ACTIVE,memory_order_release);
    taskENTER_CRITICAL();
    int started=a733_i2c_engine_begin(&b->engine,(const struct a733_i2c_message *)b->messages,b->count);
    taskEXIT_CRITICAL();
    if(!started){
        b->result=(struct ss_i2c_result){(int32_t)SS_I2C_UNEXPECTED,0,0,0,b->started,b->started};
        atomic_store_explicit(&b->transfer,DONE,memory_order_release);
        os_application_wake_task(b->waiter);
    }
}
static void owner_task_body(void)
{
    for(;;){
        uint64_t next=now_us()+10000000;
        int all_closed=1;
        const int quit=atomic_load_explicit(&quiesce_wanted,memory_order_acquire);
        for(unsigned n=0;n<A733_I2C_BUS_COUNT;++n){
            struct bus *b=&buses[n];
            if(!(declared_mask&(1u<<n)))continue;
            unsigned phase=atomic_load_explicit(&b->phase,memory_order_acquire);
            unsigned command=atomic_exchange_explicit(&b->command,NO_COMMAND,memory_order_acq_rel);
            if(phase==CLOSED&&command==OPEN_COMMAND&&!quit){
                open_bus(b,n);phase=atomic_load_explicit(&b->phase,memory_order_acquire);os_application_wake_task(b->waiter);
            }else if(phase==OPEN&&(command==CLOSE_COMMAND||quit)){
                close_bus(b);phase=atomic_load_explicit(&b->phase,memory_order_acquire);os_application_wake_task(b->waiter);
            }
            if(phase==OPEN){
                if(atomic_load_explicit(&b->transfer,memory_order_acquire)==SUBMITTED)start_transfer(b);
                if(atomic_load_explicit(&b->transfer,memory_order_acquire)==ACTIVE){
                    if(now_us()>=b->deadline_us)finish_aborted(b);
                    else if(b->deadline_us<next)next=b->deadline_us;
                }
            }
            if(phase!=CLOSED)all_closed=0;
        }
        if(quit&&all_closed)atomic_store_explicit(&quiesced,1,memory_order_release);
        if(now_us()<next)(void)sunstone_deadline_wait_until(next);
    }
}
void os_i2c_task(void *unused)
{
    (void)unused;
    if(sunstone_deadline_register()!=1)for(;;)vTaskSuspend(NULL);
    owner_task_body();
}

/* --- quiesce and start ---------------------------------------------------------------------------------------------- */
static enum os_quiesce_result quiesce_step(struct os_quiesce *q)
{
    for(unsigned n=0;n<A733_I2C_BUS_COUNT;++n)
        if((declared_mask&(1u<<n))&&atomic_load_explicit(&buses[n].phase,memory_order_acquire)==FAULT){
            (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x911,
                n|buses[n].fault<<8|buses[n].fault_detail<<16,SUNSTONE_CRUMB_HERE());
            return OS_QUIESCE_FAULT;
        }
    if(atomic_load_explicit(&quiesced,memory_order_acquire))return OS_QUIESCE_DONE;
    atomic_store_explicit(&quiesce_wanted,1,memory_order_release);
    if(owner)xTaskNotifyGive(owner);
    return OS_QUIESCE_PENDING;
}
static struct os_quiesce i2c_quiesce={.owner=OS_QUIESCE_I2C,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_i2c_start(void)
{
    if(!ss_i2c_declaration_count)return 1;   /* touches nothing */
    if(!os_session_admitted()||!declarations_valid()||!os_quiesce_register(&i2c_quiesce))return 0;
    for(unsigned i=0;i<ss_i2c_declaration_count;++i){
        const unsigned n=ss_i2c_declarations[i].bus;
        struct bus *b=&buses[n];
        b->data=a733_i2c_bus_data(n);b->route=route_of(n);b->scope=scope_of(n);
        b->max_rate=ss_i2c_declarations[i].max_rate_hz;
        b->gic=(struct gicv3_spi){GICD,b->data->intid};
        declared_mask|=1u<<n;
    }
    owner=xTaskCreateStaticAffinitySet(os_i2c_task,"os-i2c",sizeof(owner_stack)/sizeof(owner_stack[0]),NULL,4,
        owner_stack,&owner_tcb,1);
    return owner!=NULL;
}

/* --- application calls ---------------------------------------------------------------------------------------------- */
static struct bus *granted(unsigned n)
{
    if(n>=A733_I2C_BUS_COUNT||!(declared_mask&(1u<<n))||!ss_grant()||atomic_load_explicit(&quiesce_wanted,memory_order_acquire))return NULL;
    return &buses[n];
}
static void kick(void){if(owner)xTaskNotifyGive(owner);}
int ss_i2c_open(unsigned n,uint32_t rate_hz)
{
    struct bus *b=granted(n);
    if(!b||!rate_hz||rate_hz>b->max_rate)return -1;
    switch(atomic_load_explicit(&b->phase,memory_order_acquire)){
    case OPEN:return 1;
    case FAULT:case CLOSING:return -1;
    case OPENING:return 0;
    default:break;
    }
    unsigned none=NO_COMMAND;
    b->want_rate=rate_hz;b->waiter=xTaskGetCurrentTaskHandle();
    if(atomic_compare_exchange_strong_explicit(&b->command,&none,OPEN_COMMAND,memory_order_acq_rel,memory_order_acquire))kick();
    return 0;
}
int ss_i2c_submit(unsigned n,const struct ss_i2c_message *m,uint32_t count,uint32_t timeout_us)
{
    struct bus *b=granted(n);
    if(!b||!m||!count||!timeout_us)return -1;
    const unsigned phase=atomic_load_explicit(&b->phase,memory_order_acquire);
    if(phase==FAULT)return -1;
    if(phase!=OPEN||atomic_load_explicit(&b->transfer,memory_order_acquire)!=FREE)return 0;
    for(uint32_t i=0;i<count;++i)
        if(m[i].address>0x7f||(m[i].flags&~SS_I2C_READ)||((m[i].flags&SS_I2C_READ)&&!m[i].length)||(m[i].length&&!m[i].data))return -1;
    b->messages=m;b->count=count;b->timeout_us=timeout_us;b->waiter=xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&b->transfer,SUBMITTED,memory_order_release);
    kick();
    return 1;
}
int ss_i2c_result(unsigned n,struct ss_i2c_result *out)
{
    struct bus *b=granted(n);
    if(!b||!out)return -1;
    const unsigned t=atomic_load_explicit(&b->transfer,memory_order_acquire);
    if(t==DONE){
        *out=b->result;
        atomic_store_explicit(&b->transfer,FREE,memory_order_release);
        return 1;
    }
    return t==FREE?-1:0;
}
static int any_done(void)
{
    for(unsigned n=0;n<A733_I2C_BUS_COUNT;++n)
        if((declared_mask&(1u<<n))&&atomic_load_explicit(&buses[n].transfer,memory_order_acquire)==DONE)return 1;
    return 0;
}
void ss_i2c_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,any_done)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x758);
}
int ss_i2c_info(unsigned n,struct ss_i2c_info *info)
{
    struct bus *b=n<A733_I2C_BUS_COUNT&&(declared_mask&(1u<<n))?&buses[n]:NULL;
    if(!b||!info||!ss_grant()||!b->opened)return -1;
    *info=(struct ss_i2c_info){n,b->rate,b->input_hz,b->word,b->engine.interrupts,b->engine.spurious,
        atomic_load_explicit(&b->strays,memory_order_relaxed),b->data->intid,b->idle_starts,b->busy_starts,
        b->lines_open,b->lines_close,b->fault,b->fault_detail};
    return 1;
}
int ss_i2c_close(unsigned n)
{
    struct bus *b=declared_mask&(1u<<n)&&n<A733_I2C_BUS_COUNT?&buses[n]:NULL;
    if(!b||!ss_grant())return -1;
    switch(atomic_load_explicit(&b->phase,memory_order_acquire)){
    case CLOSED:return 1;
    case FAULT:return -1;
    case CLOSING:return 0;
    default:break;
    }
    unsigned none=NO_COMMAND;
    b->waiter=xTaskGetCurrentTaskHandle();
    if(atomic_compare_exchange_strong_explicit(&b->command,&none,CLOSE_COMMAND,memory_order_acq_rel,memory_order_acquire))kick();
    return 0;
}
