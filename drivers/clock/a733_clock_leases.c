#include "a733_clock_leases.h"
#include <stddef.h>

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static uint32_t parents(uint32_t mask)
{
    uint32_t p=0;
    if(mask&(A733_CLOCK_AUDIO2|A733_CLOCK_AUDIO5))p|=A733_CLOCK_AUDIO1;
    if((mask|p)&(A733_CLOCK_AUDIO1|A733_CLOCK_PERI0))p|=A733_CLOCK_REF;
    if((mask|p)&A733_CLOCK_REF)p|=A733_CLOCK_OSC;
    return p;
}
static enum a733_clock_result enter(struct a733_clock_leases *m)
{
    if(!m || !m->initialized)return A733_CLOCK_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&m->busy,&expected,1,memory_order_acquire,memory_order_relaxed))
        return A733_CLOCK_BUSY;
    if(m->failed) {atomic_store_explicit(&m->busy,0,memory_order_release);return A733_CLOCK_FAULT;}
    return A733_CLOCK_OK;
}
static enum a733_clock_result leave(struct a733_clock_leases *m,enum a733_clock_result r)
{atomic_store_explicit(&m->busy,0,memory_order_release);return r;}
static enum a733_clock_result clock_check(struct a733_clock_leases *m,uint64_t deadline)
{
    uint64_t now;
    if(m->now(m->context,&now)!=1 || now<m->last_now) {m->failed=1;return A733_CLOCK_FAULT;}
    m->last_now=now;
    return now<deadline ? A733_CLOCK_OK : A733_CLOCK_EXPIRED;
}
static int equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static unsigned find(struct a733_clock_leases *m,const struct a733_clock_token *t)
{
    if(!t->id || !t->owner)return A733_CLOCK_CLIENTS;
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)if(equal(&m->slots[i],t))return i;
    return A733_CLOCK_CLIENTS;
}
int a733_clock_leases_init(struct a733_clock_leases *m,int (*now)(void *,uint64_t *),void *ctx)
{
    if(!m || m->initialized || !now)return 0;
    *m=(struct a733_clock_leases){.next_id=1,.now=now,.context=ctx,.initialized=1};
    atomic_init(&m->busy,0);return 1;
}
enum a733_clock_result a733_clock_acquire(struct a733_clock_leases *m,uint64_t owner,
    uint32_t shared,uint32_t exclusive,uint64_t deadline,struct a733_clock_token *out)
{
    if(!owner || !(shared|exclusive) || ((shared|exclusive)&~A733_CLOCK_ALL) ||
        (shared&exclusive) || (shared&(A733_CLOCK_LOCALS|A733_CLOCK_DMA0)) ||
        !separate(m,sizeof(*m),out,sizeof(*out)))return A733_CLOCK_INVALID;
    shared|=parents(shared|exclusive);shared&=~exclusive;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned free=A733_CLOCK_CLIENTS;
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i) {
        const struct a733_clock_token *t=&m->slots[i];
        if(!t->id) {if(free==A733_CLOCK_CLIENTS)free=i;continue;}
        if((exclusive&(t->shared|t->exclusive)) || (shared&t->exclusive))return leave(m,A733_CLOCK_BUSY);
    }
    if(free==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_BUSY);
    if(!m->next_id)return leave(m,A733_CLOCK_EXHAUSTED);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    struct a733_clock_token t={m->next_id++,owner,shared,exclusive};
    if(exclusive&(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_AUDIO1|A733_CLOCK_AUDIO2|A733_CLOCK_AUDIO5))
        m->audio_profile=0;
    m->slots[free]=t;*out=t;return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_release(struct a733_clock_leases *m,
    const struct a733_clock_token *token,uint64_t deadline)
{
    if(!separate(m,sizeof(*m),token,sizeof(*token)))return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned slot=find(m,token);
    if(slot==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_INVALID);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    m->slots[slot]=(struct a733_clock_token){0};return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_publish_audio(struct a733_clock_leases *m,
    const struct a733_clock_token *token,uint64_t deadline,struct a733_clock_token *out)
{
    if(!separate(m,sizeof(*m),token,sizeof(*token)) ||
        !separate(m,sizeof(*m),out,sizeof(*out)) || !separate(token,sizeof(*token),out,sizeof(*out)))
        return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned slot=find(m,token);
    if(slot==A733_CLOCK_CLIENTS || token->exclusive!=A733_CLOCK_AUDIO_CHANGE ||
        token->shared!=(A733_CLOCK_REF|A733_CLOCK_OSC))return leave(m,A733_CLOCK_INVALID);
    if(!m->next_id)return leave(m,A733_CLOCK_EXHAUSTED);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    struct a733_clock_token t={m->next_id++,token->owner,parents(A733_CLOCK_AUDIO5)|A733_CLOCK_AUDIO5,0};
    m->audio_profile=1;m->slots[slot]=t;*out=t;return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *m,const struct a733_clock_token *token)
{
    if(!separate(m,sizeof(*m),token,sizeof(*token)))return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    if(find(m,token)==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_INVALID);
    m->failed=1;return leave(m,A733_CLOCK_FAULT);
}
