/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_rail_leases.h"
#include "a733_range.h"
#include <stddef.h>

_Static_assert(sizeof(struct a733_rail_slot)==32,"one holder slot is half a line");
_Static_assert(sizeof(struct a733_rail_borrow_slot)==32,"one borrow slot is half a line");
_Static_assert(offsetof(struct a733_rail_leases,self)==64 && offsetof(struct a733_rail_leases,busy)<128,
    "status reads the state word line and one header line");
_Static_assert(offsetof(struct a733_rail_leases,slots)%64==0,"holder slots do not straddle lines");
_Static_assert(sizeof(struct a733_rail_leases)<=1632,"ledger storage budget");
_Static_assert(A733_RAIL_CLIENTS==(1u<<A733_RAIL_SLOT_BITS) && A733_RAIL_CLIENTS<=32,"slot index bits");
_Static_assert(A733_RAIL_BORROWS<=(1u<<A733_RAIL_BORROW_BITS),"borrow index bits");
#define GENERATIONS (UINT64_MAX>>A733_RAIL_SLOT_BITS)
#define BORROW_GENERATIONS (UINT64_MAX>>A733_RAIL_BORROW_BITS)

static int external(const struct a733_rail_leases *m,const void *p,size_t n)
{
    return a733_ranges_disjoint(m,sizeof(*m),p,n) &&
        a733_ranges_disjoint(m->binding.clocks,sizeof(*m->binding.clocks),p,n) &&
        a733_ranges_disjoint(m->binding.gpu,sizeof(*m->binding.gpu),p,n);
}
static enum a733_rail_result enter(struct a733_rail_leases *m)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&m->busy,&expected,1,
        memory_order_acquire,memory_order_relaxed) ? A733_RAIL_READY : A733_RAIL_BUSY;
}
static enum a733_rail_result leave(struct a733_rail_leases *m,enum a733_rail_result r)
{atomic_store_explicit(&m->busy,0,memory_order_release);return r;}
static enum a733_rail_result fault(struct a733_rail_leases *m)
{a733_power_authority_set(&m->authority,A733_POWER_LEDGER_FAULT);return A733_RAIL_RETAINED;}
static int faulted(const struct a733_rail_leases *m)
{return !a733_power_authority_admits(&m->authority,A733_POWER_LEDGER_FAULT);}
static enum a733_rail_result time_check(struct a733_rail_leases *m,uint64_t deadline)
{
    uint64_t now;
    if(m->binding.now!=m->sealed_now || m->binding.context!=m->sealed_context ||
       m->binding.now(m->binding.context,&now)!=1 || !a733_monotonic_advance(now,&m->last_now))return fault(m);
    return now<deadline ? A733_RAIL_READY : A733_RAIL_EXPIRED;
}
static a733_rail_mask_t expand(a733_rail_mask_t mask)
{
    const a733_rail_mask_t aliases=A733_RAIL_BIT(A733_RAIL_DCDC1)|
        A733_RAIL_BIT(A733_RAIL_DC1SW1)|A733_RAIL_BIT(A733_RAIL_DC1SW2);
    return mask&aliases ? mask|aliases : mask;
}
/* The exact holder slot of t, or NULL. One slot is compared: the identifier
 * names its slot and is never reused. */
static struct a733_rail_slot *find(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    struct a733_rail_slot *s=&m->slots[t->id&(A733_RAIL_CLIENTS-1)];
    return t->id && s->id==t->id && s->owner==t->owner && s->shared==t->shared &&
        s->exclusive==t->exclusive ? s : NULL;
}
static unsigned slot_index(const struct a733_rail_leases *m,const struct a733_rail_slot *s)
{return (unsigned)(s-m->slots);}
int a733_rail_leases_init(struct a733_rail_leases *m,const struct a733_rail_binding *binding)
{
    if(!m || m->self || !a733_ranges_disjoint(m,sizeof(*m),binding,sizeof(*binding)))return 0;
    const struct a733_rail_binding b=*binding;
    if(!b.clocks || !b.gpu || !b.bridge_owner || !b.now || !b.clocks->initialized ||
        !b.gpu->initialized || atomic_load(&b.clocks->failed) || b.gpu->failed || b.clocks->authority ||
        b.now!=b.clocks->now || b.context!=b.clocks->context ||
        !a733_ranges_disjoint(m,sizeof(*m),b.clocks,sizeof(*b.clocks)) ||
        !a733_ranges_disjoint(m,sizeof(*m),b.gpu,sizeof(*b.gpu)) ||
        !a733_ranges_disjoint(b.clocks,sizeof(*b.clocks),b.gpu,sizeof(*b.gpu)))return 0;
    *m=(struct a733_rail_leases){.self=m,.free_slots=UINT32_MAX>>(32-A733_RAIL_CLIENTS),
        .next_generation=1,.next_borrow=1,.binding=b,.sealed_now=b.now,.sealed_context=b.context};
    atomic_init(&m->authority.word,0);atomic_init(&m->busy,0);
    b.clocks->authority=&m->authority;
    return 1;
}
enum a733_rail_result a733_rail_acquire(struct a733_rail_leases *m,uint64_t owner,
    a733_rail_mask_t shared,a733_rail_mask_t exclusive,uint64_t deadline,struct a733_rail_token *out)
{
    if(!m || m->self!=m || !owner || !(shared|exclusive) || ((shared|exclusive)&~A733_RAIL_ACCEPTED) ||
        (shared&exclusive) || !external(m,out,sizeof(*out)))return A733_RAIL_INVALID;
    shared=expand(shared);exclusive=expand(exclusive);shared&=~exclusive;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    if((exclusive&(m->shared_held|m->exclusive_held)) || (shared&m->exclusive_held) || !m->free_slots)
        return leave(m,A733_RAIL_BUSY);
    if(m->next_generation>GENERATIONS)return leave(m,A733_RAIL_EXHAUSTED);
    r=time_check(m,deadline);if(r!=A733_RAIL_READY)return leave(m,r);
    const unsigned i=(unsigned)__builtin_ctz(m->free_slots);
    const struct a733_rail_token t={m->next_generation++<<A733_RAIL_SLOT_BITS|i,owner,shared,exclusive};
    m->slots[i]=(struct a733_rail_slot){t.id,owner,shared,exclusive};
    m->free_slots&=~(UINT32_C(1)<<i);
    m->exclusive_held|=exclusive;m->shared_held|=shared;
    for(a733_rail_mask_t bits=shared;bits;bits&=bits-1)++m->sharers[__builtin_ctzll(bits)];
    *out=t;return leave(m,A733_RAIL_PENDING);
}
/* Admission is complete at acquire, so the first poll reports READY. */
enum a733_rail_result a733_rail_poll(struct a733_rail_leases *m,const struct a733_rail_token *t)
{return a733_rail_status(m,t);}
enum a733_rail_result a733_rail_status(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!m || m->self!=m || !t)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    if(!find(m,t))return leave(m,A733_RAIL_INVALID);
    return leave(m,faulted(m) ? A733_RAIL_RETAINED : A733_RAIL_READY);
}
static enum a733_rail_result pin(struct a733_rail_leases *m,struct a733_rail_slot *s)
{
    uint8_t *p=&m->pins[slot_index(m,s)];
    if(*p==UINT8_MAX)return A733_RAIL_BUSY;
    ++*p;return A733_RAIL_READY;
}
enum a733_rail_result a733_rail_borrow(struct a733_rail_leases *m,const struct a733_rail_token *t,
    a733_rail_mask_t rails,struct a733_rail_borrow *out)
{
    if(!m || m->self!=m || !rails || (rails&~A733_RAIL_ALL) || !t || !external(m,out,sizeof(*out)) ||
        !a733_ranges_disjoint(t,sizeof(*t),out,sizeof(*out)))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    if(!s || (rails&~(s->shared|s->exclusive)))return leave(m,A733_RAIL_INVALID);
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    unsigned free=A733_RAIL_BORROWS;
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i) {
        struct a733_rail_borrow_slot *b=&m->borrows[i];
        if(!b->id) {if(free==A733_RAIL_BORROWS)free=i;}
        else if(!a733_ranges_disjoint(out,sizeof(*out),b->address,sizeof(*out)))return leave(m,A733_RAIL_INVALID);
    }
    if(free==A733_RAIL_BORROWS)return leave(m,A733_RAIL_BUSY);
    if(m->next_borrow>BORROW_GENERATIONS)return leave(m,A733_RAIL_EXHAUSTED);
    r=pin(m,s);if(r!=A733_RAIL_READY)return leave(m,r);
    const struct a733_rail_borrow b={m->next_borrow++<<A733_RAIL_BORROW_BITS|free,*t,rails};
    m->borrows[free]=(struct a733_rail_borrow_slot){b.id,t->id,out,rails};
    *out=b;return leave(m,A733_RAIL_READY);
}
static enum a733_rail_result borrowed(struct a733_rail_leases *m,const struct a733_rail_borrow *b,int end)
{
    if(!m || m->self!=m || !b)return A733_RAIL_INVALID;
    const unsigned index=(unsigned)(b->id&((1u<<A733_RAIL_BORROW_BITS)-1));
    if(!b->id || index>=A733_RAIL_BORROWS)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_borrow_slot *bs=&m->borrows[index];
    if(bs->id!=b->id || bs->address!=b || bs->rails!=b->rails || bs->parent!=b->parent.id)
        return leave(m,A733_RAIL_INVALID);
    /* A pinned parent cannot be released, so a mismatch is corruption. */
    struct a733_rail_slot *s=find(m,&b->parent);
    if(!s || !m->pins[slot_index(m,s)])return leave(m,fault(m));
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    if(end){--m->pins[slot_index(m,s)];*bs=(struct a733_rail_borrow_slot){0};}
    return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_borrow_status(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{return borrowed(m,b,0);}
enum a733_rail_result a733_rail_end_borrow(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{return borrowed(m,b,1);}
enum a733_rail_result a733_rail_pin(struct a733_rail_leases *m,const struct a733_rail_token *t,a733_rail_mask_t rails)
{
    if(!m || m->self!=m || !t || !rails)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    if(!s || (rails&~(s->shared|s->exclusive)))return leave(m,A733_RAIL_INVALID);
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    return leave(m,pin(m,s));
}
enum a733_rail_result a733_rail_unpin(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!m || m->self!=m || !t)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    if(!s || !m->pins[slot_index(m,s)])return leave(m,A733_RAIL_INVALID);
    --m->pins[slot_index(m,s)];return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_floor(struct a733_rail_leases *m,const struct a733_rail_token *t,
    enum a733_rail rail,uint32_t observed_uv,uint32_t *floor_uv)
{
    if(!m || m->self!=m || !t || !floor_uv || !observed_uv ||
       (rail!=A733_RAIL_DCDC3 && rail!=A733_RAIL_DCDC5))return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);
    if(!s || !(s->exclusive&A733_RAIL_BIT(rail)))return leave(m,A733_RAIL_INVALID);
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    uint32_t *f=&m->floor_uv[rail==A733_RAIL_DCDC5];
    if(!*f)*f=observed_uv;
    *floor_uv=*f;return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_release(struct a733_rail_leases *m,const struct a733_rail_token *t,uint64_t deadline)
{
    if(!m || m->self!=m || !t)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    struct a733_rail_slot *s=find(m,t);if(!s)return leave(m,A733_RAIL_INVALID);
    if(faulted(m))return leave(m,A733_RAIL_RETAINED);
    const unsigned i=slot_index(m,s);
    if(m->pins[i])return leave(m,A733_RAIL_BUSY);
    r=time_check(m,deadline);if(r!=A733_RAIL_READY)return leave(m,r);
    m->exclusive_held&=~s->exclusive;
    for(a733_rail_mask_t bits=s->shared;bits;bits&=bits-1) {
        const unsigned rail=(unsigned)__builtin_ctzll(bits);
        if(!--m->sharers[rail])m->shared_held&=~(UINT64_C(1)<<rail);
    }
    *s=(struct a733_rail_slot){0};m->free_slots|=UINT32_C(1)<<i;
    return leave(m,A733_RAIL_READY);
}
enum a733_rail_result a733_rail_retain(struct a733_rail_leases *m,const struct a733_rail_token *t)
{
    if(!m || m->self!=m || !t)return A733_RAIL_INVALID;
    enum a733_rail_result r=enter(m);if(r!=A733_RAIL_READY)return r;
    return leave(m,find(m,t) ? fault(m) : A733_RAIL_INVALID);
}
