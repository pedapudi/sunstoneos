#include "a733_uart_platform.h"
#include "a733_clock_reference.h"
#define CCU UINT32_C(0x02002000)
#define RCCU UINT32_C(0x07010000)
#define GICD UINT32_C(0x03400000)
#define OSC UINT32_C(0x07090160)
#define BIT(n) (UINT32_C(1)<<(n))
#define PENDING A733_UART_PLATFORM_PENDING
#define BUSY A733_UART_PLATFORM_BUSY
#define READY A733_UART_PLATFORM_READY
#define CLOSED A733_UART_PLATFORM_CLOSED
#define RETAINED A733_UART_PLATFORM_RETAINED
#define INVALID A733_UART_PLATFORM_INVALID
/* Default polling mode stays masked. Deferred mode has an external mask-only
 * ISR; actual controller service remains exclusively in the owner task. */
enum { S_CLOCK, S_ADMIT, S_BASELINE, S_GIC_WAIT, S_PINS, S_GATE,
       S_RESET_HOLD, S_CONFIGURE, S_CONFIGURING, S_READY, S_STOP_ADMIT,
       S_STOP_MASK, S_STOP_GIC_WAIT, S_DRAIN, S_DRAINING, S_RESET,
       S_STOP_HOLD, S_GATE_OFF, S_UNPIN, S_UNCLOCK, S_CLOSED };
int a733_uart_route_plan(enum a733_uart_route id,struct a733_uart_route_plan *out)
{
    static const struct a733_uart_route_plan routes[]={
        {0,A733_PIN_B,3,0,1,0,0,0}, {0,A733_PIN_B,2,9,10,0,0,0},
        {0,A733_PIN_F,3,2,4,0,0,0}, {1,A733_PIN_G,2,6,7,8,9,1},
        {1,A733_PIN_E,8,12,13,0,0,0}, {2,A733_PIN_B,2,0,1,2,3,1},
        {3,A733_PIN_H,2,4,5,6,7,1}, {4,A733_PIN_G,3,6,7,8,9,1},
        {5,A733_PIN_H,2,13,14,15,16,1}, {6,A733_PIN_E,8,0,1,2,3,1},
        {7,A733_PIN_L,3,2,3,0,0,0}, {8,A733_PIN_L,2,2,3,0,0,0}
    };
    if (!out || (unsigned)id>=sizeof(routes)/sizeof(routes[0])) return 0;
    *out=routes[id]; return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static uintptr_t bus(const struct a733_uart_platform *s)
{ return s->route.port<7 ? CCU+0xe00+s->route.port*4 : RCCU+0x18c; }
static uint32_t gate(const struct a733_uart_platform *s)
{ return s->route.port<7 ? 1u : BIT(s->route.port-7); }
static uintptr_t parent(const struct a733_uart_platform *s)
{ return s->route.port<7 ? CCU+0x538 : RCCU+0x10; }
static uintptr_t gic(const struct a733_uart_platform *s,unsigned offset)
{ return GICD+offset+(s->intid/32)*4; }
static uint32_t irq_bit(const struct a733_uart_platform *s)
{ return BIT(s->intid%32); }
static a733_clock_mask_t local_scope(unsigned port)
{
    static const a733_clock_mask_t scopes[]={A733_CLOCK_UART0,A733_CLOCK_UART1,
        A733_CLOCK_UART2,A733_CLOCK_UART3,A733_CLOCK_UART4,A733_CLOCK_UART5,
        A733_CLOCK_UART6,A733_CLOCK_S_UART0,A733_CLOCK_S_UART1};
    return scopes[port];
}
static int time_ok(struct a733_uart_platform *s)
{
    uint64_t now;
    if (s->ops.now_us(s->cookie,&now)!=1 || now<s->last_now || now>=s->deadline) return 0;
    s->last_now=now; return 1;
}
static enum a733_uart_platform_result fault(struct a733_uart_platform *s)
{
    s->fault=1;
    if (s->held) { s->ops.retain(s->cookie); s->held=0; }
    if (s->clock.id && !s->retained) {
        enum a733_clock_result r=a733_clock_retain(s->registry,&s->clock);
        if (r!=A733_CLOCK_BUSY) s->retained=1;
    }
    return RETAINED;
}
static enum a733_uart_platform_result finish(struct a733_uart_platform *s,enum a733_uart_platform_result r)
{
    if (s->held) { s->ops.leave(s->cookie); s->held=0; }
    atomic_store_explicit(&s->busy,0,memory_order_release); return r;
}
static int lock(struct a733_uart_platform *s)
{
    if (!time_ok(s)) return -1;
    int r=s->ops.enter(s->cookie,s->deadline);
    if (r==0) return time_ok(s) ? 0 : -1;
    if (r!=1) return -1;
    s->held=1;
    return time_ok(s) ? 1 : -1;
}
static int rd(struct a733_uart_platform *s,uintptr_t a,uint32_t *v)
{ return time_ok(s) && s->ops.read(s->cookie,a,v)==1 && time_ok(s); }
static int wr(struct a733_uart_platform *s,uintptr_t a,uint32_t v)
{ return time_ok(s) && s->ops.write(s->cookie,a,v)==1 && time_ok(s); }
static int update(struct a733_uart_platform *s,uintptr_t a,uint32_t mask,uint32_t expected,uint32_t bits)
{
    uint32_t old,got;
    if (!rd(s,a,&old) || (old&mask)!=expected) return 0;
    uint32_t value=(old&~mask)|bits;
    return value==old || (wr(s,a,value) && rd(s,a,&got) && got==value);
}
static int clock_profile(struct a733_uart_platform *s)
{
    uint32_t osc,ref,v;
    if (!rd(s,OSC,&osc) || !rd(s,parent(s),&v) || (v&0x0700001fu)) return 0;
    /* Main source zero is normalized SYS_CLK24M. S_UART source zero is
     * raw DCXO, so only that consumer requires a 24 MHz oscillator. */
    if (s->route.port<7) {
        if (!rd(s,CCU,&ref) || !a733_clock_ref24_valid(osc,ref)) return 0;
    } else if ((osc&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1)) ||
               a733_clock_dcxo_hz(osc)!=24000000) return 0;
    return 1;
}
/* Mask propagation and no active handler are necessary observations. The
 * authenticated handoff additionally excludes other software/remote owners. */
static int masked(struct a733_uart_platform *s)
{
    uint32_t v;
    if (!rd(s,GICD,&v)) return -1;
    if (v&BIT(31)) return 0;
    if (!rd(s,gic(s,0x100),&v)) return -1;
    if (v&irq_bit(s)) return 0;
    if (!rd(s,gic(s,0x300),&v)) return -1;
    return (v&irq_bit(s)) ? 0 : 1;
}
static int token_live(struct a733_uart_platform *s)
{
    enum a733_clock_result r=a733_clock_token_status(s->registry,&s->clock);
    return r==A733_CLOCK_OK ? 1 : r==A733_CLOCK_BUSY ? 0 : -1;
}
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *s)
{
    if(!s||!s->initialized||s->ops.current(s->cookie)!=1)return INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return BUSY;
    return finish(s,fault(s));
}
static int uart_address(struct a733_uart_platform *s,uintptr_t a,unsigned writing)
{
    if (a<s->uart.plan.base || a>s->uart.plan.base+0xa4 || (a&3)) return 0;
    switch (a-s->uart.plan.base) {
    case 0:case 4:case 8:case 0xc:case 0x10:case 0xa4:return 1;
    case 0x14:case 0x18:case 0x7c:return !writing;
    default:return 0;
    }
}
/* The core owns UART register serialization. Independent protocol workers use
 * only the designated task. Provider callbacks must not reenter service.
 * Shared CCU/GIC setup holds the common mutex, while exclusive UART data I/O
 * does not take that global mutex for each FIFO byte. */
static int uart_access(struct a733_uart_platform *s,uintptr_t a,uint32_t *v,unsigned writing)
{
    if (s->fault || s->ops.current(s->cookie)!=1 || !uart_address(s,a,writing)) return 0;
    unsigned entered=0;
    if (!s->core_call) {
        unsigned expected=0;
        if (s->stage!=S_READY || !atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,
            memory_order_acquire,memory_order_relaxed)) return 0;
        entered=1;
    }
    int result=writing ? s->ops.write(s->cookie,a,*v) : s->ops.read(s->cookie,a,v);
    if (result!=1) fault(s);
    if (entered) atomic_store_explicit(&s->busy,0,memory_order_release);
    return result==1;
}
static int uart_read(void *cookie,uintptr_t a,uint32_t *v)
{ return uart_access(cookie,a,v,0); }
static int uart_write(void *cookie,uintptr_t a,uint32_t v)
{ return uart_access(cookie,a,&v,1); }
static int uart_now(void *cookie,uint64_t *v)
{
    struct a733_uart_platform *s=cookie;
    if (s->fault || s->ops.current(s->cookie)!=1) return 0;
    if (s->ops.now_us(s->cookie,v)!=1 || *v<s->uart.last_now || *v>=s->uart.deadline) {
        fault(s); return 0;
    }
    return 1;
}
int a733_uart_platform_init(struct a733_uart_platform *s,
    const struct a733_uart_platform_config *c,const struct a733_uart_platform_ops *o,
    void *cookie,struct a733_clock_leases *registry,struct a733_pins *pins)
{
    struct a733_uart_route_plan route; struct a733_uart_plan plan;
    if (!s || s->initialized || !c || !o || !o->now_us || !o->current || !o->authorize ||
        !o->enter || !o->leave || !o->retain || !o->read || !o->write || !registry || !registry->initialized ||
        !pins || !pins->initialized || !c->owner || !c->handoff || !c->startup_deadline_us ||
        !a733_uart_route_plan(c->route,&route) || c->format.port!=route.port ||
        c->deferred_irq>1 || c->format.clock_hz!=24000000 || (c->format.auto_flow && !route.flow) ||
        a733_uart_plan(&c->format,&plan)!=1 || !separate(s,sizeof(*s),c,sizeof(*c)) ||
        !separate(s,sizeof(*s),o,sizeof(*o)) || !separate(s,sizeof(*s),registry,sizeof(*registry)) ||
        !separate(s,sizeof(*s),pins,sizeof(*pins))) return 0;
    unsigned count=c->format.auto_flow ? 4 : 2;
    unsigned numbers[]={route.tx,route.rx,route.rts,route.cts};
    for (unsigned i=0;i<count;++i) {
        if (c->desired[i].function!=route.function ||
            (c->expected[i].function!=0 && c->expected[i].function!=15 && c->expected[i].function!=route.function) ||
            !a733_pin_config_valid_banked(route.bank,numbers[i],c->expected[i]) ||
            !a733_pin_config_valid_banked(route.bank,numbers[i],c->desired[i])) return 0;
    }
    s->config=*c; s->ops=*o; s->cookie=cookie; s->registry=registry; s->pins=pins;
    s->route=route; s->count=count; s->deadline=c->startup_deadline_us;
    s->intid=route.port<7 ? 34+route.port : 233+route.port-7;
    for (unsigned i=0;i<count;++i) s->requests[i]=(struct a733_pin_request_banked){
        route.bank,numbers[i],c->expected[i],c->desired[i],0,0,0};
    atomic_init(&s->busy,0); s->initialized=1; return 1;
}
static enum a733_uart_platform_result metadata(struct a733_uart_platform *s)
{
    if (s->stage==S_CLOCK) {
        a733_clock_mask_t parent_scope=s->route.port<7 ? A733_CLOCK_APB_UART : A733_CLOCK_APBS1;
        enum a733_clock_result r=a733_clock_acquire(s->registry,s->config.owner,parent_scope,
            local_scope(s->route.port),s->deadline,&s->clock);
        if (r==A733_CLOCK_BUSY) return BUSY;
        if (r!=A733_CLOCK_OK || !time_ok(s)) return fault(s);
        s->stage=S_ADMIT; return PENDING;
    }
    int live=token_live(s);
    if (!live) return BUSY;
    if (live<0) return fault(s);
    if (s->stage==S_ADMIT || s->stage==S_STOP_ADMIT) {
        enum a733_uart_admission phase=s->stage==S_ADMIT ? A733_UART_ADMIT_START : A733_UART_ADMIT_STOP;
        int r=s->ops.authorize(s->cookie,&s->config,&s->clock,phase);
        if (!time_ok(s) || (r!=0 && r!=1)) return fault(s);
        if (!r) return PENDING;
        s->stage=s->stage==S_ADMIT ? S_BASELINE : S_STOP_MASK;
        return PENDING;
    }
    if (s->stage==S_PINS) {
        enum a733_pins_result r=a733_pins_acquire_banked(s->pins,s->config.owner,s->requests,s->count,
            &s->pin_lease,s->deadline);
        if (!time_ok(s)) return fault(s);
        if (r==A733_PINS_BUSY || r==A733_PINS_CONFLICT) return BUSY;
        if (r!=A733_PINS_OK) return fault(s);
        s->stage=S_GATE; return PENDING;
    }
    if (s->stage==S_UNPIN) {
        enum a733_pins_result r=a733_pins_release_banked(s->pins,&s->pin_lease,1,s->deadline);
        /* The pin manager's successful release is final; do not resurrect its
         * invalidated token on a later time observation. Keep the clock lease. */
        if (r==A733_PINS_OK) s->stage=S_UNCLOCK;
        if (!time_ok(s)) return fault(s);
        if (r==A733_PINS_BUSY) return BUSY;
        return r==A733_PINS_OK ? PENDING : fault(s);
    }
    if (s->stage==S_UNCLOCK) {
        enum a733_clock_result r=a733_clock_release(s->registry,&s->clock,s->deadline);
        if (r==A733_CLOCK_BUSY) return BUSY;
        if (r!=A733_CLOCK_OK) return fault(s);
        /* Registry OK is the final linearization point, checked before its
         * mutation. No post-release callback can fabricate retained ownership. */
        s->clock=(struct a733_clock_token){0}; s->stage=S_CLOSED; return CLOSED;
    }
    return INVALID;
}
static enum a733_uart_platform_result registers(struct a733_uart_platform *s)
{
    int held=lock(s);
    if (held<0) return fault(s);
    if (!held) return BUSY;
    uint32_t g=gate(s),reset=g<<16,v;
    int ok=(!s->pin_lease.id || !s->pins->failed) && clock_profile(s);
    if (!ok) return fault(s);
    if (s->stage==S_BASELINE) {
        ok=rd(s,bus(s),&s->initial_bus) && !(s->initial_bus&(g|reset)) &&
           wr(s,gic(s,0x180),irq_bit(s));
        if (ok) s->stage=S_GIC_WAIT;
    } else if (s->stage==S_GIC_WAIT || s->stage==S_STOP_GIC_WAIT) {
        int r=masked(s); ok=r>=0;
        if (r==1) s->stage=s->stage==S_GIC_WAIT ? S_PINS : S_DRAIN;
    } else if (s->stage==S_GATE) {
        ok=update(s,bus(s),g|reset,0,g);
        if (ok) {s->wait_since=s->last_now;s->stage=S_RESET_HOLD;}
    } else if (s->stage==S_RESET_HOLD) {
        ok=rd(s,bus(s),&v) && (v&(g|reset))==g;
        if (ok && s->last_now-s->wait_since>=10) {
            ok=update(s,bus(s),g|reset,g,g|reset);
            if (ok) s->stage=S_CONFIGURE;
        }
    } else if (s->stage==S_CONFIGURE || s->stage==S_CONFIGURING) {
        ok=rd(s,bus(s),&v) && (v&(g|reset))==(g|reset);
        if (ok) {
            struct a733_uart_ops ops={uart_read,uart_write,uart_now};
            s->core_call=1;
            int r=s->stage==S_CONFIGURE ? a733_uart_begin(&s->uart,&ops,s,&s->config.format,s->deadline) :
                                       a733_uart_poll(&s->uart);
            s->core_call=0;
            ok=r==A733_UART_CONFIGURING || r==A733_UART_READY;
            if (ok) s->stage=r==A733_UART_READY ? S_READY : S_CONFIGURING;
        }
    } else if (s->stage==S_STOP_MASK) {
        ok=wr(s,gic(s,0x180),irq_bit(s));
        if (ok) s->stage=S_STOP_GIC_WAIT;
    } else if (s->stage==S_DRAIN || s->stage==S_DRAINING) {
        s->core_call=1;
        int r=s->stage==S_DRAIN ? a733_uart_drain(&s->uart,s->deadline) : a733_uart_poll(&s->uart);
        s->core_call=0;
        ok=r==A733_UART_DRAINING || r==A733_UART_DRAINED;
        if (ok) s->stage=r==A733_UART_DRAINED ? S_RESET : S_DRAINING;
    } else if (s->stage==S_RESET) {
        ok=masked(s)==1 && update(s,bus(s),g|reset,g|reset,g);
        if (ok) {s->wait_since=s->last_now;s->stage=S_STOP_HOLD;}
    } else if (s->stage==S_STOP_HOLD) {
        ok=rd(s,bus(s),&v) && (v&(g|reset))==g;
        if (ok && s->last_now-s->wait_since>=10) s->stage=S_GATE_OFF;
    } else if (s->stage==S_GATE_OFF) {
        ok=update(s,bus(s),g|reset,g,s->initial_bus&(g|reset)) &&
           wr(s,gic(s,0x280),irq_bit(s)) && rd(s,gic(s,0x200),&v) && !(v&irq_bit(s)) && masked(s)==1;
        if (ok) s->stage=S_UNPIN;
    } else ok=0;
    if (!ok || !time_ok(s) || s->fault) return fault(s);
    return s->stage==S_READY ? READY : PENDING;
}
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *s)
{
    if (!s || !s->initialized) return INVALID;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed)) return BUSY;
    if (s->fault) return finish(s,fault(s));
    if (s->ops.current(s->cookie)!=1) return finish(s,INVALID);
    if (s->stage==S_CLOSED) return finish(s,CLOSED);
    if (s->stage==S_READY) return finish(s,s->uart.state==A733_UART_READY ? READY : fault(s));
    if (!time_ok(s)) return finish(s,fault(s));
    enum a733_uart_platform_result r;
    if (s->stage==S_CLOCK || s->stage==S_ADMIT || s->stage==S_PINS ||
        s->stage==S_STOP_ADMIT || s->stage==S_UNPIN || s->stage==S_UNCLOCK) r=metadata(s);
    else {
        int live=token_live(s);
        r=live<0 ? fault(s) : !live ? BUSY : registers(s);
    }
    return finish(s,r);
}
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *s)
{
    return s && s->initialized && !s->fault && s->stage==S_READY &&
        s->uart.state==A733_UART_READY && s->ops.current(s->cookie)==1 ? &s->uart : 0;
}
enum a733_uart_platform_result a733_uart_platform_live(struct a733_uart_platform *s,uint64_t deadline)
{
    if (!s || !s->initialized || !deadline) return INVALID;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed)) return BUSY;
    if (s->fault) return finish(s,fault(s));
    if (s->ops.current(s->cookie)!=1 || s->stage!=S_READY) return finish(s,INVALID);
    s->deadline=deadline;
    if (!time_ok(s) || s->uart.state!=A733_UART_READY) return finish(s,fault(s));
    int live=token_live(s),held=0;
    if (live<0) return finish(s,fault(s));
    if (!live) return finish(s,BUSY);
    enum a733_pins_result r=a733_pins_held_banked(s->pins,&s->pin_lease,&held,deadline);
    if (r==A733_PINS_BUSY) return finish(s,BUSY);
    if (r!=A733_PINS_OK || !held || !time_ok(s)) return finish(s,fault(s));
    /* held_banked preserves ownership reporting after pin recovery. Read its
     * separate health state only under the same register mutex as all writers. */
    int locked=lock(s);
    if (locked<0) return finish(s,fault(s));
    if (!locked) return finish(s,BUSY);
    if (s->pins->failed || !time_ok(s)) return finish(s,fault(s));
    /* A completed synchronous UART call retains its old deadline. A borrowed
     * worker may first query its clock after firmware startup, before exchange
     * installs the next bound. Successful live admission authorizes that READY
     * boundary; protocol partial-frame/command deadlines remain unchanged. */
    s->uart.deadline=deadline;
    return finish(s,READY);
}
enum a733_uart_platform_result a733_uart_platform_irq_sync(struct a733_uart_platform *s,uint64_t deadline)
{
    if (!s || !s->initialized || s->config.deferred_irq!=1) return INVALID;
    enum a733_uart_platform_result r=a733_uart_platform_live(s,deadline);
    if (r!=READY) return r;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed)) return BUSY;
    int held=lock(s);
    if (held<0) return finish(s,fault(s));
    if (!held) return finish(s,BUSY);
    int complete=masked(s);
    return finish(s,complete<0 ? fault(s) : complete ? READY : PENDING);
}
enum a733_uart_platform_result a733_uart_platform_close(struct a733_uart_platform *s,unsigned quiet,uint64_t deadline)
{
    if (!s || !s->initialized || quiet!=1 || !deadline) return INVALID;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed)) return BUSY;
    if (s->fault) return finish(s,fault(s));
    if (s->ops.current(s->cookie)!=1 || s->stage!=S_READY || s->uart.state!=A733_UART_READY) return finish(s,INVALID);
    s->deadline=deadline;
    if (!time_ok(s)) return finish(s,fault(s));
    s->stage=S_STOP_ADMIT;
    return finish(s,PENDING);
}
