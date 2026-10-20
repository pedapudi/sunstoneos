#include "a733_dma_platform.h"
#include "a733_clock_reference.h"

#define CCU UINT32_C(0x02002000)
#define DMA UINT32_C(0x04601000)
#define OSC UINT32_C(0x07090160)
#define BUS (CCU+0x704u)
#define MBUS_GATE (CCU+0x5e4u)
#define CRITICAL UINT32_C(0x40302)
#define BIT(n) (UINT32_C(1)<<(n))
#define READY A733_DMA_PLATFORM_READY
#define BUSY A733_DMA_PLATFORM_BUSY
#define PENDING A733_DMA_PLATFORM_PENDING
#define RETAINED A733_DMA_PLATFORM_RETAINED
enum { ACQUIRE, BASELINE, RESET_WAIT, ENABLE, CONTROLLER, COMPLETE };
static const uint32_t fabric_offsets[]={0x500,0x580,0x588};
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};

int a733_dma_platform_scopes(const struct a733_dma_platform_config *c,a733_clock_mask_t *out)
{
    if (!c || !out || !c->owner || !c->handoff || !c->deadline_us || c->cold>1 ||
        ((c->expected_cpu|c->expected_mcu|c->desired_cpu|c->desired_mcu|
          c->route_change_mask)&~UINT32_C(0xffff)) || !c->desired_cpu ||
        (c->desired_cpu&c->desired_mcu) ||
        (((c->expected_cpu^c->desired_cpu)|(c->expected_mcu^c->desired_mcu))&~c->route_change_mask) ||
        (!c->cold && (c->expected_cpu!=c->desired_cpu || c->expected_mcu!=c->desired_mcu))) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_OSC|A733_CLOCK_REF;
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7];
        unsigned divisor=(c->fabric[i]&31)+1;
        if (!mhz || (mhz*1000000u)%divisor || c->rate_hz[i]!=(mhz*1000000u)/divisor) return 0;
        if (mhz!=24) shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
int a733_dma_platform_init(struct a733_dma_platform *p,
    const struct a733_dma_platform_config *c,const struct a733_dma_platform_operations *o,void *cookie)
{
    a733_clock_mask_t shared;
    if (!p || p->initialized || !o || !o->acquire || !o->enter || !o->leave ||
        !o->now_us || !o->read || !o->write || !a733_dma_platform_scopes(c,&shared)) return 0;
    p->config=*c;p->ops=*o;p->cookie=cookie;p->shared=shared;
    for (unsigned i=0;i<3;++i) p->selected_mhz[i]=parent_mhz[i][(c->fabric[i]>>24)&7];
    p->initialized=1;return 1;
}
static int timely(struct a733_dma_platform *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->config.deadline_us) return 0;
    p->last_now=now;return 1;
}
static enum a733_dma_platform_result fault(struct a733_dma_platform *p)
{p->failed=1;return RETAINED;}
static int rd(struct a733_dma_platform *p,uintptr_t address,uint32_t *value)
{
    return timely(p) && p->ops.read(p->cookie,address,value)==1 && timely(p);
}
static int update(struct a733_dma_platform *p,uintptr_t address,uint32_t mask,
    uint32_t value,uint32_t required)
{
    uint32_t old,actual;
    if (!rd(p,address,&old) || (old&required)!=required) return 0;
    value=(old&~mask)|(value&mask);
    if (value==old) return 1;
    if (!timely(p)) return 0;
    p->wrote=1; /* A failed write callback can still have performed the write. */
    return p->ops.write(p->cookie,address,value)==1 && timely(p) &&
        rd(p,address,&actual) && actual==value;
}
static int parents_stable(struct a733_dma_platform *p)
{
    uint32_t osc,ref,peri,a,b,gate_config,gate_status;
    /* Fabric source zero is SYS_CLK24M; PERI0 also uses normalized PLL_REF.
     * Raw DCXO need not itself run at 24 MHz. Retain and check REF for both. */
    if (!rd(p,OSC,&osc) || !rd(p,CCU,&ref) || !a733_clock_ref24_valid(osc,ref)) return 0;
    if (!(p->shared&A733_CLOCK_PERI0)) return 1;
    /* User Manual 4.1.6.6: input divisor M1 is bit 1 plus one. */
    if (!rd(p,CCU+0xa0,&peri) || (peri&UINT32_C(0xfe000000))!=UINT32_C(0xfe000000) ||
        (peri&(BIT(24)|BIT(1))) || !rd(p,CCU+0xa8,&a) || (a&BIT(31)) ||
        !rd(p,CCU+0xac,&b) || (b&BIT(27)) || !rd(p,CCU+0x1908,&gate_config) ||
        !rd(p,CCU+0x1988,&gate_status)) return 0;
    unsigned n=((peri>>8)&255)+1;
    if (n<53 || n>105) return 0;
    for (unsigned i=0;i<3;++i) {
        unsigned mhz=p->selected_mhz[i], divisor, branches;
        switch (mhz) {
        case 24: continue;
        case 600: divisor=(((peri>>20)&7)+1)*2;branches=BIT(11)|BIT(9);break;
        case 400: divisor=(((peri>>20)&7)+1)*3;branches=BIT(11)|BIT(2)|BIT(1);break;
        case 800: divisor=((peri>>16)&7)+1;branches=BIT(10);break;
        case 480: divisor=((peri>>2)&7)+1;branches=BIT(8)|BIT(7);break;
        default: return 0;
        }
        /* Manual 4.1.6.249/258: low bits set mean No-Auto; upper
         * bits enable each selected branch and report its effective gate. */
        uint32_t enabled=branches<<16, required=branches|enabled;
        if ((gate_config&required)!=required || (gate_status&enabled)!=enabled ||
            24u*n!=mhz*divisor) return 0;
    }
    return 1;
}
static int baseline(struct a733_dma_platform *p)
{
    uint32_t v;
    for (unsigned i=0;i<3;++i)
        if (!rd(p,CCU+fabric_offsets[i],&v) || (v&fabric_masks[i])!=p->config.fabric[i]) return 0;
    if (!parents_stable(p) || !rd(p,BUS,&v) ||
        (v&UINT32_C(0x10001))!=(p->config.cold ? 0u : UINT32_C(0x10001))) return 0;
    if (!rd(p,MBUS_GATE,&v) || (v&CRITICAL)!=CRITICAL || (v&1)!=(p->config.cold ? 0u : 1u)) return 0;
    return 1;
}
static int quiet(struct a733_dma_platform *p)
{
    uint32_t v;
    if (!rd(p,DMA+0x30,&v) || v) return 0;
    /* Refuse inherited mode, pending interrupt, paused, enabled or high-address
     * descriptor state. No acknowledgment or channel stop is performed here. */
    static const unsigned offsets[]={0,4,0x28,0x34,0x38,0x3c};
    for (unsigned ch=0;ch<16;++ch)
        for (unsigned j=0;j<sizeof(offsets)/sizeof(*offsets);++j)
            if (!rd(p,DMA+0x100+0x40*ch+offsets[j],&v) || v) return 0;
    return rd(p,DMA+0x30,&v) && !v;
}
static int controller(struct a733_dma_platform *p)
{
    const struct a733_dma_platform_config *c=&p->config;
    uint32_t cpu,mcu,automatic;
    if (!quiet(p) || !rd(p,DMA+0x34,&cpu) || (cpu&0xffff)!=c->expected_cpu ||
        !rd(p,DMA+0x38,&mcu) || (mcu&0xffff)!=c->expected_mcu ||
        !rd(p,DMA+0x28,&automatic)) return 0;
    if (!c->cold) return automatic==4;
    if (automatic && automatic!=4) return 0;
    /* Clear removed routes before adding destinations. Interrupt masks and
     * exclusive remote-MCU handoff remain prerequisites throughout. */
    return update(p,DMA+0x28,UINT32_MAX,4,0) &&
        update(p,DMA+0x34,0xffff,c->expected_cpu&c->desired_cpu,0) &&
        update(p,DMA+0x38,0xffff,c->expected_mcu&c->desired_mcu,0) &&
        update(p,DMA+0x34,0xffff,c->desired_cpu,0) &&
        update(p,DMA+0x38,0xffff,c->desired_mcu,0) && quiet(p);
}
static int same_token(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
enum a733_dma_platform_result a733_dma_platform_poll(struct a733_dma_platform *p)
{
    if (!p || !p->initialized || p->failed) return RETAINED;
    if (p->stage==COMPLETE) return READY;
    if (!timely(p)) return fault(p);
    if (p->stage==ACQUIRE) {
        struct a733_clock_token old=p->token;
        enum a733_dma_platform_result r=p->ops.acquire(p->cookie,&p->config,p->shared,&p->token);
        if ((old.id && !same_token(&old,&p->token)) || (r==BUSY && !same_token(&old,&p->token))) {
            if (old.id) p->token=old;
            return fault(p);
        }
        if (!timely(p) || r==RETAINED) return fault(p);
        if (r==BUSY) return BUSY;
        if ((r!=READY && r!=PENDING) || !p->token.id || p->token.owner!=p->config.owner ||
            p->token.shared!=p->shared || p->token.exclusive!=A733_CLOCK_DMA0) return fault(p);
        if (r==READY) p->stage=BASELINE;
        return PENDING;
    }
    if (p->stage==RESET_WAIT) {
        if (p->last_now-p->reset_since<20) return PENDING;
        p->stage=ENABLE;
    }
    int entered=p->ops.enter(p->cookie,p->config.deadline_us);
    int ontime=timely(p);
    if (entered!=1) {
        if (entered!=0 || !ontime) return fault(p);
        return BUSY;
    }
    if (!ontime) {p->ops.leave(p->cookie);return fault(p);}
    int ok=0;
    if (p->stage==BASELINE) {
        ok=baseline(p);
        if (ok) {p->reset_since=p->last_now;p->stage=p->config.cold ? RESET_WAIT : CONTROLLER;}
    } else if (p->stage==ENABLE) {
        /* Recheck the asserted-reset baseline after waiting, before any write. */
        ok=baseline(p) && update(p,BUS,BIT(16),BIT(16),0) &&
            update(p,BUS,BIT(0),BIT(0),BIT(16)) &&
            update(p,MBUS_GATE,BIT(0),BIT(0),CRITICAL);
        if (ok) p->stage=CONTROLLER;
    } else if (p->stage==CONTROLLER) {
        ok=controller(p);
        if (ok) p->stage=COMPLETE;
    }
    p->ops.leave(p->cookie);
    if (!ok || !timely(p)) return fault(p);
    return p->stage==COMPLETE ? READY : PENDING;
}
int a733_dma_platform_ready(const struct a733_dma_platform *p)
{return p && p->initialized && !p->failed && p->stage==COMPLETE;}
