/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_pmic_consumer.h"
#include "a733_gpu_opp.h"
#include "a733_range.h"
enum { NEW, RESERVED, STABLE, REQUESTED, SAMPLED };
static int identity(const struct a733_pmic_consumer *c)
{
    return c && c->self==c && !c->failed && c->config.provider==c->sealed.provider &&
        c->config.owner==c->sealed.owner && c->config.rail==c->sealed.rail &&
        c->config.lifetime.provider==c->sealed.lifetime.provider &&
        c->config.lifetime.owner==c->sealed.lifetime.owner &&
        c->config.lifetime.identity==c->sealed.lifetime.identity;
}
static int fault(struct a733_pmic_consumer *c)
{c->failed=1;c->armed=0;return -1;}
static int timely(struct a733_pmic_consumer *c,uint64_t deadline)
{
    uint64_t now;struct a733_rail_leases *r=c->config.provider->rails;
    if(!deadline || r->binding.now(r->binding.context,&now)!=1 || !a733_deadline_admit(now,&c->last_now,deadline))
        return fault(c);
    return 1;
}
static int request_identity(const struct a733_pmic_consumer *c)
{
    return c->request.id && c->request.owner==c->config.owner &&
        c->request.lifetime.provider==c->config.lifetime.provider &&
        c->request.lifetime.owner==c->config.lifetime.owner &&
        c->request.lifetime.identity==c->config.lifetime.identity;
}
/* DCDC2 at its power-on 800 mV; DCDC4 at the GPU operating point's 960 mV,
 * which the GPU platform sets before it samples (a733_gpu_opp.h). */
static int correct(const struct a733_pmic_consumer *c,const struct a733_pmic_snapshot *s)
{
    const int gpu=c->config.rail==A733_RAIL_DCDC4;
    const uint32_t uv=gpu ? (c->gpu_performance ? c->expected_uv : A733_GPU_SUPPLY_UV) : 800000;
    return s->rail==c->config.rail && !s->page_before && s->chip==3 && !s->revision &&
        !s->page_after && s->enabled==1 && s->decoded==1 &&
        s->programmed_uv==uv && (s->selector&127)==(uv-500000)/10000 && (s->enable&(gpu ? 8 : 2));
}
static int rail_identity(const struct a733_pmic_consumer *c)
{
    const a733_rail_mask_t bit=A733_RAIL_BIT(c->config.rail);
    return c->rail.owner==c->config.owner &&
        c->rail.shared==(c->gpu_performance ? 0 : bit) &&
        c->rail.exclusive==(c->gpu_performance ? bit : 0);
}
static int equal(const struct a733_pmic_snapshot *a,const struct a733_pmic_snapshot *b)
{
    return a->rail==b->rail && a->page_before==b->page_before && a->chip==b->chip &&
        a->revision==b->revision && a->enable==b->enable && a->selector==b->selector &&
        a->page_after==b->page_after && a->enabled==b->enabled && a->decoded==b->decoded &&
        a->programmed_uv==b->programmed_uv;
}
int a733_pmic_consumer_storage(struct a733_pmic *p,const void *v,size_t n)
{
    return p && p->self==p && p->rails && p->clocks && p->pins &&
        a733_ranges_disjoint(v,n,p,sizeof(*p)) && a733_ranges_disjoint(v,n,p->rails,sizeof(*p->rails)) &&
        a733_ranges_disjoint(v,n,p->clocks,sizeof(*p->clocks)) && a733_ranges_disjoint(v,n,p->pins,sizeof(*p->pins)) &&
        a733_ranges_disjoint(v,n,p->rails->binding.gpu,sizeof(*p->rails->binding.gpu)) &&
        a733_pmic_outside(p,v,n)==1;
}
int a733_pmic_consumer_init(struct a733_pmic_consumer *c,const struct a733_pmic_consumer_config *v)
{
    if(!c || c->self || !v || !v->provider || !v->owner ||
        (v->rail!=A733_RAIL_DCDC2 && v->rail!=A733_RAIL_DCDC4) ||
        !a733_ranges_disjoint(c,sizeof(*c),v,sizeof(*v)))return 0;
    struct a733_pmic_consumer_config copy=*v;struct a733_pmic *p=copy.provider;
    if(!a733_pmic_consumer_storage(p,c,sizeof(*c)))return 0;
    struct a733_pmic_lifetime actual;
    if(a733_pmic_identity(p,&actual)!=A733_PMIC_READY || actual.provider!=copy.lifetime.provider ||
        actual.owner!=copy.lifetime.owner || actual.identity!=copy.lifetime.identity)return 0;
    *c=(struct a733_pmic_consumer){.self=c,.config=copy,.sealed=copy};return 1;
}
int a733_pmic_consumer_reserve(struct a733_pmic_consumer *c,uint64_t deadline)
{
    if(!identity(c))return -1;
    if(c->deadline && c->deadline!=deadline)return fault(c);
    c->deadline=deadline;if(timely(c,deadline)<0)return -1;
    struct a733_rail_leases *r=c->config.provider->rails;enum a733_rail_result status;
    if(c->stage==NEW) {
        const a733_rail_mask_t bit=A733_RAIL_BIT(c->config.rail);
        status=a733_rail_acquire(r,c->config.owner,c->gpu_performance ? 0 : bit,
            c->gpu_performance ? bit : 0,deadline,&c->rail);
        if(status==A733_RAIL_BUSY)return 0;
        if(status!=A733_RAIL_PENDING || !c->rail.id)return fault(c);
        c->stage=RESERVED;return timely(c,deadline)<0 ? -1 : 0;
    }
    if(!rail_identity(c))return fault(c);
    status=c->stage==RESERVED ? a733_rail_poll(r,&c->rail) : a733_rail_status(r,&c->rail);
    if(status==A733_RAIL_BUSY || status==A733_RAIL_PENDING)return 0;
    if(status!=A733_RAIL_READY || timely(c,deadline)<0)return fault(c);
    if(c->stage==RESERVED)c->stage=STABLE;
    return 1;
}
int a733_pmic_consumer_sample(struct a733_pmic_consumer *c,uint64_t deadline)
{
    int r=a733_pmic_consumer_reserve(c,deadline);if(r!=1)return r;
    struct a733_pmic *p=c->config.provider;
    enum a733_pmic_status status=a733_pmic_live(p,&c->config.lifetime);
    if(status==A733_PMIC_BUSY || status==A733_PMIC_PENDING)return 0;
    if(status!=A733_PMIC_READY)return fault(c);
    if(c->stage==STABLE) {
        status=a733_pmic_submit(p,c->config.owner,&c->rail,c->config.rail,deadline,&c->request);
        if(status==A733_PMIC_BUSY || (status==A733_PMIC_PENDING && !c->request.id))return 0;
        if(status!=A733_PMIC_PENDING || !request_identity(c))return fault(c);
        c->stage=REQUESTED;return timely(c,deadline)<0 ? -1 : 0;
    }
    if(c->stage==SAMPLED)return a733_pmic_consumer_health(c);
    if(c->stage!=REQUESTED || !request_identity(c))return fault(c);
    status=a733_pmic_result(p,&c->request,&c->snapshot);
    if(status==A733_PMIC_BUSY || status==A733_PMIC_PENDING)return 0;
    if(status!=A733_PMIC_COMPLETE || !correct(c,&c->snapshot) || timely(c,deadline)<0)return fault(c);
    c->stage=SAMPLED;return 1;
}
int a733_pmic_consumer_health(struct a733_pmic_consumer *c)
{
    if(!identity(c))return -1;
    if(c->stage!=SAMPLED)return 0;
    if(!request_identity(c) || !rail_identity(c))return fault(c);
    enum a733_rail_result rail=a733_rail_status(c->config.provider->rails,&c->rail);
    if(rail==A733_RAIL_BUSY)return 0;
    if(rail!=A733_RAIL_READY)return fault(c);
    enum a733_pmic_status s=a733_pmic_live(c->config.provider,&c->config.lifetime);
    if(s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_READY)return fault(c);
    struct a733_pmic_snapshot actual;
    s=a733_pmic_result(c->config.provider,&c->request,&actual);
    if(s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_COMPLETE || !correct(c,&actual) || !equal(&actual,&c->snapshot))return fault(c);
    return 1;
}
int a733_pmic_consumer_refresh(struct a733_pmic_consumer *c,uint64_t deadline)
{
    int r=a733_pmic_consumer_health(c);if(r!=1)return r;
    if(c->armed || deadline!=c->deadline || timely(c,deadline)<0)return fault(c);
    enum a733_pmic_status s=a733_pmic_reap(c->config.provider,&c->request);
    if(s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_COMPLETE)return fault(c);
    c->request=(struct a733_pmic_request){0};c->stage=STABLE;
    return timely(c,deadline);
}
int a733_pmic_consumer_gpu_performance(struct a733_pmic_consumer *c)
{
    if(!identity(c) || c->config.rail!=A733_RAIL_DCDC4 || c->stage!=NEW || c->deadline ||
        c->gpu_performance)return 0;
    c->gpu_performance=1;c->expected_uv=A733_GPU_SUPPLY_UV;return 1;
}
int a733_pmic_consumer_gpu_begin(struct a733_pmic_consumer *c,uint64_t deadline,uint32_t uv)
{
    if(!identity(c) || !c->gpu_performance || c->config.rail!=A733_RAIL_DCDC4 || c->armed ||
        (uv!=A733_GPU_SUPPLY_UV && uv!=A733_GPU_LOW_UV) || timely(c,deadline)<0)return -1;
    if(c->stage==NEW || c->stage==RESERVED) {
        if(uv!=A733_GPU_SUPPLY_UV)return fault(c);
        return a733_pmic_consumer_reserve(c,deadline);
    }
    if(c->stage==SAMPLED) {
        int r=a733_pmic_consumer_health(c);if(r!=1)return r;
        enum a733_pmic_status s=a733_pmic_reap(c->config.provider,&c->request);
        if(s==A733_PMIC_BUSY)return 0;
        if(s!=A733_PMIC_COMPLETE)return fault(c);
        c->request=(struct a733_pmic_request){0};c->stage=STABLE;
    }
    if(c->stage!=STABLE || !rail_identity(c))return fault(c);
    c->deadline=deadline;c->expected_uv=uv;
    return 1;
}
int a733_pmic_consumer_arm(struct a733_pmic_consumer *c,uint64_t deadline)
{
    if(!identity(c))return -1;
    c->armed=0;int r=a733_pmic_consumer_health(c);if(r!=1)return r;
    if(!c->recipient || !c->destination || timely(c,deadline)<0)return fault(c);
    c->armed_deadline=deadline;c->armed=1;return 1;
}
int a733_pmic_consumer_take(struct a733_pmic_consumer *c,const void *recipient,uint64_t deadline,
    struct a733_gpu_resource_rail *out)
{
    if(!identity(c) || !out || out!=c->destination || !c->armed || c->recipient!=recipient ||
        c->armed_deadline!=deadline || c->config.rail!=A733_RAIL_DCDC4 ||
        c->stage!=SAMPLED || !request_identity(c) || !correct(c,&c->snapshot))return 0;
    c->armed=0;const struct a733_pmic_snapshot *s=&c->snapshot;
    *out=(struct a733_gpu_resource_rail){s->page_before,s->chip,s->revision,s->enable,s->selector,s->page_after};
    return 1;
}
