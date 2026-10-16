#include "a733_ce_entropy.h"
#include <stddef.h>
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static void zero(void *p,size_t n)
{volatile uint8_t *b=p;while (n--) *b++=0;}
static int outside(const struct device_memory_pool *p,const void *a,size_t n)
{return separate(a,n,(const void *)p->backing.cpu,p->backing.bytes) && separate(a,n,p,sizeof(*p));}
static int token_equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int bound(struct a733_ce_entropy *s)
{
    struct device_memory_extent e;enum device_memory_state state;
    if (s->self!=(uintptr_t)s || !device_memory_describe(s->binding.pool,s->binding.lease,&e,&state) ||
        state!=DEVICE_MEMORY_DEVICE || e.cpu!=s->binding.extent.cpu || e.physical!=s->binding.extent.physical ||
        e.bytes!=s->binding.extent.bytes || s->controller.self!=(uintptr_t)&s->controller ||
        s->controller.memory!=(struct a733_ce_memory *)e.cpu || s->controller.registry!=s->binding.registry ||
        s->controller.cookie!=s->binding.cookie || !token_equal(&s->controller.token,&s->binding.token)) return 0;
    const struct a733_ce_ops *a=&s->controller.ops,*b=&s->binding.ops;
    if (a->now_us!=b->now_us || a->authorize!=b->authorize || a->enter!=b->enter ||
        a->leave!=b->leave || a->retain!=b->retain || a->read!=b->read || a->write!=b->write || a->sync!=b->sync) return 0;
    const struct a733_ce_config *c=&s->controller.config,*d=&s->binding.config;
    if (c->owner!=d->owner || c->handoff!=d->handoff || c->health_profile!=d->health_profile ||
        c->configure_health!=d->configure_health || c->descriptor_physical!=d->descriptor_physical ||
        c->output_physical!=d->output_physical) return 0;
    for (unsigned i=0;i<4;++i) if (c->health[i]!=d->health[i]) return 0;
    for (unsigned i=0;i<3;++i) if (c->fabric[i]!=d->fabric[i] || c->fabric_hz[i]!=d->fabric_hz[i]) return 0;
    return 1;
}
static enum a733_ce_entropy_result finish(struct a733_ce_entropy *s,enum a733_ce_entropy_result r)
{atomic_store_explicit(&s->busy,0,memory_order_release);return r;}
static enum a733_ce_entropy_result retain(struct a733_ce_entropy *s)
{
    s->fault=1;zero(s->staging,sizeof(s->staging));
    (void)device_memory_retain(s->binding.pool,s->binding.lease);
    if (a733_clock_retain(s->binding.registry,&s->binding.token)==A733_CLOCK_BUSY)
        return finish(s,A733_CE_ENTROPY_BUSY);
    s->terminal=1;return finish(s,A733_CE_ENTROPY_RETAINED);
}
static int time_ok(struct a733_ce_entropy *s,uint64_t deadline)
{
    uint64_t now;
    if (s->binding.ops.now_us(s->binding.cookie,&now)!=1 || now<s->last_now || now>=deadline) return 0;
    s->last_now=now;return 1;
}
static int output_ok(const struct a733_ce_entropy *s,const void *out,size_t n)
{
    return separate(s,sizeof(*s),out,n) && outside(s->binding.pool,out,n) &&
        separate(s->binding.registry,sizeof(*s->binding.registry),out,n);
}
int a733_ce_entropy_bind(struct a733_ce_entropy *s,struct device_memory_pool *pool,
    struct device_memory_lease lease,const struct a733_ce_config *c,const struct a733_ce_ops *o,
    void *cookie,struct a733_clock_leases *registry,const struct a733_clock_token *token)
{
    if (!s || s->initialized || !pool || !pool->initialized || !c || !o || !registry || !token ||
        lease.owner!=c->owner) return 0;
    const void *objects[]={s,pool,c,o,registry,token};
    const size_t sizes[]={sizeof(*s),sizeof(*pool),sizeof(*c),sizeof(*o),sizeof(*registry),sizeof(*token)};
    for (unsigned i=0;i<6;++i) {
        if (i!=1 && !outside(pool,objects[i],sizes[i])) return 0;
        for (unsigned j=i+1;j<6;++j) if (!separate(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    }
    struct device_memory_extent e;enum device_memory_state state;
    if (!device_memory_describe(pool,lease,&e,&state) || state!=DEVICE_MEMORY_CPU || e.bytes!=DEVICE_MEMORY_PAGE) return 0;
    struct a733_ce_config config=*c;
    config.descriptor_physical=e.physical;
    config.output_physical=e.physical+offsetof(struct a733_ce_memory,output);
    if (!a733_ce_init(&s->controller,&config,o,cookie,registry,token,(struct a733_ce_memory *)e.cpu)) return 0;
    s->self=(uintptr_t)s;s->binding.pool=pool;s->binding.lease=lease;s->binding.extent=e;
    s->binding.registry=registry;s->binding.token=*token;s->binding.config=config;s->binding.ops=*o;s->binding.cookie=cookie;
    s->next_generation=1;s->initialized=1;atomic_init(&s->busy,0);
    device_memory_lookup(pool,lease)->state=DEVICE_MEMORY_DEVICE;
    return 1;
}
enum a733_ce_entropy_result a733_ce_entropy_request(struct a733_ce_entropy *s,size_t bytes,
    uint64_t deadline,uint64_t *generation)
{
    if (!s || !s->initialized) return A733_CE_ENTROPY_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_ENTROPY_BUSY;
    if (s->terminal) return finish(s,A733_CE_ENTROPY_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!bytes || bytes>A733_CE_ENTROPY_MAX || !deadline || !generation ||
        !output_ok(s,generation,sizeof(*generation)) || !s->next_generation) return finish(s,A733_CE_ENTROPY_INVALID);
    if (s->active) return finish(s,A733_CE_ENTROPY_BUSY);
    uint64_t now;
    if (s->binding.ops.now_us(s->binding.cookie,&now)!=1 || now<s->last_now) return retain(s);
    s->last_now=now;
    if (now>=deadline) return finish(s,A733_CE_ENTROPY_INVALID);
    zero(s->staging,sizeof(s->staging));s->deadline=deadline;s->wanted=bytes;s->filled=0;
    s->active=1;s->consumed=0;s->need_request=1;s->generation=s->next_generation++;
    *generation=s->generation;return finish(s,A733_CE_ENTROPY_PENDING);
}
enum a733_ce_entropy_result a733_ce_entropy_poll(struct a733_ce_entropy *s,uint64_t generation,uint8_t *out,size_t bytes)
{
    if (!s || !s->initialized) return A733_CE_ENTROPY_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_ENTROPY_BUSY;
    if (s->terminal) return finish(s,A733_CE_ENTROPY_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!generation || generation!=s->generation || bytes!=s->wanted || !output_ok(s,out,bytes))
        return finish(s,A733_CE_ENTROPY_INVALID);
    if (s->consumed) return finish(s,A733_CE_ENTROPY_CONSUMED);
    if (!s->active) return finish(s,A733_CE_ENTROPY_INVALID);
    if (!time_ok(s,s->deadline)) return retain(s);
    if (s->need_request) {
        enum a733_ce_result r=a733_ce_request(&s->controller,s->deadline);
        if (s->controller.fault || r==A733_CE_RETAINED || r==A733_CE_INVALID || !time_ok(s,s->deadline)) return retain(s);
        if (r==A733_CE_BUSY) return finish(s,A733_CE_ENTROPY_BUSY);
        if (r!=A733_CE_PENDING) return retain(s);
        s->need_request=0;return finish(s,A733_CE_ENTROPY_PENDING);
    }
    uint8_t block[32]={0};
    enum a733_ce_result r=a733_ce_poll(&s->controller,block);
    if (s->controller.fault || r==A733_CE_RETAINED || r==A733_CE_INVALID ||
        r==A733_CE_CONSUMED || !time_ok(s,s->deadline)) {zero(block,sizeof(block));return retain(s);}
    if (r==A733_CE_RANDOM) {
        size_t take=s->wanted-s->filled;if (take>32) take=32;
        for (size_t i=0;i<take;++i) s->staging[s->filled+i]=block[i];
        s->filled+=take;
    }
    zero(block,sizeof(block));
    if (s->filled==s->wanted) {
        if (!time_ok(s,s->deadline)) return retain(s);
        for (size_t i=0;i<s->wanted;++i) out[i]=s->staging[i];
        zero(s->staging,sizeof(s->staging));s->active=0;s->consumed=1;
        return finish(s,A733_CE_ENTROPY_BYTES);
    }
    if (r==A733_CE_RANDOM) s->need_request=1;
    return finish(s,r==A733_CE_BUSY ? A733_CE_ENTROPY_BUSY:A733_CE_ENTROPY_PENDING);
}
