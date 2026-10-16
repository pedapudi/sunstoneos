#include "a733_ce_entropy.h"
#include <stddef.h>
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static void zero(void *p,size_t n)
{volatile uint8_t *b=p;while (n--) *b++=0;}
static int bound(struct a733_ce_entropy *s)
{return s->self==(uintptr_t)s && s->engine==s->bound_engine && a733_ce_session_bound(s->engine);}
static enum a733_ce_entropy_result finish(struct a733_ce_entropy *s,enum a733_ce_entropy_result r)
{atomic_store_explicit(&s->busy,0,memory_order_release);return r;}
static enum a733_ce_entropy_result retain(struct a733_ce_entropy *s)
{
    s->fault=1;zero(s->staging,sizeof(s->staging));
    if (a733_ce_session_retain(s->bound_engine)==A733_CE_BUSY)
        return finish(s,A733_CE_ENTROPY_BUSY);
    s->terminal=1;return finish(s,A733_CE_ENTROPY_RETAINED);
}
static int time_ok(struct a733_ce_entropy *s,uint64_t deadline)
{
    uint64_t now;
    if (s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 || now<s->last_now || now>=deadline) return 0;
    s->last_now=now;return 1;
}
static int output_ok(const struct a733_ce_entropy *s,const void *out,size_t n)
{return separate(s,sizeof(*s),out,n) && a733_ce_session_outside(s->engine,out,n);}
int a733_ce_entropy_attach(struct a733_ce_entropy *s,struct a733_ce_session *engine)
{
    if (!s || s->initialized || !a733_ce_session_bound(engine) || engine->client ||
        engine->binding.config.profile==A733_CE_SHA256_ONLY ||
        !a733_ce_session_outside(engine,s,offsetof(struct a733_ce_entropy,owned_session)) ||
        !separate(s,sizeof(*s),engine->binding.pool,sizeof(*engine->binding.pool)) ||
        !separate(s,sizeof(*s),(void *)engine->binding.pool->backing.cpu,engine->binding.pool->backing.bytes) ||
        !separate(s,sizeof(*s),engine->binding.registry,sizeof(*engine->binding.registry)) ||
        (engine!=&s->owned_session && !separate(s,sizeof(*s),engine,sizeof(*engine)))) return 0;
    s->self=(uintptr_t)s;s->engine=engine;s->bound_engine=engine;
    s->next_generation=1;s->initialized=1;atomic_init(&s->busy,0);return 1;
}
int a733_ce_entropy_bind(struct a733_ce_entropy *s,struct device_memory_pool *pool,
    struct device_memory_lease lease,const struct a733_ce_config *c,const struct a733_ce_ops *o,
    void *cookie,struct a733_clock_leases *registry,const struct a733_clock_token *token)
{
    if (!s || s->initialized || !pool || !c || c->profile==A733_CE_SHA256_ONLY ||
        !separate(s,sizeof(*s),pool,sizeof(*pool)) ||
        !separate(s,sizeof(*s),(void *)pool->backing.cpu,pool->backing.bytes) ||
        !separate(s,sizeof(*s),registry,sizeof(*registry)) ||
        !separate(s,sizeof(*s),c,sizeof(*c)) || !separate(s,sizeof(*s),o,sizeof(*o)) ||
        !separate(s,sizeof(*s),token,sizeof(*token))) return 0;
    if (!a733_ce_session_bind(&s->owned_session,pool,lease,c,o,cookie,registry,token)) return 0;
    return a733_ce_entropy_attach(s,&s->owned_session);
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
    if (s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 || now<s->last_now) return retain(s);
    s->last_now=now;
    if (now>=deadline) return finish(s,A733_CE_ENTROPY_INVALID);
    enum a733_ce_result claimed=a733_ce_session_claim(s->engine,s,offsetof(struct a733_ce_entropy,owned_session),
        A733_CE_CLIENT_ENTROPY,&s->session_generation);
    if (claimed==A733_CE_BUSY) return finish(s,A733_CE_ENTROPY_BUSY);
    if (claimed==A733_CE_INVALID) return finish(s,A733_CE_ENTROPY_INVALID);
    if (claimed!=A733_CE_PENDING) return retain(s);
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
    if (!a733_ce_session_client(s->engine,s,A733_CE_CLIENT_ENTROPY,s->session_generation)) return retain(s);
    if (!time_ok(s,s->deadline)) return retain(s);
    if (s->need_request) {
        enum a733_ce_result r=a733_ce_request(&s->engine->controller,s->deadline);
        if (s->engine->controller.fault || r==A733_CE_RETAINED || r==A733_CE_INVALID || !time_ok(s,s->deadline)) return retain(s);
        if (r==A733_CE_BUSY) return finish(s,A733_CE_ENTROPY_BUSY);
        if (r!=A733_CE_PENDING) return retain(s);
        s->need_request=0;return finish(s,A733_CE_ENTROPY_PENDING);
    }
    uint8_t block[32]={0};
    enum a733_ce_result r=a733_ce_poll(&s->engine->controller,block);
    if (s->engine->controller.fault || r==A733_CE_RETAINED || r==A733_CE_INVALID ||
        r==A733_CE_CONSUMED || r==A733_CE_DIGEST || !time_ok(s,s->deadline)) {zero(block,sizeof(block));return retain(s);}
    if (r==A733_CE_RANDOM) {
        size_t take=s->wanted-s->filled;if (take>32) take=32;
        for (size_t i=0;i<take;++i) s->staging[s->filled+i]=block[i];
        s->filled+=take;
    }
    zero(block,sizeof(block));
    if (s->filled==s->wanted) {
        if (!time_ok(s,s->deadline)) return retain(s);
        if (!a733_ce_session_done(s->engine,s,A733_CE_CLIENT_ENTROPY,s->session_generation)) return retain(s);
        for (size_t i=0;i<s->wanted;++i) out[i]=s->staging[i];
        zero(s->staging,sizeof(s->staging));s->active=0;s->consumed=1;
        return finish(s,A733_CE_ENTROPY_BYTES);
    }
    if (r==A733_CE_RANDOM) s->need_request=1;
    return finish(s,r==A733_CE_BUSY ? A733_CE_ENTROPY_BUSY:A733_CE_ENTROPY_PENDING);
}
