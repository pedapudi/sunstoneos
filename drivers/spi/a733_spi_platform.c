#include "a733_spi_platform.h"
#define CCU UINT32_C(0x02002000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
#define PENDING A733_SPI_PLATFORM_PENDING
#define BUSY A733_SPI_PLATFORM_BUSY
#define READY A733_SPI_PLATFORM_READY
#define COMPLETE A733_SPI_PLATFORM_COMPLETE
#define CLOSED A733_SPI_PLATFORM_CLOSED
#define RETAINED A733_SPI_PLATFORM_RETAINED
#define INVALID A733_SPI_PLATFORM_INVALID
#define CLOCK_MASK UINT32_C(0x87001f1f)
enum { S_CLOCK,S_ADMIT,S_BASELINE,S_MASK_WAIT,S_ENABLE,S_HOLD_RESET,
    S_BEGIN,S_CONFIGURE,S_PIN_CHECK,S_PINS,S_READY,S_STOP_ADMIT,S_DRAIN,
    S_UNPIN,S_CLOSE,S_RESET,S_STOP_HOLD,S_RESTORE,S_UNCLOCK,S_CLOSED };
int a733_spi_route_plan(enum a733_spi_route id,unsigned cs,struct a733_spi_route_plan *out)
{
    static const struct {unsigned port,bank,function,clock,mosi,miso;uint8_t cs[4];} routes[]={
        {0,A733_PIN_C,5,12,2,4,{3,7,255,255}},
        {1,A733_PIN_D,6,11,12,13,{10,15,255,255}},
        {2,A733_PIN_B,4,1,2,3,{0,4,5,6}},
        {3,A733_PIN_E,5,1,2,3,{0,4,255,255}},
        {5,A733_PIN_L,4,5,6,7,{4,255,255,255}},
        {5,A733_PIN_M,3,1,2,3,{0,255,255,255}}
    };
    if(!out||(unsigned)id>=sizeof(routes)/sizeof(routes[0])||cs>3||routes[id].cs[cs]==255)return 0;
    out->port=routes[id].port;out->bank=routes[id].bank;out->function=routes[id].function;
    out->clock=routes[id].clock;out->mosi=routes[id].mosi;out->miso=routes[id].miso;out->cs=routes[id].cs[cs];return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static a733_clock_mask_t local(unsigned port)
{ return A733_CLOCK_SPI0<<port; }
static uintptr_t irq(struct a733_spi_platform *s,unsigned off)
{ return GIC+off+4*(s->spi.plan.intid/32); }
static uint32_t irqbit(struct a733_spi_platform *s)
{ return BIT(s->spi.plan.intid%32); }
static int time_ok(struct a733_spi_platform *s)
{
    uint64_t now;if(s->ops.now(s->context,&now)!=1||now<s->last_now||now>=s->deadline)return 0;
    s->last_now=now;return 1;
}
static enum a733_spi_platform_result fault(struct a733_spi_platform *s)
{
    s->fault=1;
    if(s->held){s->ops.retain(s->context);s->held=0;}
    if(s->clock.id&&!s->retained) {
        enum a733_clock_result r=a733_clock_retain(s->registry,&s->clock);
        if(r!=A733_CLOCK_BUSY)s->retained=1;
    }
    return RETAINED;
}
static enum a733_spi_platform_result finish(struct a733_spi_platform *s,enum a733_spi_platform_result r)
{
    if(s->held){s->ops.leave(s->context);s->held=0;}
    atomic_store_explicit(&s->busy,0,memory_order_release);return r;
}
static int take(struct a733_spi_platform *s)
{
    unsigned expected=0;return atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static int lock(struct a733_spi_platform *s)
{
    if(!time_ok(s))return -1;
    int r=s->ops.enter(s->context,s->deadline);if(r==0)return time_ok(s)?0:-1;
    if(r!=1)return -1;s->held=1;
    return time_ok(s)&&!s->pins->failed?1:-1;
}
static int rd(struct a733_spi_platform *s,uintptr_t a,uint32_t *v)
{ return time_ok(s)&&s->ops.read(s->context,a,v)==1&&time_ok(s); }
static int wr(struct a733_spi_platform *s,uintptr_t a,uint32_t v)
{ return time_ok(s)&&s->ops.write(s->context,a,v)==1&&time_ok(s); }
static int update(struct a733_spi_platform *s,uintptr_t a,uint32_t mask,uint32_t expected,uint32_t bits)
{
    uint32_t old,v;if(!rd(s,a,&old)||(old&mask)!=expected)return 0;
    uint32_t value=(old&~mask)|bits;
    return value==old||(wr(s,a,value)&&rd(s,a,&v)&&v==value);
}
static int parent(struct a733_spi_platform *s)
{
    uint32_t osc,ref;
    if(!rd(s,0x07090160,&osc)||(osc&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1))||
       (((osc>>14)&3)!=0&&((osc>>14)&3)!=3))return 0;
    if(s->route.port<5) {
        if(!rd(s,CCU,&ref)||(ref&0xf8000000u)!=0xf8000000u||
           ((ref>>8)&255)<79||((ref>>8)&255)>115||((ref>>8)&255)!=((ref>>16)&127))return 0;
    }
    return 1;
}
static int masked(struct a733_spi_platform *s)
{
    uint32_t v;if(!rd(s,GIC,&v))return -1;if(v&BIT(31))return 0;
    if(!rd(s,irq(s,0x100),&v))return -1;if(v&irqbit(s))return 0;
    if(!rd(s,irq(s,0x300),&v))return -1;return v&irqbit(s)?0:1;
}
static int profile(struct a733_spi_platform *s)
{
    uint32_t c,b;return parent(s)&&rd(s,s->spi.plan.clock,&c)&&c==s->spi.plan.clock_word&&
        rd(s,s->spi.plan.reset,&b)&&(b&0x10001)==0x10001&&masked(s)==1;
}
static int tokens(struct a733_spi_platform *s,unsigned pins)
{
    enum a733_clock_result c=a733_clock_token_status(s->registry,&s->clock);
    if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return -1;
    if(pins) {
        int held=0;enum a733_pins_result r=a733_pins_held_banked(s->pins,&s->pin_lease,&held,s->deadline);
        if(r==A733_PINS_BUSY)return 0;if(r!=A733_PINS_OK||!held)return -1;
    }
    return time_ok(s)?1:-1;
}
static int core_admit(void *p,const struct a733_spi_format *f,const struct a733_spi_plan *plan)
{
    struct a733_spi_platform *s=p;const struct a733_spi_format *c=&s->config.format;
    return !s->fault&&s->held&&s->core_call&&f->port==c->port&&f->cs==c->cs&&f->mode==c->mode&&
        f->lsb_first==c->lsb_first&&f->active_high==c->active_high&&f->max_hz==c->max_hz&&
        f->setup_us==c->setup_us&&f->hold_us==c->hold_us&&plan==&s->spi.plan&&profile(s);
}
static int core_read(void *p,uintptr_t a,uint32_t *v)
{struct a733_spi_platform *s=p;return s->held&&s->core_call&&!s->fault&&rd(s,a,v);}
static int core_write(void *p,uintptr_t a,uint32_t v)
{struct a733_spi_platform *s=p;return s->held&&s->core_call&&!s->fault&&wr(s,a,v);}
static int core_read8(void *p,uintptr_t a,uint8_t *v)
{
    struct a733_spi_platform *s=p;
    return s->held&&s->core_call&&!s->fault&&time_ok(s)&&s->ops.read8(s->context,a,v)==1&&time_ok(s);
}
static int core_write8(void *p,uintptr_t a,uint8_t v)
{
    struct a733_spi_platform *s=p;
    return s->held&&s->core_call&&!s->fault&&time_ok(s)&&s->ops.write8(s->context,a,v)==1&&time_ok(s);
}
static int core_now(void *p,uint64_t *t)
{struct a733_spi_platform *s=p;if(!s->held||!s->core_call||s->fault||!time_ok(s))return 0;*t=s->last_now;return 1;}
int a733_spi_platform_init(struct a733_spi_platform *s,const struct a733_spi_platform_config *c,
    const struct a733_spi_platform_ops *o,void *ctx,struct a733_clock_leases *registry,struct a733_pins *pins)
{
    struct a733_spi_route_plan route;struct a733_spi_plan plan;
    if(!s||s->initialized||!c||!o||!o->now||!o->current||!o->authorize||!o->enter||!o->leave||
       !o->retain||!o->read||!o->write||!o->read8||!o->write8||!registry||!registry->initialized||
       !pins||!pins->initialized||!c->owner||!c->handoff||!c->deadline||
       !a733_spi_route_plan(c->route,c->format.cs,&route)||route.port!=c->format.port||
       c->format.mode>3||c->format.active_high>1||c->format.lsb_first>1||
       a733_spi_plan(c->format.port,c->format.max_hz,&plan)||
       !separate(s,sizeof(*s),c,sizeof(*c))||!separate(s,sizeof(*s),o,sizeof(*o))||
       !separate(s,sizeof(*s),registry,sizeof(*registry))||!separate(s,sizeof(*s),pins,sizeof(*pins)))return 0;
    unsigned numbers[]={route.clock,route.mosi,route.miso,route.cs};
    for(unsigned i=0;i<4;i++) {
        if((c->expected[i].function!=0&&c->expected[i].function!=15)||c->desired[i].function!=route.function||
           !a733_pin_config_valid_banked(route.bank,numbers[i],c->expected[i])||
           !a733_pin_config_valid_banked(route.bank,numbers[i],c->desired[i]))return 0;
        s->requests[i]=(struct a733_pin_request_banked){.bank=route.bank,.pin=numbers[i],
            .expected=c->expected[i],.desired=c->desired[i]};
    }
    s->config=*c;s->ops=*o;s->route=route;s->spi.plan=plan;s->context=ctx;
    s->registry=registry;s->pins=pins;s->deadline=c->deadline;s->stage=S_CLOCK;
    atomic_init(&s->busy,0);s->initialized=1;return 1;
}
static int inactive(struct a733_spi_platform *s)
{
    uint32_t t,f,b,g,e;
    return rd(s,s->spi.plan.base+4,&g)&&g==0x83&&rd(s,s->spi.plan.base+8,&t)&&
        t==s->spi.tcr&&!(t&BIT(31))&&!!(t&0x80)!=s->config.format.active_high&&
        rd(s,s->spi.plan.base+0x10,&e)&&!e&&rd(s,s->spi.plan.base+0x1c,&f)&&!(f&0x00ff00ff)&&
        rd(s,s->spi.plan.base+0x400,&b)&&!(b&0x070700ff);
}
static enum a733_spi_platform_result metadata(struct a733_spi_platform *s)
{
    if(s->stage==S_CLOCK) {
        a733_clock_mask_t parents=A733_CLOCK_OSC|A733_CLOCK_FABRIC;
        if(s->route.port<5)parents|=A733_CLOCK_REF;
        enum a733_clock_result r=a733_clock_acquire(s->registry,s->config.owner,parents,local(s->route.port),s->deadline,&s->clock);
        if(r==A733_CLOCK_BUSY)return BUSY;if(r!=A733_CLOCK_OK)return fault(s);
        s->stage=S_ADMIT;
    } else if(s->stage==S_ADMIT||s->stage==S_STOP_ADMIT) {
        int r=s->ops.authorize(s->context,&s->config,&s->clock,
            s->stage==S_ADMIT?A733_SPI_PLATFORM_START:A733_SPI_PLATFORM_STOP);
        if(!time_ok(s))return fault(s);if(r==0)return PENDING;if(r!=1)return fault(s);
        s->stage=s->stage==S_ADMIT?S_BASELINE:S_DRAIN;
    } else if(s->stage==S_PINS) {
        enum a733_pins_result r=a733_pins_acquire_banked(s->pins,s->config.owner,s->requests,4,&s->pin_lease,s->deadline);
        if(!time_ok(s))return fault(s);if(r==A733_PINS_BUSY||r==A733_PINS_CONFLICT)return BUSY;
        if(r!=A733_PINS_OK)return fault(s);s->stage=S_READY;
    } else if(s->stage==S_UNPIN) {
        enum a733_pins_result r=a733_pins_release_banked(s->pins,&s->pin_lease,1,s->deadline);
        if(r==A733_PINS_OK){s->pins_released=1;s->stage=S_CLOSE;}
        if(!time_ok(s))return fault(s);if(r==A733_PINS_BUSY)return BUSY;
        if(r!=A733_PINS_OK)return fault(s);
    } else if(s->stage==S_UNCLOCK) {
        enum a733_clock_result r=a733_clock_release(s->registry,&s->clock,s->deadline);
        if(r==A733_CLOCK_BUSY)return BUSY;if(r!=A733_CLOCK_OK)return fault(s);
        s->clock=(struct a733_clock_token){0};s->stage=S_CLOSED;return CLOSED;
    } else return INVALID;
    return s->stage==S_READY?READY:PENDING;
}
static enum a733_spi_platform_result registers(struct a733_spi_platform *s,unsigned budget)
{
    int held=lock(s);if(held<0)return fault(s);if(!held)return BUSY;
    uint32_t v;int ok=parent(s);
    if(!ok)return fault(s);
    switch(s->stage) {
    case S_BASELINE:
        ok=rd(s,s->spi.plan.clock,&s->initial_clock)&&!(s->initial_clock&~0x07001f1fu)&&
           rd(s,s->spi.plan.reset,&s->initial_bus)&&!(s->initial_bus&0x10001)&&
           rd(s,irq(s,0x100),&v)&&!(v&irqbit(s))&&rd(s,irq(s,0x200),&v)&&!(v&irqbit(s))&&
           wr(s,irq(s,0x180),irqbit(s));
        if(ok)s->stage=S_MASK_WAIT;break;
    case S_MASK_WAIT:{int m=masked(s);ok=m>=0;if(m==1)s->stage=S_ENABLE;break;}
    case S_ENABLE:
        ok=update(s,s->spi.plan.clock,CLOCK_MASK,s->initial_clock&CLOCK_MASK,s->spi.plan.clock_word)&&
           update(s,s->spi.plan.reset,0x10001,0,1);
        if(ok){s->wait_since=s->last_now;s->stage=S_HOLD_RESET;}break;
    case S_HOLD_RESET:
        ok=rd(s,s->spi.plan.reset,&v)&&(v&0x10001)==1;
        if(ok&&s->last_now-s->wait_since>=10) {
            ok=update(s,s->spi.plan.reset,0x10001,1,0x10001);if(ok)s->stage=S_BEGIN;
        }break;
    case S_BEGIN:case S_CONFIGURE:{
        struct a733_spi_ops ops={core_admit,core_read,core_write,core_read8,core_write8,core_now};
        s->core_call=1;int r=s->stage==S_BEGIN?a733_spi_begin(&s->spi,&ops,s,&s->config.format,s->deadline):a733_spi_poll(&s->spi,budget);
        s->core_call=0;ok=r==A733_SPI_RESETTING||r==A733_SPI_FIFO_RESET||r==A733_SPI_READY;
        if(ok)s->stage=r==A733_SPI_READY?S_PIN_CHECK:S_CONFIGURE;break;}
    case S_PIN_CHECK:
        ok=profile(s)&&inactive(s);if(ok)s->stage=S_PINS;break;
    case S_READY:
        ok=profile(s);if(ok){s->core_call=1;int r=a733_spi_poll(&s->spi,budget);s->core_call=0;ok=r!=A733_SPI_FAULT&&r>=0;}break;
    case S_DRAIN:
        ok=profile(s)&&inactive(s)&&wr(s,irq(s,0x180),irqbit(s));
        if(ok)s->stage=S_UNPIN;break;
    case S_CLOSE:
        s->core_call=1;ok=a733_spi_close(&s->spi,s->deadline)==A733_SPI_CLOSED;s->core_call=0;
        if(ok)s->stage=S_RESET;break;
    case S_RESET:
        ok=masked(s)==1&&update(s,s->spi.plan.reset,0x10001,0x10001,1);
        if(ok){s->wait_since=s->last_now;s->stage=S_STOP_HOLD;}break;
    case S_STOP_HOLD:
        ok=rd(s,s->spi.plan.reset,&v)&&(v&0x10001)==1;
        if(ok&&s->last_now-s->wait_since>=10)s->stage=S_RESTORE;break;
    case S_RESTORE:
        ok=update(s,s->spi.plan.reset,0x10001,1,s->initial_bus&0x10001)&&
           update(s,s->spi.plan.clock,CLOCK_MASK,s->spi.plan.clock_word,s->initial_clock&CLOCK_MASK)&&
           wr(s,irq(s,0x280),irqbit(s))&&rd(s,irq(s,0x200),&v)&&!(v&irqbit(s))&&masked(s)==1;
        if(ok)s->stage=S_UNCLOCK;break;
    default:ok=0;break;
    }
    if(!ok||!time_ok(s)||s->fault)return fault(s);
    if(s->stage!=S_READY)return PENDING;
    return s->spi.state==A733_SPI_COMPLETE?COMPLETE:s->spi.state==A733_SPI_READY?READY:PENDING;
}
static int guard(struct a733_spi_platform *s)
{
    return tokens(s,s->pin_lease.id&&!s->pins_released);
}
enum a733_spi_platform_result a733_spi_platform_poll(struct a733_spi_platform *s,unsigned budget)
{
    if(!s||!s->initialized||!budget||budget>64)return INVALID;if(!take(s))return BUSY;
    if(s->fault)return finish(s,fault(s));if(s->ops.current(s->context)!=1)return finish(s,INVALID);
    if(s->stage==S_CLOSED)return finish(s,CLOSED);
    /* Idle status is cached. New commands supply fresh deadlines and validate
     * all live resources before acting; an expired prior command is not active. */
    if(s->stage==S_READY&&s->spi.state==A733_SPI_READY)return finish(s,READY);
    if(s->stage==S_READY&&s->spi.state==A733_SPI_COMPLETE)return finish(s,COMPLETE);
    if(!time_ok(s))return finish(s,fault(s));
    if(s->stage!=S_CLOCK) {int g=guard(s);if(g<=0)return finish(s,g==0?BUSY:fault(s));}
    enum a733_spi_platform_result r;
    if(s->stage==S_CLOCK||s->stage==S_ADMIT||s->stage==S_PINS||s->stage==S_STOP_ADMIT||s->stage==S_UNPIN||s->stage==S_UNCLOCK)r=metadata(s);
    else r=registers(s,budget);
    return finish(s,r);
}
static enum a733_spi_platform_result command(struct a733_spi_platform *s,const struct a733_spi_transfer *t,uint64_t deadline,unsigned op)
{
    if(!s||!s->initialized||!deadline)return INVALID;if(!take(s))return BUSY;
    if(s->fault)return finish(s,fault(s));
    if(s->ops.current(s->context)!=1||s->stage!=S_READY)return finish(s,INVALID);
    if((op!=1&&s->spi.state!=A733_SPI_READY)||(op==1&&s->spi.state!=A733_SPI_COMPLETE))return finish(s,INVALID);
    s->deadline=deadline;int g=guard(s);if(g<=0)return finish(s,g==0?BUSY:fault(s));
    int held=lock(s);if(held<0)return finish(s,fault(s));if(!held)return finish(s,BUSY);
    if(!profile(s))return finish(s,fault(s));
    if(op==2){s->stage=S_STOP_ADMIT;return finish(s,PENDING);}
    if(op==1) {
        /* All fallible admission and time observations precede buffer release.
         * Reap success is final; no later callback can claim retained buffers. */
        if(!time_ok(s))return finish(s,fault(s));
        return finish(s,a733_spi_reap(&s->spi)==A733_SPI_READY?READY:INVALID);
    }
    s->core_call=1;int r=a733_spi_submit(&s->spi,t,deadline);s->core_call=0;
    if(r<0)return finish(s,INVALID);if(r==A733_SPI_FAULT||!time_ok(s))return finish(s,fault(s));
    return finish(s,r==A733_SPI_READY?READY:PENDING);
}
enum a733_spi_platform_result a733_spi_platform_submit(struct a733_spi_platform *s,const struct a733_spi_transfer *t,uint64_t deadline)
{
    if(!s||!t||!separate(s,sizeof(*s),t,sizeof(*t)))return INVALID;
    const void *buffers[]={t->tx,t->rx};size_t lengths[]={t->tx_bytes,t->rx_bytes};
    for(unsigned i=0;i<2;i++)if(lengths[i]&&
        (!separate(s,sizeof(*s),buffers[i],lengths[i])||
         !separate(s->registry,sizeof(*s->registry),buffers[i],lengths[i])||
         !separate(s->pins,sizeof(*s->pins),buffers[i],lengths[i])))return INVALID;
    return command(s,t,deadline,0);
}
enum a733_spi_platform_result a733_spi_platform_reap(struct a733_spi_platform *s,uint64_t deadline)
{return command(s,0,deadline,1);}
enum a733_spi_platform_result a733_spi_platform_close(struct a733_spi_platform *s,uint64_t deadline)
{return command(s,0,deadline,2);}
