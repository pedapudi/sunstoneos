#include "a733_rail_leases.h"
#include <stddef.h>

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static int external(const struct a733_rail_leases *m,const void *p,size_t n)
{
    return m && separate(m,sizeof(*m),p,n) &&
        separate(m->binding.clocks,sizeof(*m->binding.clocks),p,n) &&
        separate(m->binding.gpu,sizeof(*m->binding.gpu),p,n);
}
static int binding_equal(const struct a733_rail_binding *a,const struct a733_rail_binding *b)
{return a->clocks==b->clocks && a->gpu==b->gpu && a->bridge_owner==b->bridge_owner &&
    a->now==b->now && a->context==b->context;}
static enum a733_rail_result enter(struct a733_rail_leases *m)
{
    if(!m || m->self!=m)return A733_RAIL_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&m->busy,&expected,1,
        memory_order_acquire,memory_order_relaxed))return A733_RAIL_BUSY;
    if(!binding_equal(&m->binding,&m->sealed_binding)) {
        m->failed=1;atomic_store_explicit(&m->busy,0,memory_order_release);return A733_RAIL_RETAINED;
    }
    return A733_RAIL_READY;
}
static enum a733_rail_result leave(struct a733_rail_leases *m,enum a733_rail_result r)
{atomic_store_explicit(&m->busy,0,memory_order_release);return r;}
static int equal(const struct a733_rail_token *a,const struct a733_rail_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static struct a733_rail_slot *find(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!t->id || !t->owner)return NULL;
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i)
        if(m->slots[i].state!=A733_RAIL_FREE && equal(&m->slots[i].token,t))return &m->slots[i];
    return NULL;
}
static enum a733_rail_result fault(struct a733_rail_leases *m,struct a733_rail_slot *s)
{m->failed=1;if(s)s->state=A733_RAIL_FAULTED;return A733_RAIL_RETAINED;}
static enum a733_rail_result time_check(struct a733_rail_leases *m,uint64_t deadline)
{
    uint64_t now;
    if(m->binding.now(m->binding.context,&now)!=1 || now<m->last_now)return fault(m,NULL);
    m->last_now=now;
    return now<deadline ? A733_RAIL_READY : A733_RAIL_EXPIRED;
}
static a733_rail_mask_t expand(a733_rail_mask_t mask)
{
    const a733_rail_mask_t aliases=A733_RAIL_BIT(A733_RAIL_DCDC1)|
        A733_RAIL_BIT(A733_RAIL_DC1SW1)|A733_RAIL_BIT(A733_RAIL_DC1SW2);
    return mask&aliases ? mask|aliases : mask;
}
static enum a733_rail_result bridge_health(struct a733_rail_leases *m,struct a733_rail_slot *s)
{
    if(m->failed || s->state==A733_RAIL_FAULTED)return A733_RAIL_RETAINED;
    const a733_rail_mask_t mask=s->token.shared|s->token.exclusive;
    if(s->state==A733_RAIL_ACTIVE &&
        (((mask&A733_RAIL_BIT(A733_RAIL_DCDC2)) && !s->dcdc2.id) ||
         ((mask&A733_RAIL_BIT(A733_RAIL_DCDC4)) && !s->dcdc4.lease.token)))return fault(m,s);
    if(s->dcdc2.id) {
        enum a733_clock_result r=a733_clock_token_status(m->binding.clocks,&s->dcdc2);
        if(r==A733_CLOCK_BUSY)return A733_RAIL_BUSY;
        if(r!=A733_CLOCK_OK)return fault(m,s);
    }
    if(s->dcdc4.lease.token) {
        enum a733_gpu_resource_result r=a733_gpu_rail_guard_status(m->binding.gpu,&s->dcdc4);
        if(r==A733_GPU_RESOURCE_BUSY)return A733_RAIL_BUSY;
        if(r!=A733_GPU_RESOURCE_OK)return fault(m,s);
    }
    return A733_RAIL_READY;
}
static enum a733_rail_result health(struct a733_rail_leases *m,struct a733_rail_slot *s)
{
    enum a733_rail_result r=bridge_health(m,s);
    if(r!=A733_RAIL_READY)return r;
    return s->state==A733_RAIL_ACTIVE ? A733_RAIL_READY : A733_RAIL_PENDING;
}
int a733_rail_leases_init(struct a733_rail_leases *m,const struct a733_rail_binding *binding)
{
    if(!m || m->self || !separate(m,sizeof(*m),binding,sizeof(*binding)))return 0;
    const struct a733_rail_binding b=*binding;
    if(!b.clocks || !b.gpu || !b.bridge_owner || !b.now || !b.clocks->initialized ||
        !b.gpu->initialized || b.clocks->failed || b.gpu->failed ||
        b.now!=b.clocks->now || b.context!=b.clocks->context ||
        !separate(m,sizeof(*m),b.clocks,sizeof(*b.clocks)) ||
        !separate(m,sizeof(*m),b.gpu,sizeof(*b.gpu)) ||
        !separate(b.clocks,sizeof(*b.clocks),b.gpu,sizeof(*b.gpu)))return 0;
    *m=(struct a733_rail_leases){.self=m,.binding=b,.sealed_binding=b,.next_id=1,.next_borrow=1};
    atomic_init(&m->busy,0);return 1;
}
enum a733_rail_result a733_rail_acquire(struct a733_rail_leases *m,uint64_t owner,
    a733_rail_mask_t shared,a733_rail_mask_t exclusive,uint64_t deadline,struct a733_rail_token *out)
{
    if(!owner || !(shared|exclusive) || ((shared|exclusive)&~A733_RAIL_ALL) ||
        (shared&exclusive) || !external(m,out,sizeof(*out)))return A733_RAIL_INVALID;
    shared=expand(shared);exclusive=expand(exclusive);shared&=~exclusive;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    if(m->failed)return leave(m,A733_RAIL_RETAINED);
    struct a733_rail_slot *free=NULL;
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i) {
        struct a733_rail_slot *s=&m->slots[i];
        if(s->state==A733_RAIL_FREE) {if(!free)free=s;continue;}
        if((exclusive&(s->token.shared|s->token.exclusive)) || (shared&s->token.exclusive))
            return leave(m,A733_RAIL_BUSY);
    }
    if(!free)return leave(m,A733_RAIL_BUSY);
    if(!m->next_id)return leave(m,A733_RAIL_EXHAUSTED);
    r=time_check(m,deadline);if(r!=A733_RAIL_READY)return leave(m,r);
    struct a733_rail_token t={m->next_id++,owner,shared,exclusive};
    *free=(struct a733_rail_slot){.token=t,.deadline=deadline,.state=A733_RAIL_OPENING};
    *out=t;return leave(m,A733_RAIL_PENDING);
}
enum a733_rail_result a733_rail_poll(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!external(m,t,sizeof(*t)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);if(!s)return leave(m,A733_RAIL_INVALID);
    r=bridge_health(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    if(s->state!=A733_RAIL_OPENING)return leave(m,s->state==A733_RAIL_ACTIVE ? A733_RAIL_READY : A733_RAIL_PENDING);
    if(time_check(m,s->deadline)!=A733_RAIL_READY)return leave(m,fault(m,s));
    const a733_rail_mask_t mask=s->token.shared|s->token.exclusive;
    if((mask&A733_RAIL_BIT(A733_RAIL_DCDC2)) && !s->dcdc2.id) {
        int change=(s->token.exclusive&A733_RAIL_BIT(A733_RAIL_DCDC2))!=0;
        enum a733_clock_result c=a733_clock_acquire(m->binding.clocks,m->binding.bridge_owner,
            change ? 0 : A733_POWER_DCDC2,change ? A733_POWER_DCDC2 : 0,s->deadline,&s->dcdc2);
        if(c==A733_CLOCK_BUSY)return leave(m,A733_RAIL_PENDING);
        if(c!=A733_CLOCK_OK)return leave(m,fault(m,s));
    }
    if((mask&A733_RAIL_BIT(A733_RAIL_DCDC4)) && !s->dcdc4.lease.token) {
        enum a733_gpu_resource_result g=a733_gpu_rail_guard_acquire(m->binding.gpu,
            (s->token.exclusive&A733_RAIL_BIT(A733_RAIL_DCDC4))!=0,&s->dcdc4);
        if(g==A733_GPU_RESOURCE_BUSY)return leave(m,A733_RAIL_PENDING);
        if(g!=A733_GPU_RESOURCE_OK)return leave(m,fault(m,s));
    }
    if(((mask&A733_RAIL_BIT(A733_RAIL_DCDC2)) && !s->dcdc2.id) ||
        ((mask&A733_RAIL_BIT(A733_RAIL_DCDC4)) && !s->dcdc4.lease.token))return leave(m,fault(m,s));
    r=bridge_health(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    if(time_check(m,s->deadline)!=A733_RAIL_READY)return leave(m,fault(m,s));
    s->state=A733_RAIL_ACTIVE;return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_status(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!external(m,t,sizeof(*t)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    return leave(m,s ? health(m,s) : A733_RAIL_INVALID);
}
enum a733_rail_result a733_rail_borrow(struct a733_rail_leases *m,const struct a733_rail_token *t,
    a733_rail_mask_t rails,struct a733_rail_borrow *out)
{
    if(!rails || (rails&~A733_RAIL_ALL) || !external(m,t,sizeof(*t)) ||
        !external(m,out,sizeof(*out)) || !separate(t,sizeof(*t),out,sizeof(*out)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);if(!s || (rails&~(t->shared|t->exclusive)))return leave(m,A733_RAIL_INVALID);
    r=health(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    struct a733_rail_borrow_slot *free=NULL;
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i) {
        struct a733_rail_borrow_slot *b=&m->borrows[i];
        if(!b->ticket.id) {if(!free)free=b;}
        else if(!separate(out,sizeof(*out),b->address,sizeof(*out)))return leave(m,A733_RAIL_INVALID);
    }
    if(!free)return leave(m,A733_RAIL_BUSY);
    if(!m->next_borrow)return leave(m,A733_RAIL_EXHAUSTED);
    struct a733_rail_borrow b={m->next_borrow++,*t,rails};
    *free=(struct a733_rail_borrow_slot){b,out};++s->borrows;*out=b;
    return leave(m,A733_RAIL_READY);
}
static struct a733_rail_borrow_slot *find_borrow(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i) {
        struct a733_rail_borrow_slot *s=&m->borrows[i];
        if(s->address==b && b->id && s->ticket.id==b->id && s->ticket.rails==b->rails &&
            equal(&s->ticket.parent,&b->parent))return s;
    }
    return NULL;
}
static enum a733_rail_result borrowed(struct a733_rail_leases *m,const struct a733_rail_borrow *b,int end)
{
    if(!external(m,b,sizeof(*b)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_borrow_slot *bs=find_borrow(m,b);
    if(!bs)return leave(m,A733_RAIL_INVALID);
    struct a733_rail_slot *s=find(m,&bs->ticket.parent);
    if(!s || !s->borrows)return leave(m,fault(m,s));
    r=health(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    if(end) {--s->borrows;*bs=(struct a733_rail_borrow_slot){0};}
    return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_borrow_status(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{return borrowed(m,b,0);}
enum a733_rail_result a733_rail_end_borrow(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{return borrowed(m,b,1);}
enum a733_rail_result a733_rail_release(struct a733_rail_leases *m,const struct a733_rail_token *t,uint64_t deadline)
{
    if(!external(m,t,sizeof(*t)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);if(!s)return leave(m,A733_RAIL_INVALID);
    r=bridge_health(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    if(s->borrows)return leave(m,A733_RAIL_BUSY);
    r=time_check(m,deadline);
    if(r!=A733_RAIL_READY)return leave(m,s->state==A733_RAIL_CLOSING ? fault(m,s) : r);
    s->state=A733_RAIL_CLOSING;
    if(s->dcdc2.id) {
        enum a733_clock_result c=a733_clock_release(m->binding.clocks,&s->dcdc2,deadline);
        if(c==A733_CLOCK_BUSY)return leave(m,A733_RAIL_PENDING);
        if(c!=A733_CLOCK_OK)return leave(m,fault(m,s));
        s->dcdc2=(struct a733_clock_token){0};
    }
    if(s->dcdc4.lease.token) {
        if(time_check(m,deadline)!=A733_RAIL_READY)return leave(m,fault(m,s));
        enum a733_gpu_resource_result g=a733_gpu_rail_guard_end(m->binding.gpu,&s->dcdc4);
        if(g==A733_GPU_RESOURCE_BUSY)return leave(m,A733_RAIL_PENDING);
        if(g!=A733_GPU_RESOURCE_OK)return leave(m,fault(m,s));
        s->dcdc4=(struct a733_gpu_rail_guard){0};
    }
    /* All fallible operations precede the final slot clear. */
    *s=(struct a733_rail_slot){0};return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_retain(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!external(m,t,sizeof(*t)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    return leave(m,s ? fault(m,s) : A733_RAIL_INVALID);
}
