/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_pmic_consumer.h"
#include "a733_power_domains.h"
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
static int correct(const struct a733_pmic_consumer *c,const struct a733_pmic_snapshot *s)
{
    return s->rail==c->config.rail && !s->page_before && s->chip==3 && !s->revision &&
        !s->page_after && s->enabled==1 && s->decoded==1 && s->programmed_uv==800000 &&
        (s->selector&127)==30 && (s->enable&(c->config.rail==A733_RAIL_DCDC2 ? 2 : 8));
}
static int equal(const struct a733_pmic_snapshot *a,const struct a733_pmic_snapshot *b)
{
    return a->rail==b->rail && a->page_before==b->page_before && a->chip==b->chip &&
        a->revision==b->revision && a->enable==b->enable && a->selector==b->selector &&
        a->page_after==b->page_after && a->enabled==b->enabled && a->decoded==b->decoded &&
        a->programmed_uv==b->programmed_uv;
}
/* DCDC4 completes from the service's boot verification when the provider is
 * bound to a power-domain owner: no bus transaction and no PMIC slot. */
static int recorded(const struct a733_pmic_consumer *c)
{return c->config.rail==A733_RAIL_DCDC4 && c->config.provider->domains;}
/* 1 verified, 0 verification pending, -1 refused: one acquire load. */
static int record(const struct a733_pmic_consumer *c)
{
    const unsigned w=a733_power_authority_load(&c->config.provider->rails->authority);
    if(w&A733_POWER_PMIC_TRANSFER)return -1;
    if(!(w&A733_POWER_SUPPLY_DONE))return 0;
    return w&A733_POWER_DCDC4_VERIFIED ? 1 : -1;
}
int a733_pmic_consumer_storage(struct a733_pmic *p,const void *v,size_t n)
{
    return p && p->self==p && p->rails && p->clocks && p->pins &&
        a733_ranges_disjoint(v,n,p,sizeof(*p)) && a733_ranges_disjoint(v,n,p->rails,sizeof(*p->rails)) &&
        a733_ranges_disjoint(v,n,p->clocks,sizeof(*p->clocks)) && a733_ranges_disjoint(v,n,p->pins,sizeof(*p->pins)) &&
        a733_ranges_disjoint(v,n,p->rails->binding.gpu,sizeof(*p->rails->binding.gpu)) &&
        p->ops.outside(p->cookie,v,n)==1;
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
        status=a733_rail_acquire(r,c->config.owner,A733_RAIL_BIT(c->config.rail),0,deadline,&c->rail);
        if(status==A733_RAIL_BUSY)return 0;
        if(status!=A733_RAIL_PENDING || !c->rail.id)return fault(c);
        c->stage=RESERVED;return timely(c,deadline)<0 ? -1 : 0;
    }
    if(c->rail.owner!=c->config.owner || c->rail.shared!=A733_RAIL_BIT(c->config.rail) || c->rail.exclusive)
        return fault(c);
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
    if(recorded(c)) {
        if(c->stage==SAMPLED)return a733_pmic_consumer_health(c);
        r=record(c);if(r<=0)return r<0 ? fault(c) : 0;
        /* The verified bit, loaded with acquire order above, publishes the record. */
        const struct a733_pmic_snapshot *s=&p->domains->dcdc4;
        if(!correct(c,s) || timely(c,deadline)<0)return fault(c);
        c->snapshot=*s;c->stage=SAMPLED;return 1;
    }
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
    if((!recorded(c) && !request_identity(c)) || c->rail.owner!=c->config.owner ||
        c->rail.shared!=A733_RAIL_BIT(c->config.rail) || c->rail.exclusive)return fault(c);
    enum a733_rail_result rail=a733_rail_status(c->config.provider->rails,&c->rail);
    if(rail==A733_RAIL_BUSY)return 0;
    if(rail!=A733_RAIL_READY)return fault(c);
    if(recorded(c))return record(c)==1 ? 1 : fault(c);
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
    if(recorded(c))return 1; /* No request: the boot record stays valid. */
    enum a733_pmic_status s=a733_pmic_reap(c->config.provider,&c->request);
    if(s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_COMPLETE)return fault(c);
    c->request=(struct a733_pmic_request){0};c->stage=STABLE;
    return timely(c,deadline);
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
        c->stage!=SAMPLED || (!recorded(c) && !request_identity(c)) || !correct(c,&c->snapshot))return 0;
    c->armed=0;const struct a733_pmic_snapshot *s=&c->snapshot;
    *out=(struct a733_gpu_resource_rail){s->page_before,s->chip,s->revision,s->enable,s->selector,s->page_after};
    return 1;
}
