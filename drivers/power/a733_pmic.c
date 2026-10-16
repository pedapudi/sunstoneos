/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_pmic.h"
#define TWI UINT32_C(0x07083000)
#define BIT(n) (UINT32_C(1)<<(n))
enum { S_CLOCK,S_BASELINE,S_PINS,S_VERIFY,S_READY };
enum { FREE,QUEUED,ACTIVE,DONE,EXPIRED };
enum { P_START,P_WAIT_START,P_ADDRESS,P_WAIT_ADDRESS,P_REGISTER,P_WAIT_REGISTER,
    P_RESTART,P_WAIT_RESTART,P_READ_ADDRESS,P_WAIT_READ_ADDRESS,P_RECEIVE,
    P_WAIT_RECEIVE,P_DATA,P_STOP,P_WAIT_STOP,P_PUBLISH };
struct profile { uint8_t enable,bit,selector,mask; };
static int profile(enum a733_rail rail,struct profile *p)
{
    switch(rail) {
    case A733_RAIL_DCDC1:*p=(struct profile){0x10,0,0x12,31};break;
    case A733_RAIL_DCDC2:*p=(struct profile){0x10,1,0x13,127};break;
    case A733_RAIL_DCDC4:*p=(struct profile){0x10,3,0x15,127};break;
    case A733_RAIL_DCDC8:*p=(struct profile){0x10,7,0x19,127};break;
    case A733_RAIL_BLDO5:*p=(struct profile){0x21,2,0x2e,31};break;
    case A733_RAIL_CLDO1:*p=(struct profile){0x21,3,0x2f,31};break;
    case A733_RAIL_DC1SW2:*p=(struct profile){0x11,4,0x12,31};break;
    case A733_RAIL_CLDO5:*p=(struct profile){0x21,7,0x33,31};break;
    default:return 0;
    }
    return 1;
}
int a733_pmic_decode(enum a733_rail rail,uint8_t enable,uint8_t selector,struct a733_pmic_snapshot *out)
{
    struct profile p;if(!out || !profile(rail,&p))return 0;
    unsigned code=selector&p.mask,mv=0;
    if(rail==A733_RAIL_DCDC1 || rail==A733_RAIL_DC1SW2) {if(code<=28)mv=1000+100*code;}
    else if(rail==A733_RAIL_BLDO5 || rail==A733_RAIL_CLDO1 || rail==A733_RAIL_CLDO5) {if(code<=29)mv=500+100*code;}
    else if(code<=(rail==A733_RAIL_DCDC8 ? 101u : 87u))
        mv=code<=70 ? 500+10*code : 1200+20*(code-70);
    *out=(struct a733_pmic_snapshot){.rail=rail,.enable=enable,.selector=selector,
        .enabled=!!(enable&BIT(p.bit)),.decoded=!!mv,.programmed_uv=mv*1000};return 1;
}
static int separate(const void *p,size_t n,const void *q,size_t k)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)q;
    return p && q && n<=UINTPTR_MAX-a && k<=UINTPTR_MAX-b && (a+n<=b || b+k<=a);
}
static int outside(struct a733_pmic *p,const void *v,size_t n)
{
    return separate(v,n,p,sizeof(*p)) && separate(v,n,p->clocks,sizeof(*p->clocks)) &&
        separate(v,n,p->rails,sizeof(*p->rails)) && separate(v,n,p->pins,sizeof(*p->pins)) &&
        separate(v,n,p->rails->binding.gpu,sizeof(*p->rails->binding.gpu)) && p->ops.outside(p->cookie,v,n)==1;
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
    p->failed=1;return A733_PMIC_RETAINED;
}
static int time_now(struct a733_pmic *p)
{
    uint64_t now;
    if(p->ops.now(p->cookie,&now)!=1 || now<p->last_now){fault(p);return 0;}
    p->last_now=now;return 1;
}
static int timely(struct a733_pmic *p)
{return time_now(p) && p->last_now<p->deadline;}
static int rd(struct a733_pmic *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int wr(struct a733_pmic *p,uintptr_t a,uint32_t v)
{return timely(p) && p->ops.write(p->cookie,a,v)==1 && timely(p);}
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
        p->clock.shared==(A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC) ? 1 : -1;
}
static int bus_idle(struct a733_pmic *p)
{
    return match(p,TWI+0xc,0xff,0x40) && match(p,TWI+0x10,0xff,0xf8) &&
        match(p,TWI+0x20,0x35,0x30);
}
static int baseline(struct a733_pmic *p)
{
    uint32_t osc,divider;
    if(!rd(p,0x07090160,&osc) || (osc&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1)) ||
       (((osc>>14)&3)!=0 && ((osc>>14)&3)!=3) ||
       !match(p,0x07010010,0x0700001f,0) || !match(p,0x0701019c,0x10001,0x10001) ||
       !match(p,0x07025000,0xff,0x22) || !match(p,0x03400000,BIT(31),0) ||
       !match(p,0x0340011c,BIT(11),0) || !match(p,0x0340021c,BIT(11),0) ||
       !match(p,0x0340031c,BIT(11),0) || !bus_idle(p) ||
       !match(p,TWI+0x18,1,0) || !match(p,TWI+0x1c,UINT32_MAX,0) ||
       !match(p,TWI+0x200,0x8f000003,0) || !match(p,TWI+0x210,0xc3,0xc0) ||
       !match(p,TWI+0x214,0x000f0003,0) || !match(p,TWI+0x218,0x01000100,0) ||
       !match(p,TWI+0x21c,0x007f007f,0) || !match(p,TWI+0x314,0xffff,0) ||
       !match(p,TWI+0x318,0xffffff,0) || !match(p,TWI+0x31c,0xffffff,0) ||
       !rd(p,TWI+0x14,&divider) || (divider&~0x1ffu))return 0;
    unsigned m=(divider>>3)&15,n=divider&7;
    unsigned denominator=(1u<<n)*(m+1)*((m==0 && (divider&0x180)==0x80) ? 11u : 10u);
    /* 10–400kHz supported read profile; no inherited divisor mutation. */
    if(denominator<60 || denominator>2400)return 0;
    if(p->stage==S_BASELINE)p->divider=divider;
    else if(divider!=p->divider)return 0;
    return 1;
}
int a733_pmic_init(struct a733_pmic *p,const struct a733_pmic_config *c,
    const struct a733_pmic_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_rail_leases *rails,struct a733_pins *pins)
{
    if(!p || p->self || !c || !o || !clocks || !rails || !pins || !clocks->initialized ||
       rails->self!=rails || rails->binding.clocks!=clocks || !rails->binding.gpu || !pins->initialized ||
       !c->owner || !c->identity || !c->startup_deadline || !o->now || !o->current || !o->enter ||
       !o->leave || !o->retain || !o->read || !o->write || !o->outside ||
       !separate(p,sizeof(*p),c,sizeof(*c)) || !separate(p,sizeof(*p),o,sizeof(*o)) ||
       !separate(p,sizeof(*p),clocks,sizeof(*clocks)) || !separate(p,sizeof(*p),rails,sizeof(*rails)) ||
       !separate(p,sizeof(*p),pins,sizeof(*pins)) ||
       !separate(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu)) ||
       o->outside(cookie,p,sizeof(*p))!=1)return 0;
    for(unsigned i=0;i<2;++i)if(c->pins[i].function!=2 ||
        !a733_pin_config_valid_banked(A733_PIN_L,i,c->pins[i]))return 0;
    *p=(struct a733_pmic){.self=p,.config=*c,.ops=*o,.cookie=cookie,.clocks=clocks,.rails=rails,
        .pins=pins,.next_id=1,.deadline=c->startup_deadline,.active=A733_PMIC_REQUESTS};
    atomic_init(&p->busy,0);return 1;
}
enum a733_pmic_status a733_pmic_identity(struct a733_pmic *p,struct a733_pmic_lifetime *out)
{
    if(!p || p->self!=p || !outside(p,out,sizeof(*out)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    *out=identity(p);return leave(p,p->failed ? A733_PMIC_RETAINED : A733_PMIC_READY);
}
enum a733_pmic_status a733_pmic_live(struct a733_pmic *p,const struct a733_pmic_lifetime *v)
{
    if(!p || p->self!=p || !v || !outside(p,v,sizeof(*v)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    if(!same(p,v))return leave(p,A733_PMIC_INVALID);
    if(p->failed)return leave(p,A733_PMIC_RETAINED);
    if(p->stage!=S_READY)return leave(p,A733_PMIC_PENDING);
    int live=clock_live(p);
    return leave(p,live==1 ? A733_PMIC_READY : live==0 ? A733_PMIC_BUSY : fault(p));
}
enum a733_pmic_status a733_pmic_submit(struct a733_pmic *p,uint64_t owner,
    const struct a733_rail_token *rail,enum a733_rail which,uint64_t deadline,struct a733_pmic_request *out)
{
    struct profile f;
    if(!p || p->self!=p || !rail)return A733_PMIC_INVALID;
    const struct a733_rail_token parent=*rail;
    if(!owner || parent.owner!=owner || !profile(which,&f) ||
       !(parent.shared&A733_RAIL_BIT(which)) || !outside(p,rail,sizeof(*rail)) ||
       !outside(p,out,sizeof(*out)) || !separate(rail,sizeof(*rail),out,sizeof(*out)))return A733_PMIC_INVALID;
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
    *out=s->request;return leave(p,A733_PMIC_PENDING);
}
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
       !separate(r,sizeof(*r),out,sizeof(*out)))return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    struct a733_pmic_slot *s=request(p,r);if(!s)return leave(p,A733_PMIC_INVALID);
    if(p->failed)return leave(p,A733_PMIC_RETAINED);
    enum a733_rail_result status=a733_rail_borrow_status(p->rails,&s->borrow);
    if(status!=A733_RAIL_READY)return leave(p,status==A733_RAIL_BUSY ? A733_PMIC_BUSY : fault(p));
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
static enum a733_pmic_status transfer(struct a733_pmic *p)
{
    struct a733_pmic_slot *s=&p->slots[p->active];struct profile f;profile(s->snapshot.rail,&f);
    const uint8_t regs[]={0xff,0x0e,0x0f,f.enable,f.selector,0xff};
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
    case P_DATA:if(!rd(p,TWI+8,&value))return fault(p);p->values[p->byte_index]=(uint8_t)value;break;
    case P_STOP:ok=wr(p,TWI+0xc,0x58);break;
    case P_WAIT_STOP:
        if(!rd(p,TWI+0xc,&value))return fault(p);
        if(value&0x10)return A733_PMIC_PENDING;
        if(!bus_idle(p))return fault(p);
        if((p->byte_index==0 && p->values[0]!=0) || (p->byte_index==1 && p->values[1]!=3) ||
           (p->byte_index==2 && p->values[2]!=0) || (p->byte_index==5 && p->values[5]!=0))return fault(p);
        if(++p->byte_index<6){p->phase=P_START;return A733_PMIC_PENDING;}break;
    case P_PUBLISH: {
        enum a733_rail_result r=a733_rail_borrow_status(p->rails,&s->borrow);
        if(r==A733_RAIL_BUSY)return A733_PMIC_BUSY;
        if(r!=A733_RAIL_READY || !baseline(p) || !timely(p))return fault(p);
        struct a733_pmic_snapshot snapshot;
        a733_pmic_decode(s->snapshot.rail,p->values[3],p->values[4],&snapshot);
        snapshot.page_before=p->values[0];snapshot.chip=p->values[1];snapshot.revision=p->values[2];snapshot.page_after=p->values[5];
        p->ops.leave(p->cookie);p->held=0;
        if(!timely(p))return fault(p);
        s->snapshot=snapshot;s->state=DONE;p->active=A733_PMIC_REQUESTS;return A733_PMIC_READY;
    }
    default:return fault(p);
    }
    if(ok<0 || (!ok && p->phase!=P_WAIT_START && p->phase!=P_WAIT_ADDRESS &&
       p->phase!=P_WAIT_REGISTER && p->phase!=P_WAIT_RESTART && p->phase!=P_WAIT_READ_ADDRESS &&
       p->phase!=P_WAIT_RECEIVE))return fault(p);
    if(ok>0)++p->phase;
    return A733_PMIC_PENDING;
}
enum a733_pmic_status a733_pmic_poll(struct a733_pmic *p)
{
    if(!p || p->self!=p || p->ops.current(p->cookie)!=1)return A733_PMIC_INVALID;
    if(!enter(p))return A733_PMIC_BUSY;
    if(p->failed)return leave(p,fault(p));
    if(!time_now(p))return leave(p,fault(p));
    if(p->stage!=S_READY && p->last_now>=p->deadline)return leave(p,fault(p));
    if(p->active<A733_PMIC_REQUESTS && p->last_now>=p->deadline)return leave(p,fault(p));
    if(p->stage==S_CLOCK) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,A733_CLOCK_APBS1,
            A733_CLOCK_S_TWI0,p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return leave(p,A733_PMIC_BUSY);
        if(r!=A733_CLOCK_OK)return leave(p,fault(p));p->stage=S_BASELINE;return leave(p,A733_PMIC_PENDING);
    }
    int live=clock_live(p);
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
        return leave(p,p->stage==S_READY ? A733_PMIC_READY : A733_PMIC_PENDING);
    }
    if(p->active<A733_PMIC_REQUESTS) {
        if(!timely(p))return leave(p,fault(p));
        enum a733_rail_result r=a733_rail_borrow_status(p->rails,&p->slots[p->active].borrow);
        if(r==A733_RAIL_BUSY)return leave(p,A733_PMIC_BUSY);
        if(r!=A733_RAIL_READY)return leave(p,fault(p));
        return leave(p,transfer(p));
    }
    unsigned selected=A733_PMIC_REQUESTS;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)if(p->slots[i].state==QUEUED &&
       (selected==A733_PMIC_REQUESTS || p->slots[i].request.id<p->slots[selected].request.id))selected=i;
    if(selected==A733_PMIC_REQUESTS)return leave(p,A733_PMIC_READY);
    struct a733_pmic_slot *s=&p->slots[selected];p->deadline=s->deadline;
    if(p->last_now>=p->deadline){s->state=EXPIRED;return leave(p,A733_PMIC_EXPIRED);}
    int held=0;enum a733_pins_result pins=a733_pins_held_banked(p->pins,&p->pin,&held,p->deadline);
    if(pins==A733_PINS_BUSY)return leave(p,A733_PMIC_BUSY);
    if(pins!=A733_PINS_OK || !held)return leave(p,fault(p));
    enum a733_rail_result r=a733_rail_borrow_status(p->rails,&s->borrow);
    if(r==A733_RAIL_BUSY)return leave(p,A733_PMIC_BUSY);
    if(r!=A733_RAIL_READY)return leave(p,fault(p));
    if(p->ops.enter(p->cookie,p->deadline)!=1)return leave(p,A733_PMIC_BUSY);p->held=1;
    if(!baseline(p))return leave(p,fault(p));
    p->active=selected;s->state=ACTIVE;p->byte_index=0;p->phase=P_START;
    return leave(p,A733_PMIC_PENDING);
}
