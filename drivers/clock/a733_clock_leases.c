#include "a733_clock_leases.h"
#include "a733_range.h"
#include <stddef.h>

/* Masks and tokens are private metadata under the existing 32-bit lock.
 * Wider masks require rebuilding every consumer; no 128-bit atomic is used. */
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"registry fault publication must be lock-free");
_Static_assert(sizeof(atomic_uint)==sizeof(uint32_t),"32-bit atomic required");
_Static_assert(_Alignof(atomic_uint)==_Alignof(uint32_t),"32-bit atomic alignment required");
_Static_assert(sizeof(a733_clock_mask_t)==16 && _Alignof(a733_clock_mask_t)==16,"128-bit mask ABI");
_Static_assert(sizeof(struct a733_clock_token)==48 && _Alignof(struct a733_clock_token)==16,"private token ABI");
#define OFFSET(type,field,value) _Static_assert(offsetof(struct type,field)==value,"private ABI offset: " #field)
OFFSET(a733_clock_token,id,0); OFFSET(a733_clock_token,owner,8);
OFFSET(a733_clock_token,shared,16); OFFSET(a733_clock_token,exclusive,32);
_Static_assert(sizeof(struct a733_clock_leases)==6208 && _Alignof(struct a733_clock_leases)==16,"private registry ABI");
OFFSET(a733_clock_leases,busy,0); OFFSET(a733_clock_leases,next_id,8);
OFFSET(a733_clock_leases,last_now,16); OFFSET(a733_clock_leases,slots,32);
OFFSET(a733_clock_leases,now,6176); OFFSET(a733_clock_leases,context,6184);
OFFSET(a733_clock_leases,initialized,6192); OFFSET(a733_clock_leases,failed,6196);
OFFSET(a733_clock_leases,authority,6200);
#undef OFFSET
_Static_assert((A733_CLOCK_CLIENTS&(A733_CLOCK_CLIENTS-1))==0,"slot index is a mask of the identifier");

static a733_clock_mask_t parents(a733_clock_mask_t mask)
{
    a733_clock_mask_t p=0;
    if(mask&A733_CLOCK_LRADC)p|=A733_CLOCK_FABRIC|A733_CLOCK_RTC32K;
    if(mask&A733_CLOCK_GPADC)p|=A733_CLOCK_GPADC24M|A733_CLOCK_FABRIC;
    if(mask&A733_CLOCK_PWM0)p|=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    if(mask&(A733_CLOCK_TWI11|A733_CLOCK_TWI9))p|=A733_CLOCK_FABRIC;
    if(mask&(A733_CLOCK_USB0|A733_CLOCK_USB1))p|=A733_CLOCK_USB01_REF;
    if(mask&A733_CLOCK_MAIN_UARTS)p|=A733_CLOCK_APB_UART;
    if(mask&(A733_CLOCK_S_UARTS|A733_CLOCK_S_TWI0|A733_CLOCK_S_TWI1))p|=A733_CLOCK_APBS1;
    if(mask&A733_CLOCK_THS)p|=A733_CLOCK_GPADC24M;
    if(mask&A733_CLOCK_VO0)p|=A733_CLOCK_DISPLL1;
    if((mask|p)&(A733_CLOCK_DISPLL1|A733_CLOCK_CPU_L|A733_CLOCK_CPU_B|A733_CLOCK_CPU_DSU))p|=A733_CLOCK_OSC;
    if(mask&(A733_CLOCK_AUDIO2|A733_CLOCK_AUDIO5))p|=A733_CLOCK_AUDIO1;
    if((mask|p)&(A733_CLOCK_AUDIO1|A733_CLOCK_PERI0|A733_CLOCK_PLL_NPU|
        A733_CLOCK_PLL_VIDEO0|A733_CLOCK_PLL_DE|A733_CLOCK_PLL_VE0|A733_CLOCK_PLL_VE1|
        A733_CLOCK_GPADC24M|A733_CLOCK_APB_UART|A733_CLOCK_APBS1|A733_CLOCK_APBS0|A733_CLOCK_USB01_REF))p|=A733_CLOCK_REF;
    if((mask|p)&A733_CLOCK_REF)p|=A733_CLOCK_OSC;
    return p;
}
static enum a733_clock_result enter(struct a733_clock_leases *m)
{
    if(!m || !m->initialized)return A733_CLOCK_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&m->busy,&expected,1,memory_order_acquire,memory_order_relaxed))
        return A733_CLOCK_BUSY;
    if(atomic_load_explicit(&m->failed,memory_order_acquire)) {atomic_store_explicit(&m->busy,0,memory_order_release);return A733_CLOCK_FAULT;}
    return A733_CLOCK_OK;
}
static enum a733_clock_result leave(struct a733_clock_leases *m,enum a733_clock_result r)
{atomic_store_explicit(&m->busy,0,memory_order_release);return r;}
/* Latch the registry fault, and report it in the bound power-authority word. */
static enum a733_clock_result fail(struct a733_clock_leases *m)
{
    atomic_store_explicit(&m->failed,1,memory_order_release);
    if(m->authority)a733_power_authority_set(m->authority,A733_POWER_CLOCK_FAULT);
    return A733_CLOCK_FAULT;
}
static enum a733_clock_result clock_check(struct a733_clock_leases *m,uint64_t deadline)
{
    uint64_t now;
    if(m->now(m->context,&now)!=1 || !a733_monotonic_advance(now,&m->last_now))return fail(m);
    return now<deadline ? A733_CLOCK_OK : A733_CLOCK_EXPIRED;
}
static int equal(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
/* An identifier names its slot, (id - 1) mod 128, so lookup compares one slot. */
static unsigned home(uint64_t id){return (unsigned)((id-1)&(A733_CLOCK_CLIENTS-1));}
static unsigned find(struct a733_clock_leases *m,const struct a733_clock_token *t)
{
    if(!t->id || !t->owner)return A733_CLOCK_CLIENTS;
    const unsigned i=home(t->id);
    return equal(&m->slots[i],t) ? i : A733_CLOCK_CLIENTS;
}
/* The smallest unused identifier whose slot is free, or whose slot is the one
 * being replaced. Identifiers stay increasing and are never reused. Returns 0
 * when they are exhausted. */
static uint64_t next_identifier(struct a733_clock_leases *m,unsigned replaced)
{
    for(uint64_t id=m->next_id;id;++id) {
        const unsigned i=home(id);
        if(!m->slots[i].id || i==replaced)return id;
        if(id-m->next_id>=A733_CLOCK_CLIENTS)return 0;
    }
    return 0;
}
int a733_clock_leases_init(struct a733_clock_leases *m,int (*now)(void *,uint64_t *),void *ctx)
{
    if(!m || m->initialized || !now)return 0;
    *m=(struct a733_clock_leases){.next_id=1,.now=now,.context=ctx,.initialized=1};
    atomic_init(&m->busy,0);atomic_init(&m->failed,0);return 1;
}
enum a733_clock_result a733_clock_acquire(struct a733_clock_leases *m,uint64_t owner,
    a733_clock_mask_t shared,a733_clock_mask_t exclusive,uint64_t deadline,struct a733_clock_token *out)
{
    const a733_clock_mask_t local=A733_CLOCK_LOCALS|A733_CLOCK_DMA0|A733_CLOCK_NPU|
        A733_CLOCK_MEDIA_LOCALS|A733_CLOCK_THS|A733_CLOCK_MAIN_UARTS|
        A733_CLOCK_S_UARTS|A733_CLOCK_SPIS|A733_CLOCK_CE|A733_CLOCK_USB0|A733_CLOCK_USB1|
        A733_CLOCK_SDC1|A733_CLOCK_RADIO_MODULE|A733_CLOCK_S_TWI0|
        A733_CLOCK_SDC0|A733_CLOCK_SDC2|A733_CLOCK_SDC3|A733_CLOCK_PCIE0|A733_CLOCK_COMB1|A733_CLOCK_COMB0|
        A733_CLOCK_TWI11|A733_CLOCK_TWI9|A733_CLOCK_S_TWI1|A733_CLOCK_USB2|A733_CLOCK_PWM0|A733_CLOCK_GPADC|A733_CLOCK_LRADC;
    if(!owner || !(shared|exclusive) || ((shared|exclusive)&~A733_CLOCK_ALL) ||
        (shared&exclusive) || (shared&local) ||
        !a733_ranges_disjoint(m,sizeof(*m),out,sizeof(*out)))return A733_CLOCK_INVALID;
    shared|=parents(shared|exclusive);shared&=~exclusive;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned held=0;
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i) {
        const struct a733_clock_token *t=&m->slots[i];
        if(!t->id)continue;
        ++held;
        if((exclusive&(t->shared|t->exclusive)) || (shared&t->exclusive))return leave(m,A733_CLOCK_BUSY);
    }
    if(held==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_BUSY);
    const uint64_t id=next_identifier(m,A733_CLOCK_CLIENTS);
    if(!id)return leave(m,A733_CLOCK_EXHAUSTED);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    struct a733_clock_token t={id,owner,shared,exclusive};
    m->next_id=id+1;m->slots[home(id)]=t;*out=t;return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_release(struct a733_clock_leases *m,
    const struct a733_clock_token *token,uint64_t deadline)
{
    if(!a733_ranges_disjoint(m,sizeof(*m),token,sizeof(*token)))return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned slot=find(m,token);
    if(slot==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_INVALID);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    m->slots[slot]=(struct a733_clock_token){0};return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_publish_audio(struct a733_clock_leases *m,
    const struct a733_clock_token *token,uint64_t deadline,struct a733_clock_token *out)
{
    if(!a733_ranges_disjoint(m,sizeof(*m),token,sizeof(*token)) ||
        !a733_ranges_disjoint(m,sizeof(*m),out,sizeof(*out)) || !a733_ranges_disjoint(token,sizeof(*token),out,sizeof(*out)))
        return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned slot=find(m,token);
    if(slot==A733_CLOCK_CLIENTS || token->exclusive!=A733_CLOCK_AUDIO_CHANGE ||
        token->shared!=(A733_CLOCK_REF|A733_CLOCK_OSC))return leave(m,A733_CLOCK_INVALID);
    const uint64_t id=next_identifier(m,slot);
    if(!id)return leave(m,A733_CLOCK_EXHAUSTED);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    struct a733_clock_token t={id,token->owner,parents(A733_CLOCK_AUDIO5)|A733_CLOCK_AUDIO5,0};
    m->next_id=id+1;m->slots[slot]=(struct a733_clock_token){0};m->slots[home(id)]=t;
    *out=t;return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_publish_stable(struct a733_clock_leases *m,
    const struct a733_clock_token *token,a733_clock_mask_t stable,uint64_t deadline,struct a733_clock_token *out)
{
    const a733_clock_mask_t supported=A733_CLOCK_PLL_NPU|A733_CLOCK_DISPLL1|A733_CLOCK_USB01_REF|A733_CLOCK_GPADC24M;
    if(!stable || (stable&~supported) || !a733_ranges_disjoint(m,sizeof(*m),token,sizeof(*token)) ||
        !a733_ranges_disjoint(m,sizeof(*m),out,sizeof(*out)) || !a733_ranges_disjoint(token,sizeof(*token),out,sizeof(*out)))
        return A733_CLOCK_INVALID;
    /* Snapshot before any callback; no caller bytes are consulted at commit. */
    const struct a733_clock_token old=*token;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    unsigned slot=find(m,&old);
    if(slot==A733_CLOCK_CLIENTS || (old.exclusive&stable)!=stable)return leave(m,A733_CLOCK_INVALID);
    const uint64_t id=next_identifier(m,slot);
    if(!id)return leave(m,A733_CLOCK_EXHAUSTED);
    r=clock_check(m,deadline);if(r!=A733_CLOCK_OK)return leave(m,r);
    struct a733_clock_token next=old;
    next.id=id;next.shared|=stable;next.exclusive&=~stable;
    /* No fallible callback or additional refusal after the atomic commit. */
    m->next_id=id+1;m->slots[slot]=(struct a733_clock_token){0};m->slots[home(id)]=next;
    *out=next;return leave(m,A733_CLOCK_OK);
}
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *m,const struct a733_clock_token *token)
{
    if(!a733_ranges_disjoint(m,sizeof(*m),token,sizeof(*token)))return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    if(find(m,token)==A733_CLOCK_CLIENTS)return leave(m,A733_CLOCK_INVALID);
    return leave(m,fail(m));
}

/* Status writes nothing, so a token stored inside the registry cannot change
 * its result; only a NULL token is refused. */
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *m,const struct a733_clock_token *token)
{
    if(!token)return A733_CLOCK_INVALID;
    enum a733_clock_result r=enter(m);if(r!=A733_CLOCK_OK)return r;
    return leave(m,find(m,token)==A733_CLOCK_CLIENTS ? A733_CLOCK_INVALID : A733_CLOCK_OK);
}
