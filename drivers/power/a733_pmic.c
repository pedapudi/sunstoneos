/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_pmic.h"
#include "a733_clock_reference.h"
#include "a733_range.h"
#define TWI UINT32_C(0x07083000)
#define BIT(n) (UINT32_C(1)<<(n))
#define POLL_PHASES 8u /* Bus phases one poll may advance. */
enum { S_CLOCK,S_BASELINE,S_PINS,S_VERIFY,S_READY };
enum { FREE,QUEUED,ACTIVE,DONE,EXPIRED };
enum { P_START,P_WAIT_START,P_ADDRESS,P_WAIT_ADDRESS,P_REGISTER,P_WAIT_REGISTER,
    P_RESTART,P_WAIT_RESTART,P_READ_ADDRESS,P_WAIT_READ_ADDRESS,P_RECEIVE,
    P_WAIT_RECEIVE,P_DATA,P_STOP,P_WAIT_STOP,P_PUBLISH,P_WRITE,P_WAIT_WRITE };
#define WRITE_INDEX 6u /* Byte index of the selector write; 7 reads it back. */
struct profile { uint8_t enable,bit,selector,mask; };
static int profile(enum a733_rail rail,struct profile *p)
{
    switch(rail) {
    case A733_RAIL_DCDC1:*p=(struct profile){0x10,0,0x12,31};break;
    case A733_RAIL_DCDC2:*p=(struct profile){0x10,1,0x13,127};break;
    case A733_RAIL_DCDC3:*p=(struct profile){0x10,2,0x14,127};break;
    case A733_RAIL_DCDC4:*p=(struct profile){0x10,3,0x15,127};break;
    case A733_RAIL_DCDC5:*p=(struct profile){0x10,4,0x16,127};break;
    case A733_RAIL_DCDC8:*p=(struct profile){0x10,7,0x19,127};break;
    case A733_RAIL_DCDC9:*p=(struct profile){0x11,0,0x1a,127};break;
    case A733_RAIL_BLDO1:*p=(struct profile){0x20,6,0x2a,31};break;
    case A733_RAIL_BLDO2:*p=(struct profile){0x20,7,0x2b,31};break;
    case A733_RAIL_BLDO5:*p=(struct profile){0x21,2,0x2e,31};break;
    case A733_RAIL_CLDO1:*p=(struct profile){0x21,3,0x2f,31};break;
    case A733_RAIL_CLDO3:*p=(struct profile){0x21,5,0x31,31};break;
    case A733_RAIL_CLDO2:*p=(struct profile){0x21,4,0x30,31};break;
    case A733_RAIL_DC1SW2:*p=(struct profile){0x11,4,0x12,31};break;
    case A733_RAIL_CLDO5:*p=(struct profile){0x21,7,0x33,31};break;
    case A733_RAIL_ELDO6:*p=(struct profile){0x23,3,0x3f,63};break;
    default:return 0;
    }
    return 1;
}
int a733_pmic_decode(enum a733_rail rail,uint8_t enable,uint8_t selector,struct a733_pmic_snapshot *out)
{
    struct profile p;if(!out || !profile(rail,&p))return 0;
    unsigned code=selector&p.mask,mv=0;
    if(rail==A733_RAIL_DCDC1 || rail==A733_RAIL_DC1SW2) {if(code<=28)mv=1000+100*code;}
    else if(rail==A733_RAIL_BLDO1 || rail==A733_RAIL_BLDO2 || rail==A733_RAIL_BLDO5 || rail==A733_RAIL_CLDO1 || rail==A733_RAIL_CLDO2 || rail==A733_RAIL_CLDO3 || rail==A733_RAIL_CLDO5) {if(code<=29)mv=500+100*code;}
    else if(rail==A733_RAIL_ELDO6) {if(code<=40)mv=500+25*code;}
    else if(code<=((rail==A733_RAIL_DCDC8 || rail==A733_RAIL_DCDC9) ? 101u : 87u))
        mv=code<=70 ? 500+10*code : 1200+20*(code-70);
    *out=(struct a733_pmic_snapshot){.rail=rail,.enable=enable,.selector=selector,
        .enabled=!!(enable&BIT(p.bit)),.decoded=!!mv,.programmed_uv=mv*1000};return 1;
}
static int outside(struct a733_pmic *p,const void *v,size_t n)
{
    return a733_ranges_disjoint(v,n,p,sizeof(*p)) && a733_ranges_disjoint(v,n,p->clocks,sizeof(*p->clocks)) &&
        a733_ranges_disjoint(v,n,p->rails,sizeof(*p->rails)) && a733_ranges_disjoint(v,n,p->pins,sizeof(*p->pins)) &&
        a733_ranges_disjoint(v,n,p->rails->binding.gpu,sizeof(*p->rails->binding.gpu)) && p->ops.outside(p->cookie,v,n)==1;
}
static int enter(struct a733_pmic *p)
{
    unsigned expected=0;
    return p && p->self==p && atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,
        memory_order_acquire,memory_order_relaxed);
}
static enum a733_pmic_status leave(struct a733_pmic *p,enum a733_pmic_status r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_pmic_status fault(struct a733_pmic *p)
{
    if(p->held && !p->retained && p->ops.current(p->cookie)==1) {
        p->ops.retain(p->cookie);p->retained=1;
    }
    p->failed=1;a733_power_authority_set(&p->rails->authority,A733_POWER_PROVIDER_FAULT);
    return A733_PMIC_RETAINED;
}
/* Revocation check before each bus transaction and at publication: one
 * acquire load of the state word, which carries provider, clock registry and
 * rail ledger faults, plus the provider's own latch that a client can set.
 * The provider holds its S_TWI0 lease and the request's borrow until reap, so
 * only a fault can invalidate them. */
static int authorized(const struct a733_pmic *p)
{return !p->failed && a733_power_authority_admits(&p->rails->authority,A733_POWER_PMIC_TRANSFER);}
static int time_now(struct a733_pmic *p)
{
    uint64_t now;
    if(p->ops.now(p->cookie,&now)!=1 || !a733_monotonic_advance(now,&p->last_now)){fault(p);return 0;}
    return 1;
}
static int timely(struct a733_pmic *p)
{return time_now(p) && p->last_now<p->deadline;}
/* Register accesses make no time read: transfer() checks the deadline once
 * per bus phase, and admission and startup once after their reads. */
static int rd(struct a733_pmic *p,uintptr_t a,uint32_t *v)
{return p->ops.read(p->cookie,a,v)==1;}
static int wr(struct a733_pmic *p,uintptr_t a,uint32_t v)
{return p->ops.write(p->cookie,a,v)==1;}
static int match(struct a733_pmic *p,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(p,a,&v) && (v&mask)==expected;}
static struct a733_pmic_lifetime identity(struct a733_pmic *p)
{return (struct a733_pmic_lifetime){p,p->config.owner,p->config.identity};}
static int same(struct a733_pmic *p,const struct a733_pmic_lifetime *v)
{return v->provider==p && v->owner==p->config.owner && v->identity==p->config.identity;}
static int clock_live(struct a733_pmic *p)
{
    enum a733_clock_result r=a733_clock_token_status(p->clocks,&p->clock);
    if(r==A733_CLOCK_BUSY)return 0;
    return r==A733_CLOCK_OK && p->clock.owner==p->config.owner &&
        p->clock.exclusive==A733_CLOCK_S_TWI0 &&
        p->clock.shared==(A733_CLOCK_APBS0|A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC) ? 1 : -1;
}
static int bus_idle(struct a733_pmic *p)
{
    return match(p,TWI+0xc,0xff,0x40) && match(p,TWI+0x10,0xff,0xf8) &&
        match(p,TWI+0x20,0x35,0x30);
}
/* The S_TWI0 clock route and divisor: DCXO detection, APBS0/APBS1 sources,
 * PLL_REF when selected, and a 10–400 kHz bus rate. Startup records them; each
 * request admission requires them unchanged. */
static int route(struct a733_pmic *p)
{
    uint32_t osc,apbs,apbs0,ref,divider;
    if(!rd(p,0x07090160,&osc) || (osc&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1)) ||
       !rd(p,0x0701000c,&apbs0))return 0;
    apbs0&=UINT32_C(0x0700001f);
    unsigned source=apbs0>>24;
    if((source!=0 && source!=4) ||
       (p->stage!=S_BASELINE && apbs0!=p->apbs0) ||
       !rd(p,0x07010010,&apbs))return 0;
    apbs&=UINT32_C(0x0700001f);
    if((apbs!=0 && apbs!=UINT32_C(0x04000000)) ||
       (p->stage!=S_BASELINE && apbs!=p->apbs1))return 0;
    uint32_t dcxo=a733_clock_dcxo_hz(osc);
    if((!source||!apbs) && p->stage!=S_BASELINE && dcxo!=p->dcxo_hz)return 0;
    if(apbs || source==4) {
        if(!rd(p,0x02002000,&ref) || !a733_clock_ref24_valid(osc,ref))return 0;
    }
    if(!rd(p,TWI+0x14,&divider) || (divider&~0x1ffu))return 0;
    unsigned m=(divider>>3)&15,n=divider&7;
    unsigned denominator=(1u<<n)*(m+1)*((m==0 && (divider&0x180)==0x80) ? 11u : 10u);
    /* 10–400kHz supported read profile; no inherited divisor mutation. */
    uint32_t parent=apbs?24000000:a733_clock_dcxo_hz(osc);
    if((uint64_t)parent<UINT64_C(10000)*denominator ||
       (uint64_t)parent>UINT64_C(400000)*denominator)return 0;
    if(p->stage==S_BASELINE){
        p->apbs1=apbs;p->apbs0=apbs0;p->divider=divider;p->dcxo_hz=dcxo;
        /* One SCL period, rounded up: the service spins nine of them per byte. */
        p->bit_ns=(uint32_t)((UINT64_C(1000000000)*denominator+parent-1)/parent);
    }
    else if(divider!=p->divider)return 0;
    return 1;
}
/* Startup only: the route, the S_TWI0 gate and reset, idle bus, and every
 * autonomous engine, DMA request and interrupt source disabled. Only the
 * provider can change these while it holds S_TWI0, and the native adapter
 * writes no register among them. */
static int baseline(struct a733_pmic *p)
{
    return route(p) &&
       match(p,0x0701019c,0x10001,0x10001) &&
       match(p,0x07025000,0xff,0x22) && match(p,0x03400000,BIT(31),0) &&
       match(p,0x0340011c,BIT(11),0) && match(p,0x0340021c,BIT(11),0) &&
       match(p,0x0340031c,BIT(11),0) && bus_idle(p) &&
       match(p,TWI+0x18,1,0) && match(p,TWI+0x1c,UINT32_MAX,0) &&
       match(p,TWI+0x200,0x8f000003,0) && match(p,TWI+0x210,0xc3,0xc0) &&
       match(p,TWI+0x214,0x000f0003,0) && match(p,TWI+0x218,0x01000100,0) &&
       match(p,TWI+0x21c,0x007f007f,0) && match(p,TWI+0x314,0xffff,0) &&
       match(p,TWI+0x318,0xffffff,0) && match(p,TWI+0x31c,0xffffff,0);
}
int a733_pmic_init(struct a733_pmic *p,const struct a733_pmic_config *c,
    const struct a733_pmic_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_rail_leases *rails,struct a733_pins *pins)
{
    if(!p || p->self || !c || !o || !clocks || !rails || !pins || !clocks->initialized ||
       rails->self!=rails || rails->binding.clocks!=clocks || !rails->binding.gpu || !pins->initialized ||
       !c->owner || !c->identity || !c->startup_deadline || !o->now || !o->current || !o->enter ||
       !o->leave || !o->retain || !o->read || !o->write || !o->outside ||
       !a733_ranges_disjoint(p,sizeof(*p),c,sizeof(*c)) || !a733_ranges_disjoint(p,sizeof(*p),o,sizeof(*o)) ||
       !a733_ranges_disjoint(p,sizeof(*p),clocks,sizeof(*clocks)) || !a733_ranges_disjoint(p,sizeof(*p),rails,sizeof(*rails)) ||
       !a733_ranges_disjoint(p,sizeof(*p),pins,sizeof(*pins)) ||
       !a733_ranges_disjoint(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu)) ||
       o->outside(cookie,p,sizeof(*p))!=1)return 0;
    for(unsigned i=0;i<2;++i)if(c->pins[i].function!=2 ||
        !a733_pin_config_valid_banked(A733_PIN_L,i,c->pins[i]))return 0;
    *p=(struct a733_pmic){.self=p,.config=*c,.ops=*o,.cookie=cookie,.clocks=clocks,.rails=rails,
        .pins=pins,.next_id=1,.deadline=c->startup_deadline,.active=A733_PMIC_REQUESTS};
    atomic_init(&p->busy,0);atomic_init(&p->queued,0);return 1;
}
int a733_pmic_bind_service(struct a733_pmic *p,void (*kick)(void *),void *context,
    struct a733_power_domains *domains)
{
    if(!p || p->self!=p || !kick || !domains || p->kick || p->stage!=S_CLOCK)return 0;
    p->kick=kick;p->kick_context=context;p->domains=domains;return 1;
}
unsigned a733_pmic_work(const struct a733_pmic *p,uint32_t *wait_ns)
{
    /* A START, repeated START or STOP condition takes about two bit times;
     * a byte with its acknowledgement nine. */
    const int edge=p->phase==P_WAIT_START || p->phase==P_WAIT_RESTART || p->phase==P_WAIT_STOP;
    if(wait_ns)*wait_ns=(edge ? 2u : 9u)*p->bit_ns;
    /* fault() sets the state word after the plain failed flag; held and
     * retained change only on this task. */
    if(a733_power_authority_load(&p->rails->authority)&A733_POWER_PMIC_TRANSFER)
        return A733_PMIC_WORK_FAILED|(p->held && !p->retained ? A733_PMIC_WORK_RETAIN : 0);
    unsigned work=p->stage!=S_READY ? A733_PMIC_WORK_STARTUP : 0;
    if(atomic_load_explicit(&p->queued,memory_order_acquire))work|=A733_PMIC_WORK_QUEUED;
    if(p->active<A733_PMIC_REQUESTS)work|=A733_PMIC_WORK_ACTIVE|(p->waiting ? A733_PMIC_WORK_WAITING : 0);
    return work;
}
enum a733_pmic_status a733_pmic_identity(struct a733_pmic *p,struct a733_pmic_lifetime *out)
{
    if(!p || p->self!=p || !outside(p,out,sizeof(*out)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    *out=identity(p);return leave(p,p->failed ? A733_PMIC_RETAINED : A733_PMIC_READY);
}
/* No lock: the lifetime and bindings are immutable after init, and the state
 * word carries readiness and every fault that fault() records. */
enum a733_pmic_status a733_pmic_live(struct a733_pmic *p,const struct a733_pmic_lifetime *v)
{
    if(!p || p->self!=p || !v || !outside(p,v,sizeof(*v)) || !same(p,v))return A733_PMIC_INVALID;
    const unsigned word=a733_power_authority_load(&p->rails->authority);
    if(word&A733_POWER_PMIC_TRANSFER)return A733_PMIC_RETAINED;
    return word&A733_POWER_PROVIDER_READY ? A733_PMIC_READY : A733_PMIC_PENDING;
}
/* Only the CPU rails accept writes, and only within the pinned CPU operating
 * points' 800-1050 mV, so a wrong caller value cannot leave that range. */
static int writable(enum a733_rail which,uint8_t target)
{
    struct a733_pmic_snapshot s;
    return (which==A733_RAIL_DCDC3 || which==A733_RAIL_DCDC5) && target<=127 &&
        a733_pmic_decode(which,0,target,&s) && s.decoded &&
        s.programmed_uv>=800000 && s.programmed_uv<=1050000;
}
static enum a733_pmic_status submit(struct a733_pmic *p,uint64_t owner,
    const struct a733_rail_token *rail,enum a733_rail which,unsigned write,uint8_t expected,
    uint8_t target,uint64_t deadline,struct a733_pmic_request *out)
{
    struct profile f;
    if(!p || p->self!=p || !rail)return A733_PMIC_INVALID;
    const struct a733_rail_token parent=*rail;
    if(!owner || parent.owner!=owner || !profile(which,&f) ||
       !((write ? parent.exclusive : parent.shared|parent.exclusive)&A733_RAIL_BIT(which)) ||
       (write && (!writable(which,target) || expected>f.mask)) || !outside(p,rail,sizeof(*rail)) ||
       !outside(p,out,sizeof(*out)) || !a733_ranges_disjoint(rail,sizeof(*rail),out,sizeof(*out)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    if(p->failed)return leave(p,A733_PMIC_RETAINED);
    if(p->stage!=S_READY)return leave(p,A733_PMIC_PENDING);
    if(!time_now(p))return leave(p,fault(p));
    if(p->last_now>=deadline)return leave(p,A733_PMIC_EXPIRED);
    int live=clock_live(p);if(live!=1)return leave(p,live==0 ? A733_PMIC_BUSY : fault(p));
    unsigned slot=0;while(slot<A733_PMIC_REQUESTS && p->slots[slot].state!=FREE)++slot;
    if(slot==A733_PMIC_REQUESTS)return leave(p,A733_PMIC_BUSY);
    if(!p->next_id)return leave(p,A733_PMIC_RETAINED);
    struct a733_pmic_slot *s=&p->slots[slot];
    enum a733_rail_result r=a733_rail_borrow(p->rails,&parent,A733_RAIL_BIT(which),&s->borrow);
    if(r!=A733_RAIL_READY)return leave(p,r==A733_RAIL_BUSY ? A733_PMIC_BUSY :
        r==A733_RAIL_RETAINED ? fault(p) : A733_PMIC_INVALID);
    s->request=(struct a733_pmic_request){identity(p),p->next_id++,owner};
    s->snapshot=(struct a733_pmic_snapshot){.rail=which};s->deadline=deadline;s->state=QUEUED;
    s->write=write;s->expected=expected;s->target=target;
    atomic_fetch_add_explicit(&p->queued,1,memory_order_release);
    *out=s->request;leave(p,A733_PMIC_PENDING);
    if(p->kick)p->kick(p->kick_context);
    return A733_PMIC_PENDING;
}
enum a733_pmic_status a733_pmic_submit(struct a733_pmic *p,uint64_t owner,
    const struct a733_rail_token *rail,enum a733_rail which,uint64_t deadline,struct a733_pmic_request *out)
{return submit(p,owner,rail,which,0,0,0,deadline,out);}
enum a733_pmic_status a733_pmic_submit_write(struct a733_pmic *p,uint64_t owner,
    const struct a733_rail_token *rail,enum a733_rail which,uint8_t expected,uint8_t target,
    uint64_t deadline,struct a733_pmic_request *out)
{return submit(p,owner,rail,which,1,expected,target,deadline,out);}
static struct a733_pmic_slot *request(struct a733_pmic *p,const struct a733_pmic_request *r)
{
    if(!same(p,&r->lifetime) || !r->id)return 0;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)if(p->slots[i].state!=FREE &&
       p->slots[i].request.id==r->id && p->slots[i].request.owner==r->owner)return &p->slots[i];
    return 0;
}
enum a733_pmic_status a733_pmic_result(struct a733_pmic *p,const struct a733_pmic_request *r,
    struct a733_pmic_snapshot *out)
{
    if(!p || p->self!=p || !outside(p,r,sizeof(*r)) || !outside(p,out,sizeof(*out)) ||
       !a733_ranges_disjoint(r,sizeof(*r),out,sizeof(*out)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    struct a733_pmic_slot *s=request(p,r);if(!s)return leave(p,A733_PMIC_INVALID);
    if(p->failed)return leave(p,A733_PMIC_RETAINED);
    if(!authorized(p))return leave(p,fault(p));
    if(s->state==DONE){*out=s->snapshot;return leave(p,A733_PMIC_COMPLETE);}
    return leave(p,s->state==EXPIRED ? A733_PMIC_EXPIRED : A733_PMIC_PENDING);
}
enum a733_pmic_status a733_pmic_reap(struct a733_pmic *p,const struct a733_pmic_request *r)
{
    if(!p || p->self!=p || !outside(p,r,sizeof(*r)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    struct a733_pmic_slot *s=request(p,r);if(!s)return leave(p,A733_PMIC_INVALID);
    if(p->failed)return leave(p,A733_PMIC_RETAINED);
    if(s->state!=DONE && s->state!=EXPIRED)return leave(p,A733_PMIC_PENDING);
    enum a733_rail_result result=a733_rail_end_borrow(p->rails,&s->borrow);
    if(result!=A733_RAIL_READY)return leave(p,result==A733_RAIL_BUSY ? A733_PMIC_BUSY : fault(p));
    *s=(struct a733_pmic_slot){0};return leave(p,A733_PMIC_COMPLETE);
}
static int await_status(struct a733_pmic *p,unsigned expected)
{
    uint32_t control;if(!rd(p,TWI+0xc,&control))return -1;
    if(!(control&8))return 0;
    if((control&0xf4)!=0x40)return -1;
    if(!rd(p,TWI+0x10,&p->last_status) || p->last_status!=expected)return -1;
    return 1;
}
static enum a733_pmic_status phase(struct a733_pmic *p)
{
    struct a733_pmic_slot *s=&p->slots[p->active];struct profile f;profile(s->snapshot.rail,&f);
    const uint8_t regs[]={0xff,0x0e,0x0f,f.enable,f.selector,0xff,f.selector,f.selector};
    const uint8_t written=(uint8_t)((p->values[4]&~f.mask)|s->target);
    int ok=1;uint32_t value;
    switch(p->phase) {
    case P_START:case P_RESTART:ok=wr(p,TWI+0xc,0x68);break;
    case P_ADDRESS:case P_READ_ADDRESS:
        ok=wr(p,TWI+8,p->phase==P_ADDRESS ? 0x6c : 0x6d) && wr(p,TWI+0xc,0x48);break;
    case P_REGISTER:ok=wr(p,TWI+8,regs[p->byte_index]) && wr(p,TWI+0xc,0x48);break;
    case P_RECEIVE:ok=wr(p,TWI+0xc,0x48);break;
    case P_WAIT_START:ok=await_status(p,8);break;
    case P_WAIT_ADDRESS:ok=await_status(p,0x18);break;
    case P_WAIT_REGISTER:ok=await_status(p,0x28);break;
    case P_WAIT_RESTART:ok=await_status(p,0x10);break;
    case P_WAIT_READ_ADDRESS:ok=await_status(p,0x40);break;
    case P_WAIT_RECEIVE:ok=await_status(p,0x58);break;
    case P_WRITE:ok=wr(p,TWI+8,written) && wr(p,TWI+0xc,0x48);break;
    case P_WAIT_WRITE:ok=await_status(p,0x28);break;
    case P_DATA:if(!rd(p,TWI+8,&value))return fault(p);p->values[p->byte_index]=(uint8_t)value;break;
    case P_STOP:ok=wr(p,TWI+0xc,0x58);break;
    case P_WAIT_STOP:
        /* As the bootloader's twi_stop(): wait for M_STP to clear, then for
         * the idle status 0xf8, each bounded by the request deadline. The
         * STOP write cleared INT_FLAG and M_STA cleared after the START, so
         * CNTR then holds BUS_EN alone (A733 manual 18.11.10.4); any other
         * value is an error state. Line control is read after the final
         * STOP; a line held after an earlier STOP stops the next START. */
        if(!rd(p,TWI+0xc,&value))return fault(p);
        if(value&0x10)return A733_PMIC_PENDING;
        if((value&0xff)!=0x40 || !rd(p,TWI+0x10,&value))return fault(p);
        if((value&0xff)!=0xf8)return A733_PMIC_PENDING;
        if(p->byte_index+1==(s->write ? 8u : 6u) && !match(p,TWI+0x20,0x35,0x30))return fault(p);
        if((p->byte_index==0 && p->values[0]!=0) || (p->byte_index==1 && p->values[1]!=3) ||
           (p->byte_index==2 && p->values[2]!=0) || (p->byte_index==5 && p->values[5]!=0))return fault(p);
        /* A write needs the enabled output at the selector the client read.
         * After the data byte, only an exact readback ends the request. */
        if(s->write && p->byte_index==5 && (!(p->values[3]&BIT(f.bit)) ||
           (p->values[4]&f.mask)!=s->expected))return fault(p);
        if(p->byte_index==WRITE_INDEX)p->values[WRITE_INDEX]=written;
        if(p->byte_index==WRITE_INDEX+1 && p->values[WRITE_INDEX+1]!=p->values[WRITE_INDEX])return fault(p);
        if(++p->byte_index<(s->write ? 8u : 6u)){p->phase=P_START;return A733_PMIC_PENDING;}break;
    case P_PUBLISH: {
        /* The bus was idle after the last STOP; startup verified the rest. */
        if(!authorized(p) || !timely(p))return fault(p);
        struct a733_pmic_snapshot snapshot;
        a733_pmic_decode(s->snapshot.rail,p->values[3],p->values[s->write ? 7 : 4],&snapshot);
        snapshot.page_before=p->values[0];snapshot.chip=p->values[1];snapshot.revision=p->values[2];snapshot.page_after=p->values[5];
        p->ops.leave(p->cookie);p->held=0;
        if(!timely(p))return fault(p);
        s->snapshot=snapshot;s->state=DONE;p->active=A733_PMIC_REQUESTS;return A733_PMIC_READY;
    }
    default:return fault(p);
    }
    if(ok<0 || (!ok && p->phase!=P_WAIT_START && p->phase!=P_WAIT_ADDRESS &&
       p->phase!=P_WAIT_REGISTER && p->phase!=P_WAIT_RESTART && p->phase!=P_WAIT_READ_ADDRESS &&
       p->phase!=P_WAIT_RECEIVE && p->phase!=P_WAIT_WRITE))return fault(p);
    /* The write byte sends its data after the register index, then STOP. */
    if(ok>0)p->phase=p->phase==P_WAIT_REGISTER && p->byte_index==WRITE_INDEX ? P_WRITE :
        p->phase==P_WAIT_WRITE ? P_STOP : p->phase+1;
    return A733_PMIC_PENDING;
}
/* One poll advances consecutive bus phases while the I2C mutex stays held, so
 * a snapshot does not cost one scheduler tick per phase. The poll ends at the
 * first wait phase whose flag is still clear, recorded in waiting, and after
 * POLL_PHASES phases. The provider object is held only for those few register
 * accesses; result() and reap() callers on other cores see BUSY only then.
 * The state word is checked again before each further phase. */
static int starts(unsigned phase)
{
    return phase==P_START || phase==P_RESTART || phase==P_ADDRESS || phase==P_READ_ADDRESS ||
        phase==P_REGISTER || phase==P_RECEIVE || phase==P_WRITE || phase==P_STOP;
}
static enum a733_pmic_status transfer(struct a733_pmic *p)
{
    for(unsigned n=1;;++n) {
        const unsigned before=p->phase,index=p->byte_index;
        enum a733_pmic_status r=phase(p);
        if(r!=A733_PMIC_PENDING)return r;
        /* One time read per phase: the deadline and, after a phase-start
         * write, the time the service spins from. */
        if(!timely(p))return fault(p);
        const int moved=p->phase!=before || p->byte_index!=index;
        if(moved && starts(before))p->phase_at=p->last_now;
        if(!moved){p->waiting=1;return r;}
        if(n==POLL_PHASES)return r;
        if(!authorized(p))return fault(p);
    }
}
enum a733_pmic_status a733_pmic_poll(struct a733_pmic *p)
{
    if(!p || p->self!=p || p->ops.current(p->cookie)!=1)return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    p->waiting=0;
    if(p->failed)return leave(p,fault(p));
    if(!time_now(p))return leave(p,fault(p));
    if(p->stage!=S_READY && p->last_now>=p->deadline)return leave(p,fault(p));
    if(p->active<A733_PMIC_REQUESTS && p->last_now>=p->deadline)return leave(p,fault(p));
    if(p->stage==S_CLOCK) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,A733_CLOCK_APBS0|A733_CLOCK_APBS1,
            A733_CLOCK_S_TWI0,p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return leave(p,A733_PMIC_BUSY);
        if(r!=A733_CLOCK_OK)return leave(p,fault(p));p->stage=S_BASELINE;return leave(p,A733_PMIC_PENDING);
    }
    /* The registry is queried during startup; afterwards each request is
     * admitted against it once, and the state word reports revocation. */
    int live=p->stage==S_READY && p->active<A733_PMIC_REQUESTS ? (authorized(p) ? 1 : -1) : clock_live(p);
    if(live!=1)return leave(p,live==0 ? A733_PMIC_BUSY : fault(p));
    if(p->stage==S_PINS) {
        struct a733_pin_request_banked requests[2]={0};
        for(unsigned i=0;i<2;++i)requests[i]=(struct a733_pin_request_banked){
            .bank=A733_PIN_L,.pin=i,.expected=p->config.pins[i],.desired=p->config.pins[i]};
        enum a733_pins_result r=a733_pins_acquire_banked(p->pins,p->config.owner,requests,2,&p->pin,p->deadline);
        if(r==A733_PINS_BUSY)return leave(p,A733_PMIC_BUSY);
        if(r!=A733_PINS_OK)return leave(p,fault(p));p->stage=S_VERIFY;return leave(p,A733_PMIC_PENDING);
    }
    if(p->stage==S_BASELINE || p->stage==S_VERIFY) {
        if(p->ops.enter(p->cookie,p->deadline)!=1)return leave(p,A733_PMIC_BUSY);p->held=1;
        if(!baseline(p))return leave(p,fault(p));
        p->ops.leave(p->cookie);p->held=0;
        if(!timely(p))return leave(p,fault(p));
        p->stage=p->stage==S_BASELINE ? S_PINS : S_READY;
        if(p->stage==S_READY)a733_power_authority_set(&p->rails->authority,A733_POWER_PROVIDER_READY);
        return leave(p,p->stage==S_READY ? A733_PMIC_READY : A733_PMIC_PENDING);
    }
    /* The deadline of an active transfer was checked above. */
    if(p->active<A733_PMIC_REQUESTS)return leave(p,transfer(p));
    unsigned selected=A733_PMIC_REQUESTS;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)if(p->slots[i].state==QUEUED &&
       (selected==A733_PMIC_REQUESTS || p->slots[i].request.id<p->slots[selected].request.id))selected=i;
    if(selected==A733_PMIC_REQUESTS)return leave(p,A733_PMIC_READY);
    struct a733_pmic_slot *s=&p->slots[selected];p->deadline=s->deadline;
    if(p->last_now>=p->deadline){
        s->state=EXPIRED;atomic_fetch_sub_explicit(&p->queued,1,memory_order_relaxed);
        return leave(p,A733_PMIC_EXPIRED);
    }
    enum a733_rail_result r=a733_rail_borrow_status(p->rails,&s->borrow);
    if(r==A733_RAIL_BUSY)return leave(p,A733_PMIC_BUSY);
    if(r!=A733_RAIL_READY)return leave(p,fault(p));
    /* The pin check waits for REGISTER_LOCK and the bus for the I2C mutex.
     * Both waits run without the provider object, so a live() caller that
     * holds either lock never waits for this poll. Only this task changes the
     * pin lease, the deadline and a queued slot. The poll takes the object
     * back before it uses either result; if another caller holds it then, the
     * poll releases the untouched mutex and returns BUSY. */
    const uint64_t deadline=p->deadline;leave(p,A733_PMIC_BUSY);
    int held=0;enum a733_pins_result pins=a733_pins_held_banked(p->pins,&p->pin,&held,deadline);
    int bus=pins==A733_PINS_OK && held && p->ops.enter(p->cookie,deadline)==1;
    if(!enter(p)){if(bus)p->ops.leave(p->cookie);return A733_PMIC_BUSY;}
    if(bus && !authorized(p)){p->ops.leave(p->cookie);bus=0;}
    if(!authorized(p))return leave(p,fault(p));
    if(pins==A733_PINS_BUSY)return leave(p,A733_PMIC_BUSY);
    if(pins!=A733_PINS_OK || !held)return leave(p,fault(p));
    if(!bus)return leave(p,A733_PMIC_BUSY);
    p->held=1;
    /* Startup verified the gate, reset and engines, which only the provider
     * could change while it holds S_TWI0. Admission rereads the clock route
     * and divisor, which set the bus timing, and requires an idle bus. Each
     * STOP is followed by the idle control and status values, and the final
     * STOP of a request by the idle line state. */
    if(!route(p) || !bus_idle(p) || !timely(p))return leave(p,fault(p));
    p->active=selected;s->state=ACTIVE;p->byte_index=0;p->phase=P_START;
    atomic_fetch_sub_explicit(&p->queued,1,memory_order_relaxed);
    return leave(p,A733_PMIC_PENDING);
}
