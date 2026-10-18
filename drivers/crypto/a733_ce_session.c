#include "a733_ce_session.h"
#include <stddef.h>
struct previous_session_layout {
    uintptr_t self;
    __typeof__(((struct a733_ce_session *)0)->binding) binding;
    struct a733_ce controller;
    const void *client;
    size_t client_bytes;
    uint64_t generation,next_generation;
    unsigned initialized,kind,fault,terminal;
};
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"session fault observation must be lock-free");
_Static_assert(sizeof(atomic_uint)==sizeof(unsigned),"session fault width changed");
_Static_assert(_Alignof(atomic_uint)==_Alignof(unsigned),"session fault alignment changed");
_Static_assert(sizeof(struct a733_ce_session)==sizeof(struct previous_session_layout),"session size changed");
_Static_assert(_Alignof(struct a733_ce_session)==_Alignof(struct previous_session_layout),"session alignment changed");
#define SAME_OFFSET(field) _Static_assert(offsetof(struct a733_ce_session,field)==offsetof(struct previous_session_layout,field),"session offset changed: " #field)
SAME_OFFSET(self);SAME_OFFSET(binding);SAME_OFFSET(controller);SAME_OFFSET(client);
SAME_OFFSET(client_bytes);SAME_OFFSET(generation);SAME_OFFSET(next_generation);
SAME_OFFSET(initialized);SAME_OFFSET(kind);SAME_OFFSET(fault);SAME_OFFSET(terminal);
#undef SAME_OFFSET
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static int outside(const struct device_memory_pool *p,const void *a,size_t n)
{return separate(a,n,(const void *)p->backing.cpu,p->backing.bytes) && separate(a,n,p,sizeof(*p));}
static int token_equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
int a733_ce_session_bound(struct a733_ce_session *s)
{
    struct device_memory_extent e;enum device_memory_state state;
    if (!s || !s->initialized || s->fault || s->controller.fault || s->self!=(uintptr_t)s || !device_memory_describe(s->binding.pool,s->binding.lease,&e,&state) ||
        state!=DEVICE_MEMORY_DEVICE || e.cpu!=s->binding.extent.cpu || e.physical!=s->binding.extent.physical ||
        e.bytes!=s->binding.extent.bytes || s->controller.self!=(uintptr_t)&s->controller ||
        s->controller.memory!=(struct a733_ce_memory *)e.cpu || s->controller.registry!=s->binding.registry ||
        s->controller.cookie!=s->binding.cookie || !token_equal(&s->controller.token,&s->binding.token)) return 0;
    const struct a733_ce_ops *a=&s->controller.ops,*b=&s->binding.ops;
    if (a->now_us!=b->now_us || a->authorize!=b->authorize || a->enter!=b->enter ||
        a->leave!=b->leave || a->retain!=b->retain || a->read!=b->read || a->write!=b->write || a->sync!=b->sync) return 0;
    const struct a733_ce_config *c=&s->controller.config,*d=&s->binding.config;
    if (c->owner!=d->owner || c->handoff!=d->handoff || c->health_profile!=d->health_profile ||
        c->profile!=d->profile || c->configure_health!=d->configure_health || c->descriptor_physical!=d->descriptor_physical ||
        c->output_physical!=d->output_physical) return 0;
    for (unsigned i=0;i<4;++i) if (c->health[i]!=d->health[i]) return 0;
    for (unsigned i=0;i<3;++i) if (c->fabric[i]!=d->fabric[i] || c->fabric_hz[i]!=d->fabric_hz[i]) return 0;
    return 1;
}
int a733_ce_session_bind(struct a733_ce_session *s,struct device_memory_pool *pool,
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
    s->next_generation=1;atomic_init(&s->fault,0);s->initialized=1;
    device_memory_lookup(pool,lease)->state=DEVICE_MEMORY_DEVICE;
    return 1;
}

int a733_ce_session_outside(const struct a733_ce_session *s,const void *p,size_t n)
{return s && s->initialized && separate(s,sizeof(*s),p,n) && outside(s->binding.pool,p,n) &&
    separate(s->binding.registry,sizeof(*s->binding.registry),p,n);}
enum a733_ce_result a733_ce_session_retain(struct a733_ce_session *s)
{
    if (!s || !s->initialized) return A733_CE_INVALID;
    atomic_store_explicit(&s->fault,1,memory_order_release);
    (void)device_memory_retain(s->binding.pool,s->binding.lease);
    if (a733_clock_retain(s->binding.registry,&s->binding.token)==A733_CLOCK_BUSY) return A733_CE_BUSY;
    s->terminal=1;return A733_CE_RETAINED;
}
enum a733_ce_result a733_ce_session_claim(struct a733_ce_session *s,const void *client,size_t n,
    enum a733_ce_client kind,uint64_t *generation)
{
    if (!s || !s->initialized) return A733_CE_INVALID;
    if (s->terminal) return A733_CE_RETAINED;
    if (!a733_ce_session_bound(s)) return a733_ce_session_retain(s);
    if (!client || !n || !generation || !a733_ce_session_outside(s,client,n) ||
        !a733_ce_session_outside(s,generation,sizeof(*generation)) ||
        (kind!=A733_CE_CLIENT_ENTROPY && kind!=A733_CE_CLIENT_SHA256 && kind!=A733_CE_CLIENT_AES128) ||
        (kind==A733_CE_CLIENT_ENTROPY && !(a733_ce_capabilities(s->binding.config.profile)&A733_CE_CAP_TRNG)) ||
        (kind==A733_CE_CLIENT_SHA256 && !(a733_ce_capabilities(s->binding.config.profile)&A733_CE_CAP_SHA256)) ||
        (kind==A733_CE_CLIENT_AES128 && !(a733_ce_capabilities(s->binding.config.profile)&A733_CE_CAP_AES128)) ||
        !s->next_generation) return A733_CE_INVALID;
    if (s->client) return A733_CE_BUSY;
    if (!a733_ce_idle(&s->controller)) return a733_ce_session_retain(s);
    s->client=client;s->client_bytes=n;s->kind=(unsigned)kind;s->generation=s->next_generation++;
    *generation=s->generation;return A733_CE_PENDING;
}
int a733_ce_session_client(struct a733_ce_session *s,const void *client,enum a733_ce_client kind,uint64_t generation)
{return a733_ce_session_bound(s) && s->client==client && s->kind==(unsigned)kind &&
    generation && s->generation==generation;}
int a733_ce_session_done(struct a733_ce_session *s,const void *client,enum a733_ce_client kind,uint64_t generation)
{
    if (!a733_ce_session_client(s,client,kind,generation) || !a733_ce_idle(&s->controller)) return 0;
    s->client=0;s->client_bytes=0;s->kind=0;return 1;
}

/* One physical native CE instance. Serialized boot is a constructor prerequisite;
 * successful ownership is permanent, including after retained faults. */
static struct a733_ce_session *native_owner;
int a733_ce_session_native_claim(struct a733_ce_session *s)
{
    if (!s || s->initialized || native_owner) return 0;
    native_owner=s;return 1;
}
void a733_ce_session_native_abort(struct a733_ce_session *s)
{
    if (s && native_owner==s && !s->initialized) native_owner=0;
}
