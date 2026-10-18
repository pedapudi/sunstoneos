#include "a733_npu_platform.h"
#include "a733_clock_reference.h"

#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
#define R_CCU UINT32_C(0x07010000)
#define BIT(n) (UINT32_C(1)<<(n))
#define READY A733_NPU_PLATFORM_READY
#define BUSY A733_NPU_PLATFORM_BUSY
#define PENDING A733_NPU_PLATFORM_PENDING
#define RETAINED A733_NPU_PLATFORM_RETAINED
enum { ACQUIRE, BASELINE, PROGRAM, POWER_WAIT, SETTLE, COMPLETE };
static const uint32_t fabric_offsets[]={0x500,0x580,0x588};
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};
int a733_npu_platform_scopes(const struct a733_npu_platform_config *c,a733_clock_mask_t *out)
{
    if (!c || !out || !c->owner || !c->handoff || !c->deadline_us) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_PLL_NPU|A733_CLOCK_REF|A733_CLOCK_OSC;
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7], divisor=(c->fabric[i]&31)+1;
        if (!mhz || (mhz*1000000u)%divisor || c->rate_hz[i]!=(mhz*1000000u)/divisor) return 0;
        if (mhz!=24) shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
int a733_npu_platform_init(struct a733_npu_platform *p,
    const struct a733_npu_platform_config *c,const struct a733_npu_platform_operations *o,void *cookie)
{
    a733_clock_mask_t shared;
    if (!p || p->initialized || !o || !o->acquire || !o->enter || !o->leave ||
        !o->now_us || !o->read || !o->write || !a733_npu_platform_scopes(c,&shared)) return 0;
    const struct a733_npu_platform_config config=*c;
    const struct a733_npu_platform_operations ops=*o;
    *p=(struct a733_npu_platform){.config=config,.ops=ops,.cookie=cookie,.shared=shared,.initialized=1};
    for (unsigned i=0;i<3;++i) p->selected_mhz[i]=parent_mhz[i][(config.fabric[i]>>24)&7];
    return 1;
}
int a733_npu_platform_adopt_on(struct a733_npu_platform *p)
{
    if(!p || !p->initialized || p->failed || p->stage!=ACQUIRE || p->adopt_on ||
       p->resources.clock.id || p->resources.rail || p->resources.pck ||
       p->observed.clock.id || p->observed.rail || p->observed.pck || p->wrote)return 0;
    p->adopt_on=1;return 1;
}
static int timely(struct a733_npu_platform *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->config.deadline_us) return 0;
    p->last_now=now;return 1;
}
static enum a733_npu_platform_result fault(struct a733_npu_platform *p)
{p->failed=1;return RETAINED;}
static int rd(struct a733_npu_platform *p,uintptr_t address,uint32_t *value)
{return timely(p) && p->ops.read(p->cookie,address,value)==1 && timely(p);}
static int update(struct a733_npu_platform *p,uintptr_t address,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if (!rd(p,address,&old)) return 0;
    value=(old&~mask)|(value&mask);
    if (value==old) return 1;
    if (!timely(p)) return 0;
    p->wrote=1;
    return p->ops.write(p->cookie,address,value)==1 && timely(p) && rd(p,address,&actual) && actual==value;
}
static int parents_stable(struct a733_npu_platform *p)
{
    uint32_t osc,ref,pll,a,b,automatic,status;
    if (!rd(p,0x07090160,&osc) || !rd(p,CCU,&ref) ||
        !a733_clock_ref24_valid(osc,ref)) return 0;
    if (!rd(p,CCU+0x2a0,&pll) || (pll&UINT32_C(0xf8000000))!=UINT32_C(0xf8000000) || (pll&BIT(1)) ||
        !rd(p,CCU+0x2a8,&a) || (a&BIT(31)) || !rd(p,CCU+0x2ac,&b) || (b&BIT(27)) ||
        !rd(p,CCU+0x1920,&automatic) || (automatic&0x10001)!=0x10001 ||
        !rd(p,CCU+0x19a0,&status) || !(status&BIT(16))) return 0;
    unsigned n=((pll>>8)&255)+1,m=((pll>>20)&7)+1;
    if (n<53 || n>105 || (24000000u*n)%m) return 0;
    unsigned rate=(24000000u*n)/m;
    if (rate<A733_NPU_INITIAL_HZ || rate%A733_NPU_INITIAL_HZ || rate/A733_NPU_INITIAL_HZ>32) return 0;
    p->parent_hz=rate;p->divisor=rate/A733_NPU_INITIAL_HZ;
    if (!(p->shared&A733_CLOCK_PERI0)) return 1;
    if (!rd(p,CCU+0xa0,&pll) || (pll&UINT32_C(0xfe000000))!=UINT32_C(0xfe000000) ||
        (pll&BIT(1)) || !rd(p,CCU+0xa8,&a) || (a&BIT(31)) ||
        !rd(p,CCU+0xac,&b) || (b&BIT(27)) || !rd(p,CCU+0x1908,&automatic) ||
        !rd(p,CCU+0x1988,&status)) return 0;
    n=((pll>>8)&255)+1;
    if (n<53 || n>105) return 0;
    for (unsigned i=0;i<3;++i) {
        unsigned mhz=p->selected_mhz[i],divisor,mask;
        switch (mhz) {
        case 24:continue;
        case 600:divisor=(((pll>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
        case 400:divisor=(((pll>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
        case 800:divisor=((pll>>16)&7)+1;mask=BIT(10);break;
        case 480:divisor=((pll>>2)&7)+1;mask=BIT(8)|BIT(7);break;
        default:return 0;
        }
        /* Manual: low bits select No-Auto, corresponding high bits force
         * each parent/leaf gate on; status separately confirms the route. */
        unsigned gates=mask<<16;
        if ((automatic&(mask|gates))!=(mask|gates) || (status&gates)!=gates ||
            24u*n!=mhz*divisor) return 0;
    }
    return 1;
}
static int check(struct a733_npu_platform *p,uintptr_t address,uint32_t mask,uint32_t expected)
{uint32_t value;return rd(p,address,&value) && (value&mask)==expected;}
static int baseline(struct a733_npu_platform *p)
{
    for (unsigned i=0;i<3;++i)
        if (!check(p,CCU+fabric_offsets[i],fabric_masks[i],p->config.fabric[i])) return 0;
    return parents_stable(p) && check(p,CCU+0x5c0,BIT(6),BIT(6)) &&
        check(p,CCU+0x5e0,BIT(18),BIT(18)) && check(p,R_CCU+0x1ac,1,1) &&
        check(p,CCU+0xb00,BIT(31),0) && check(p,CCU+0xb04,0xf0001,BIT(19)) &&
        check(p,CCU+0x1b1c,1,0) &&
        check(p,PCK,0xf,p->adopt_on ? 8 : 0) && check(p,PCK+8,0xf,p->adopt_on ? 8 : 0);
}
struct step {uint32_t address,mask,value;};
/* Reset AHB, AXI, core; establish module rate; bus then module clocks; domain ON.
 * Critical AHB/MBUS gates remain enabled throughout. */
static const struct step steps[]={
    {CCU+0xb04,BIT(18),BIT(18)}, {CCU+0xb04,BIT(17),BIT(17)},
    {CCU+0xb04,BIT(16),BIT(16)}, {CCU+0xb00,0x0700001f,0},
    {CCU+0xb04,1,1}, {CCU+0xb00,BIT(31),BIT(31)},
    {PCK+0x170,UINT32_MAX,0x1f1f1f}, {PCK+0x174,UINT32_MAX,0x1f1f},
    {PCK+0xc00,UINT32_MAX,0x08080808}, {PCK+0xc04,UINT32_MAX,0x0808},
    {PCK+0xc10,UINT32_MAX,8}, {PCK,0xf,8}
};
static int clock_equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int preserved(const struct a733_npu_platform_resources *a,const struct a733_npu_platform_resources *b)
{
    return (!a->clock.id || clock_equal(&a->clock,&b->clock)) &&
        (!a->rail || a->rail==b->rail) && (!a->pck || a->pck==b->pck) &&
        (!a->supply_uv || a->supply_uv==b->supply_uv);
}
static int equal(const struct a733_npu_platform_resources *a,const struct a733_npu_platform_resources *b)
{return clock_equal(&a->clock,&b->clock) && a->rail==b->rail && a->pck==b->pck && a->supply_uv==b->supply_uv;}
enum a733_npu_platform_result a733_npu_platform_poll(struct a733_npu_platform *p)
{
    if (!p || !p->initialized) return RETAINED;
    if (p->failed) return RETAINED;
    if (p->stage==COMPLETE) return READY;
    if (!timely(p)) return fault(p);
    if (p->stage==ACQUIRE) {
        p->observed=p->resources;
        enum a733_npu_platform_result r=p->ops.acquire(p->cookie,&p->config,p->shared,&p->observed);
        if (!preserved(&p->resources,&p->observed) || (r==BUSY && !equal(&p->resources,&p->observed))) return fault(p);
        p->resources=p->observed;
        if (r!=READY && r!=BUSY && r!=PENDING) return fault(p);
        if (!timely(p)) return fault(p);
        if (r==BUSY || r==PENDING) return r;
        if (r!=READY || !p->resources.clock.id || p->resources.clock.owner!=p->config.owner ||
            p->resources.clock.shared!=p->shared || p->resources.clock.exclusive!=A733_CLOCK_NPU ||
            !p->resources.rail || !p->resources.pck || p->resources.supply_uv!=A733_NPU_INITIAL_UV) return fault(p);
        p->stage=BASELINE;return PENDING;
    }
    if (p->stage==SETTLE && p->last_now<p->settle_until) return PENDING;
    int locked=p->ops.enter(p->cookie,p->config.deadline_us);
    if (!locked) return timely(p) ? BUSY : fault(p);
    if (locked!=1) return fault(p);
    int ok=timely(p);
    if (ok && p->stage==BASELINE) {ok=baseline(p);if(ok)p->stage=PROGRAM;}
    else if (ok && p->stage==PROGRAM) {
        const struct step *s=&steps[p->step];
        uint32_t value=p->step==3 ? p->divisor-1 : s->value;
        ok=update(p,s->address,s->mask,value);
        if (ok && ++p->step==(p->adopt_on ? 6 : sizeof(steps)/sizeof(*steps))) p->stage=POWER_WAIT;
    } else if (ok && p->stage==POWER_WAIT) {
        uint32_t value;
        ok=rd(p,PCK+8,&value);
        if (ok) {
            value&=0xf;
            if (value==8) {
                ok=p->last_now<=UINT64_MAX-2000;
                if(ok) {p->settle_until=p->last_now+2000;p->stage=SETTLE;}
            } else if (value!=0 || p->adopt_on) ok=0;
        }
    } else if (ok && p->stage==SETTLE) {
        ok=check(p,PCK+8,0xf,8) && (!p->adopt_on || check(p,PCK,0xf,8)) && check(p,CCU+0xb04,0xf0001,0xf0001) &&
            check(p,CCU+0xb00,0x8700001f,BIT(31)|(p->divisor-1));
        if(ok)p->stage=COMPLETE;
    }
    /* Publish uncertainty while the adapter still owns its register mutex.
     * A successful bus callback can still have failed readback or timing. */
    if (!ok || !timely(p)) (void)fault(p);
    int released=p->ops.leave(p->cookie);
    if (p->failed || released!=1 || !timely(p)) return fault(p);
    return p->stage==COMPLETE ? READY : PENDING;
}
int a733_npu_platform_ready(const struct a733_npu_platform *p)
{return p && p->initialized && !p->failed && p->stage==COMPLETE;}
