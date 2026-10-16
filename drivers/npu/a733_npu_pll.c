#include "a733_npu_pll.h"
#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
#define CTRL (CCU+0x2a0)
#define LOCK BIT(28)
#define FACTORS UINT32_C(0x0070ff02)
#define PROFILE UINT32_C(0x00305100)
#define READY A733_NPU_PLL_READY
#define BUSY A733_NPU_PLL_BUSY
#define PENDING A733_NPU_PLL_PENDING
#define RETAINED A733_NPU_PLL_RETAINED
enum { ACQUIRE, BASELINE, PROGRAM, LOCK_WAIT, SETTLE, OUTPUT, COMPLETE };
static const uint32_t offsets[A733_NPU_PLL_SNAPSHOTS]={
    0x500,0x580,0x588,0xa80,0xa88,0xa8c,0xb00,0xb04,0xc00,0xc0c,0x1b10,0x1b14,0x1b1c
};
int a733_npu_pll_scopes(const struct a733_npu_pll_config *c,uint32_t *shared,uint32_t *exclusive)
{
    if (!c || !shared || !exclusive || shared==exclusive || !c->owner || !c->handoff || !c->deadline_us ||
        (c->mode!=A733_NPU_PLL_COLD_ENABLE && c->mode!=A733_NPU_PLL_ENABLED_RETUNE)) return 0;
    const uint32_t *s=c->consumers;
    uint32_t parents=A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC;
    /* Supported parent selectors: AHB SYS24/PERI0_600, NSI SYS24/PERI0_800,
     * 600/480, MBUS SYS24/PERI0_600/480/400. Pending DFS updates are refused. */
    static const unsigned allowed[]={0x9,0x1d,0x1b};
    for (unsigned i=0;i<3;++i) {
        unsigned sel=(s[i]>>24)&7;
        if (!(allowed[i]&BIT(sel)) || (i && (s[i]&(BIT(28)|BIT(27))))) return 0;
        if (sel) parents|=A733_CLOCK_PERI0;
        if (c->mode==A733_NPU_PLL_COLD_ENABLE && sel) return 0;
    }
    if ((s[3]&BIT(31)) || ((s[3]>>24)&7)>6 || (s[4]&BIT(31)) || ((s[4]>>24)&7)>6 ||
        (s[5]&0x50005) || (s[6]&BIT(31)) || ((s[6]>>24)&7)>6 ||
        (s[7]&0x70001) || (s[8]&BIT(27)) || ((s[8]>>24)&7)>2 ||
        (s[10]&1) || (s[11]&1) || (s[12]&1)) return 0;
    /* DRAM0 supports DDR/PERI1 only here. The separate PHY route still needs
     * private proof; selector bits alone cannot establish running RAM safety. */
    uint32_t pll=c->initial_control;
    if (c->mode==A733_NPU_PLL_COLD_ENABLE) {
        if ((pll&(BIT(31)|BIT(29)|BIT(27))) || (s[8]&BIT(31)) || (s[9]&BIT(16))) return 0;
    } else if ((pll&(BIT(31)|BIT(30)|BIT(29)|LOCK))!=(BIT(31)|BIT(30)|BIT(29)|LOCK)) return 0;
    *shared=parents;*exclusive=A733_CLOCK_PLL_NPU|A733_CLOCK_NPU;return 1;
}
int a733_npu_pll_init(struct a733_npu_pll *p,const struct a733_npu_pll_config *c,
    const struct a733_npu_pll_operations *o,void *cookie)
{
    uint32_t shared,exclusive;
    if (!p || p->initialized || !o || !o->acquire || !o->enter || !o->leave || !o->retain ||
        !o->now_us || !o->read || !o->write || !a733_npu_pll_scopes(c,&shared,&exclusive)) return 0;
    const struct a733_npu_pll_config config=*c;
    const struct a733_npu_pll_operations ops=*o;
    *p=(struct a733_npu_pll){.config=config,.ops=ops,.cookie=cookie,.shared=shared,
        .self=p,.expected_control=config.initial_control&~LOCK,.initialized=1};
    return 1;
}
int a733_npu_pll_bind_registry(struct a733_npu_pll *p,struct a733_clock_leases *registry)
{
    if (!p || p->self!=p || !p->initialized || p->failed || p->registry || p->successor ||
        p->stage!=ACQUIRE || p->resources.clock.id || p->observed.clock.id ||
        !registry || !registry->initialized) return 0;
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)registry;
    if (sizeof(*p)>UINTPTR_MAX-a || sizeof(*registry)>UINTPTR_MAX-b ||
        !(a+sizeof(*p)<=b || b+sizeof(*registry)<=a)) return 0;
    p->registry=registry;return 1;
}
int a733_npu_pll_ready(const struct a733_npu_pll *p)
{return p && p->self==p && p->initialized && !p->failed && p->stage==COMPLETE;}
static int timely(struct a733_npu_pll *p)
{
    uint64_t t;
    if (p->ops.now_us(p->cookie,&t)!=1 || t<p->last_now || t>=p->config.deadline_us) return 0;
    p->last_now=t;return 1;
}
static enum a733_npu_pll_result fault(struct a733_npu_pll *p)
{
    p->failed=1;
    p->ops.retain(p->cookie,&p->resources,&p->observed,p->lock_state);
    return RETAINED;
}
static int rd(struct a733_npu_pll *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int check(struct a733_npu_pll *p,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(p,a,&v) && (v&mask)==expected;}
static int update(struct a733_npu_pll *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if (!rd(p,a,&old)) return 0;
    if (a==CTRL && (old&~LOCK)!=p->expected_control) return 0;
    uint32_t next=(old&~mask)|(value&mask);
    if (a==CTRL) next&=~LOCK; /* Read-only status is not a write target. */
    uint32_t compare=a==CTRL ? ~LOCK : UINT32_MAX;
    if ((old&compare)==(next&compare)) return 1;
    if (!timely(p)) return 0;
    p->wrote=1;
    if (p->ops.write(p->cookie,a,next)!=1 || !timely(p) || !rd(p,a,&actual) ||
        (actual&compare)!=(next&compare)) return 0;
    if (a==CTRL) p->expected_control=next;
    return 1;
}
static int baseline(struct a733_npu_pll *p)
{
    for (unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)
        if (!check(p,CCU+offsets[i],UINT32_MAX,p->config.consumers[i])) return 0;
    uint32_t osc,ref;
    if (!rd(p,0x07090160,&osc) || (((osc>>14)&3)!=0 && ((osc>>14)&3)!=3) ||
        !rd(p,CCU,&ref) || (ref&0xf8000000)!=0xf8000000) return 0;
    unsigned n=((ref>>8)&255)+1,m=((ref>>16)&127)+1;
    if (n<80 || n>116 || n!=m) return 0;
    /* No modulation or analogue tuning is introduced. The charge-pump bias
     * and phase-interpolator setting must be the documented reset profile. */
    return check(p,CTRL,~LOCK,p->expected_control) &&
        (p->config.mode!=A733_NPU_PLL_ENABLED_RETUNE || check(p,CTRL,LOCK,LOCK)) &&
        check(p,CCU+0x2a4,1,0) && check(p,CCU+0x2a8,BIT(31),0) &&
        check(p,CCU+0x2ac,UINT32_C(0xff100000),UINT32_C(0x30000000)) &&
        check(p,CCU+0x2b0,UINT32_C(0x1f0000),UINT32_C(0xb0000));
}
struct step {uint32_t address,mask,value;};
/* Gate force-on ordering: software enable before No-Auto. Consumers remain
 * excluded through method2 or enabled retune and final output enable. */
static const struct step cold[]={
    {CCU+0x1920,BIT(16),BIT(16)}, {CCU+0x1920,1,1},
    {CTRL,BIT(30),BIT(30)}, {CTRL,FACTORS,PROFILE},
    {CTRL,BIT(31),BIT(31)}, {CTRL,BIT(29),BIT(29)}
};
static const struct step retune[]={
    {CCU+0x1920,BIT(16),BIT(16)}, {CCU+0x1920,1,1},
    {CTRL,BIT(27),0}, {CTRL,FACTORS,PROFILE},
    {CTRL,BIT(29),0}, {CTRL,BIT(29),BIT(29)}
};
static int clock_equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int preserved(const struct a733_npu_pll_resources *a,const struct a733_npu_pll_resources *b)
{return (!a->clock.id || clock_equal(&a->clock,&b->clock)) &&
    (!a->whole_platform || a->whole_platform==b->whole_platform);}
static int equal(const struct a733_npu_pll_resources *a,const struct a733_npu_pll_resources *b)
{return clock_equal(&a->clock,&b->clock) && a->whole_platform==b->whole_platform;}
enum a733_npu_pll_result a733_npu_pll_poll(struct a733_npu_pll *p)
{
    if (!p || p->self!=p || !p->initialized || p->failed) return RETAINED;
    if (p->stage==COMPLETE) return READY;
    if (!timely(p)) return fault(p);
    if (p->stage==ACQUIRE) {
        p->observed=p->resources;
        enum a733_npu_pll_result r=p->ops.acquire(p->cookie,&p->config,p->shared,
            A733_CLOCK_PLL_NPU|A733_CLOCK_NPU,&p->observed);
        if (!preserved(&p->resources,&p->observed) || (r==BUSY && !equal(&p->resources,&p->observed))) return fault(p);
        p->resources=p->observed;
        if ((r!=READY && r!=BUSY && r!=PENDING) || !timely(p)) return fault(p);
        if (r==BUSY || r==PENDING) return r;
        if (!p->resources.clock.id || p->resources.clock.owner!=p->config.owner ||
            p->resources.clock.shared!=p->shared || p->resources.clock.exclusive!=(A733_CLOCK_PLL_NPU|A733_CLOCK_NPU) ||
            !p->resources.whole_platform) return fault(p);
        p->stage=BASELINE;return PENDING;
    }
    if ((p->stage==LOCK_WAIT && p->last_now<p->next_sample) ||
        (p->stage==SETTLE && p->last_now<p->settle_until)) return PENDING;
    int held=p->ops.enter(p->cookie,p->config.deadline_us);
    if (!held) return timely(p) ? BUSY : fault(p);
    p->lock_state=held==1 ? 1 : 2;
    if (held!=1 || !timely(p)) return fault(p);
    int ok=1,complete=0;
    if (p->stage==BASELINE) {ok=baseline(p);if(ok)p->stage=PROGRAM;}
    else if (p->stage==PROGRAM) {
        const struct step *s=p->config.mode==A733_NPU_PLL_COLD_ENABLE ? &cold[p->step] : &retune[p->step];
        ok=update(p,s->address,s->mask,s->value);
        if (ok && ++p->step==6) {
            ok=p->last_now<=UINT64_MAX-3;
            if(ok) {p->next_sample=p->last_now+3;p->stage=LOCK_WAIT;}
        }
    } else if (p->stage==LOCK_WAIT) {
        uint32_t v;
        ok=rd(p,CTRL,&v) && (v&~LOCK)==p->expected_control;
        if(ok) {
            p->consecutive=(v&LOCK) ? p->consecutive+1 : 0;
            ++p->samples;
            if (p->consecutive==3) {
                ok=p->last_now<=UINT64_MAX-20;
                if(ok) {p->settle_until=p->last_now+20;p->stage=SETTLE;}
            } else {
                ok=p->samples<100 && p->last_now<=UINT64_MAX-3;
                if(ok)p->next_sample=p->last_now+3;
            }
        }
    } else if (p->stage==SETTLE) {
        ok=check(p,CTRL,UINT32_MAX,p->expected_control|LOCK) &&
            check(p,CCU+0x1920,0x10001,0x10001) && check(p,CCU+0x19a0,BIT(16),BIT(16));
        if(ok)p->stage=OUTPUT;
    } else if (p->stage==OUTPUT) {
        ok=check(p,CTRL,UINT32_MAX,p->expected_control|LOCK) && update(p,CTRL,BIT(27),BIT(27)) &&
            check(p,CTRL,UINT32_MAX,p->expected_control|LOCK) &&
            check(p,CCU+0x19a0,BIT(16),BIT(16));
        if(ok)complete=1;
    } else ok=0;
    /* No unlock after a failed readback/deadline. A failure first observed
     * after confirmed release retains authority but cannot retain that mutex. */
    if (!ok || !timely(p)) return fault(p);
    int released=p->ops.leave(p->cookie);
    p->lock_state=released==1 ? 0 : 2;
    if (released!=1 || !timely(p)) return fault(p);
    if(complete)p->stage=COMPLETE;
    return p->stage==COMPLETE ? READY : PENDING;
}
