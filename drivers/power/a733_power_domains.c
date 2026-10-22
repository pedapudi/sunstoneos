/* SPDX-License-Identifier: Apache-2.0 */
/* PCK600 domain owner for domains 1, 4, 5 and 10. The command, status and
 * delay-row offsets and values follow the pinned Allwinner pck600_domains.c
 * (Linux revision 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f), as
 * drivers/power/a733_de_sys_power.c does. */
#include "a733_power_authority_domains.h"
#include "a733_range.h"

#define BIT(n) (UINT32_C(1)<<(n))
#define CCU UINT32_C(0x02002000)
#define PCK_CLOCK (UINT32_C(0x07010000)+0x1ac) /* R_CCU PCK600 bus clock, bit 0. */
#define GPU_CLOCK (CCU+0x0b20)                   /* GPU core clock gate, bit 31. */
#define GPU_RESET (CCU+0x0b24)                   /* GPU bus gate 0, reset 16. */
#define CMD 0x0u
#define STATUS 0x8u
#define PCK_OFF 0u
#define PCK_ON 8u
#define SPIN_CAP 65536u /* Status reads per spin; more means the counter stopped. */
enum { UNKNOWN, OFF, ON_WAIT, ON, OFF_WAIT, FAILED };
#define D2 A733_RAIL_BIT(A733_RAIL_DCDC2)
#define D4 A733_RAIL_BIT(A733_RAIL_DCDC4)
#define C2 A733_RAIL_BIT(A733_RAIL_CLDO2)
#define C5 A733_RAIL_BIT(A733_RAIL_CLDO5)
struct info { unsigned id; uint32_t gate_5c0, gate_5e0; a733_rail_mask_t rails; unsigned verified, fault; };
static const struct info table[A733_POWER_DOMAIN_COUNT]={
    {1,BIT(5),BIT(11),D2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN1_FAULT},
    {4,BIT(6),BIT(18),D2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN4_FAULT},
    {5,BIT(7),BIT(16),D4,A733_POWER_DCDC4_VERIFIED,A733_POWER_DOMAIN5_FAULT},
    {10,0,0,D2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN10_FAULT},
};
static const struct info *lookup(unsigned id)
{
    for(unsigned i=0;i<A733_POWER_DOMAIN_COUNT;++i)if(table[i].id==id)return &table[i];
    return 0;
}
static struct a733_power_domain *state_of(struct a733_power_domains *o,const struct info *d)
{return &o->domain[d-table];}
static struct a733_power_authority *word(struct a733_power_domains *o){return &o->rails->authority;}
static int lock(struct a733_power_domains *o)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&o->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static void unlock(struct a733_power_domains *o){atomic_store_explicit(&o->busy,0,memory_order_release);}
static enum a733_power_domain_result unlocked(struct a733_power_domains *o,enum a733_power_domain_result r)
{unlock(o);return r;}
/* Under the lock: latch FAILED and the domain's fault bit. Rows, tokens and
 * the PCK lease stay held. */
static enum a733_power_domain_result fail(struct a733_power_domains *o,const struct info *d)
{state_of(o,d)->state=FAILED;a733_power_authority_set(word(o),d->fault);return A733_DOMAIN_RETAINED;}
static int rd(struct a733_power_domains *o,uintptr_t a,uint32_t *v){return o->ops.read(o->context,a,v)==1;}
static int wr(struct a733_power_domains *o,uintptr_t a,uint32_t v){return o->ops.write(o->context,a,v)==1;}
static int now(struct a733_power_domains *o,uint64_t *out)
{return o->ops.now(o->context,out)==1 && a733_monotonic_advance(*out,&o->last_now);}
/* The domain-level reads: PCK bus clock and, for 1, 4 and 5, both MBUS gates.
 * Plain reads of Device-nGnRnE registers stay in order. */
static int domain_level(struct a733_power_domains *o,const struct info *d)
{
    uint32_t v;
    if(!rd(o,PCK_CLOCK,&v) || !(v&1))return 0;
    if(!d->gate_5c0)return 1;
    return rd(o,CCU+0x5c0,&v) && (v&d->gate_5c0) && rd(o,CCU+0x5e0,&v) && (v&d->gate_5e0);
}
/* Command and status nibbles; 0 on a failed read. */
static int pck(struct a733_power_domains *o,const struct info *d,uint32_t *command,uint32_t *status)
{return rd(o,A733_PCK_BLOCK(d->id)+CMD,command) && rd(o,A733_PCK_BLOCK(d->id)+STATUS,status);}
static int gpu_closed(struct a733_power_domains *o)
{
    uint32_t clock,reset;
    return rd(o,GPU_CLOCK,&clock) && rd(o,GPU_RESET,&reset) && !(clock&BIT(31)) && !(reset&(BIT(0)|BIT(16)));
}
/* Under the lock: adopt on first use, then require command and status to
 * agree with the recorded state. 1 coherent, 0 failed read, -1 FAILED. */
static int coherent(struct a733_power_domains *o,const struct info *d)
{
    struct a733_power_domain *s=state_of(o,d);
    uint32_t command,status;
    if(s->state==FAILED)return -1;
    if(!pck(o,d,&command,&status))return 0;
    const unsigned c=command&15,t=status&15;
    if(s->state==UNKNOWN) {
        if(c==PCK_OFF && t==PCK_OFF){s->state=OFF;return 1;}
        if(c==PCK_ON && t==PCK_ON && (d->id!=5 || gpu_closed(o))){s->state=ON;return 1;}
        fail(o,d);return -1;
    }
    const int ok=s->state==OFF ? c==PCK_OFF && t==PCK_OFF : s->state==ON ? c==PCK_ON && t==PCK_ON :
        s->state==ON_WAIT ? c==PCK_ON && (t==PCK_OFF || t==PCK_ON) : c==PCK_OFF && (t==PCK_OFF || t==PCK_ON);
    if(!ok){fail(o,d);return -1;}
    return 1;
}
/* Under the lock: program the pinned delay row and write the command, then
 * one DSB SY, because the acknowledgement spin is timed from the command
 * reaching the PPU. */
static int command(struct a733_power_domains *o,const struct info *d,unsigned target)
{
    struct a733_power_domain *s=state_of(o,d);
    const uintptr_t base=A733_PCK_BLOCK(d->id);
    uint32_t value;uint64_t start;
    if(!rd(o,base+CMD,&value) || !now(o,&start))return 0;
    if(target==PCK_ON) {
        static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        for(unsigned i=0;i<5;++i)if(!wr(o,base+offsets[i],values[i]))return 0;
    }
    if(!a733_deadline_after(o->last_now,A733_POWER_DOMAIN_WAIT_US,&s->deadline) ||
       !wr(o,base+CMD,(value&~UINT32_C(15))|target))return 0;
    o->ops.barrier(o->context);
    s->state=target==PCK_ON ? ON_WAIT : OFF_WAIT;
    return 1;
}
/* Without the lock: spin on the status nibble for at most the provisional
 * bound, on raw counter ticks. 1 reached, 0 not yet, -1 other value, failed
 * read or a stopped counter. */
static int spin(struct a733_power_domains *o,const struct info *d,unsigned target)
{
    const uint64_t start=o->ops.ticks(o->context),limit=(uint64_t)A733_POWER_DOMAIN_SPIN_US*o->ticks_per_us;
    for(unsigned n=0;n<SPIN_CAP;++n) {
        uint32_t status;
        if(!rd(o,A733_PCK_BLOCK(d->id)+STATUS,&status))return -1;
        if((status&15)==target)return 1;
        if((status&15)!=PCK_OFF && (status&15)!=PCK_ON)return -1;
        if(o->ops.ticks(o->context)-start>=limit)return 0;
    }
    return -1;
}
/* Continue a transition toward target from the state wait. Any requester's
 * step can do this; only the state it read is replaced. */
static enum a733_power_domain_result progress(struct a733_power_domains *o,const struct info *d,unsigned target)
{
    struct a733_power_domain *s=state_of(o,d);
    const unsigned waiting=target==PCK_ON ? ON_WAIT : OFF_WAIT,done=target==PCK_ON ? ON : OFF;
    if(!lock(o))return A733_DOMAIN_BUSY;
    if(s->state==done)return unlocked(o,A733_DOMAIN_READY);
    if(s->state!=waiting)return unlocked(o,s->state==FAILED ? A733_DOMAIN_RETAINED : A733_DOMAIN_AGAIN);
    unlock(o);
    const int r=spin(o,d,target);
    uint64_t t;
    if(!lock(o))return A733_DOMAIN_AGAIN;
    if(s->state!=waiting)
        return unlocked(o,s->state==done ? A733_DOMAIN_READY : s->state==FAILED ? A733_DOMAIN_RETAINED : A733_DOMAIN_AGAIN);
    if(r>0){s->state=done;return unlocked(o,A733_DOMAIN_READY);}
    if(r<0 || !now(o,&t) || t>=s->deadline)return unlocked(o,fail(o,d));
    return unlocked(o,A733_DOMAIN_AGAIN);
}
static unsigned row_slot(const struct a733_power_domain *s,uint64_t id)
{
    for(unsigned i=0;i<A733_POWER_DOMAIN_ROWS;++i)if(id && s->row_ids[i]==id)return i;
    return A733_POWER_DOMAIN_ROWS;
}
int a733_power_domains_init(struct a733_power_domains *o,const struct a733_power_domain_ops *ops,
    void *context,struct a733_rail_leases *rails,uint32_t ticks_per_us)
{
    if(!o || o->self || !ops || !ops->now || !ops->ticks || !ops->read || !ops->write || !ops->barrier ||
       !rails || rails->self!=rails || !ticks_per_us ||
       !a733_ranges_disjoint(o,sizeof(*o),rails,sizeof(*rails)))return 0;
    *o=(struct a733_power_domains){.self=o,.ops=*ops,.context=context,.rails=rails,
        .ticks_per_us=ticks_per_us,.next_row=1};
    atomic_init(&o->busy,0);return 1;
}
enum a733_power_domain_result a733_power_domain_check(struct a733_power_domains *o,unsigned id)
{
    const struct info *d=lookup(id);
    if(!o || o->self!=o || !d)return A733_DOMAIN_INVALID;
    if(!a733_power_authority_admits(word(o),d->fault))return A733_DOMAIN_RETAINED;
    if(!lock(o))return A733_DOMAIN_BUSY;
    if(!domain_level(o,d))return unlocked(o,A733_DOMAIN_REFUSED);
    const int c=coherent(o,d);
    return unlocked(o,c>0 ? A733_DOMAIN_READY : c<0 ? A733_DOMAIN_RETAINED : A733_DOMAIN_REFUSED);
}
enum a733_power_domain_result a733_power_domain_request(struct a733_power_domains *o,unsigned id,
    const struct a733_rail_token *token,struct a733_power_row *row)
{
    const struct info *d=lookup(id);
    if(!o || o->self!=o || !d || !row || !a733_ranges_disjoint(o,sizeof(*o),row,sizeof(*row)))
        return A733_DOMAIN_INVALID;
    struct a733_power_domain *s=state_of(o,d);
    const unsigned w=a733_power_authority_load(word(o));
    if(w&(d->fault|A733_POWER_LEDGER_FAULT))return A733_DOMAIN_RETAINED;
    if(row->id) {
        if(row->domain!=id || row_slot(s,row->id)==A733_POWER_DOMAIN_ROWS)return A733_DOMAIN_INVALID;
        return progress(o,d,PCK_ON);
    }
    if(!token)return A733_DOMAIN_INVALID;
    /* The supplies: one load, no PMIC request. */
    if(!(w&A733_POWER_SUPPLY_DONE))return w&A733_POWER_PROVIDER_FAULT ? A733_DOMAIN_RETAINED : A733_DOMAIN_PENDING;
    const struct a733_rail_token t=*token;
    const a733_rail_mask_t held=t.shared|t.exclusive;
    a733_rail_mask_t rails=d->rails;unsigned verified=d->verified;
    if(id==10) {
        if(!(held&(C2|C5)))return A733_DOMAIN_INVALID;
        if(held&C2){rails|=C2;verified|=A733_POWER_CLDO2_VERIFIED;}
        if(held&C5){rails|=C5;verified|=A733_POWER_CLDO5_VERIFIED;}
    }
    if((held&rails)!=rails)return A733_DOMAIN_INVALID;
    if((w&verified)!=verified)return A733_DOMAIN_REFUSED;
    if(!lock(o))return A733_DOMAIN_BUSY;
    if(!domain_level(o,d))return unlocked(o,A733_DOMAIN_REFUSED);
    const int c=coherent(o,d);
    if(c<=0)return unlocked(o,c<0 ? A733_DOMAIN_RETAINED : A733_DOMAIN_REFUSED);
    /* Commands are written only from OFF or ON. */
    if(s->state==OFF_WAIT)return unlocked(o,A733_DOMAIN_AGAIN);
    unsigned free=A733_POWER_DOMAIN_ROWS;
    for(unsigned i=0;i<A733_POWER_DOMAIN_ROWS;++i)if(!s->row_ids[i]){free=i;break;}
    if(free==A733_POWER_DOMAIN_ROWS)return unlocked(o,A733_DOMAIN_BUSY);
    const enum a733_rail_result p=a733_rail_pin(o->rails,&t,rails);
    if(p!=A733_RAIL_READY)return unlocked(o,p==A733_RAIL_BUSY ? A733_DOMAIN_BUSY :
        p==A733_RAIL_RETAINED ? A733_DOMAIN_RETAINED : A733_DOMAIN_INVALID);
    s->row_ids[free]=o->next_row++;s->tokens[free]=t;++s->rows;
    *row=(struct a733_power_row){id,s->row_ids[free],t};
    if(s->state==ON)return unlocked(o,A733_DOMAIN_READY);
    if(s->state==OFF && !command(o,d,PCK_ON))return unlocked(o,fail(o,d));
    unlock(o);
    return progress(o,d,PCK_ON);
}
/* Under the lock: forget the row and unpin its token. */
static enum a733_power_domain_result drop(struct a733_power_domains *o,struct a733_power_domain *s,
    unsigned slot,struct a733_power_row *row)
{
    if(a733_rail_unpin(o->rails,&s->tokens[slot])!=A733_RAIL_READY)return A733_DOMAIN_RETAINED;
    s->row_ids[slot]=0;s->tokens[slot]=(struct a733_rail_token){0};--s->rows;
    *row=(struct a733_power_row){0};return A733_DOMAIN_READY;
}
enum a733_power_domain_result a733_power_domain_release(struct a733_power_domains *o,struct a733_power_row *row)
{
    const struct info *d=row ? lookup(row->domain) : 0;
    if(!o || o->self!=o || !d)return A733_DOMAIN_INVALID;
    struct a733_power_domain *s=state_of(o,d);
    if(!a733_power_authority_admits(word(o),d->fault|A733_POWER_LEDGER_FAULT))return A733_DOMAIN_RETAINED;
    if(!lock(o))return A733_DOMAIN_BUSY;
    const unsigned slot=row_slot(s,row->id);
    if(slot==A733_POWER_DOMAIN_ROWS)return unlocked(o,A733_DOMAIN_INVALID);
    if(s->state==FAILED)return unlocked(o,A733_DOMAIN_RETAINED);
    /* Domains 1, 4 and 10 have several bus masters and stay on. */
    if(d->id!=5 || s->rows>1)return unlocked(o,drop(o,s,slot,row));
    if(s->state==ON_WAIT) {
        /* Finish the power-on first; the next call powers off. */
        unlock(o);
        const enum a733_power_domain_result r=progress(o,d,PCK_ON);
        return r==A733_DOMAIN_READY ? A733_DOMAIN_AGAIN : r;
    }
    if(s->state==ON) {
        /* The GPU is the domain's one bus master; its clock and resets must be
         * closed after it drained its DMA. */
        if(!gpu_closed(o))return unlocked(o,A733_DOMAIN_REFUSED);
        const int c=coherent(o,d);
        if(c<=0)return unlocked(o,c<0 ? A733_DOMAIN_RETAINED : A733_DOMAIN_REFUSED);
        if(!command(o,d,PCK_OFF))return unlocked(o,fail(o,d));
    }
    unlock(o);
    enum a733_power_domain_result r=progress(o,d,PCK_OFF);
    if(r!=A733_DOMAIN_READY)return r;
    if(!lock(o))return A733_DOMAIN_AGAIN;
    if(s->state!=OFF || row_slot(s,row->id)!=slot)return unlocked(o,A733_DOMAIN_AGAIN);
    return unlocked(o,drop(o,s,slot,row));
}
uint32_t a733_power_npu_supply_uv(const struct a733_rail_leases *rails)
{
    const unsigned need=A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED;
    return rails && (a733_power_authority_load(&rails->authority)&need)==need ? 800000u : 0u;
}
/* Supply verification. */
static const struct { enum a733_rail rail; uint32_t uv; unsigned bit; } supplies[]={
    {A733_RAIL_DCDC2,800000,A733_POWER_DCDC2_VERIFIED},{A733_RAIL_DCDC4,800000,A733_POWER_DCDC4_VERIFIED},
    {A733_RAIL_CLDO2,1800000,A733_POWER_CLDO2_VERIFIED},{A733_RAIL_CLDO5,1800000,A733_POWER_CLDO5_VERIFIED},
};
enum { V_WAIT, V_TOKEN, V_READ, V_RELEASE=V_READ+4, V_DONE };
enum a733_power_domain_result a733_power_domains_verify(struct a733_power_domains *o,struct a733_pmic *p,
    const struct a733_pmic_lifetime *life,uint64_t owner,uint64_t after,uint64_t deadline)
{
    if(!o || o->self!=o || !p || !life || !owner || p->rails!=o->rails)return A733_DOMAIN_INVALID;
    if(o->verify_stage==V_DONE)return A733_DOMAIN_READY;
    if(o->verify_owner && (owner!=o->verify_owner || after!=o->verify_after || deadline!=o->verify_deadline))
        return A733_DOMAIN_INVALID;
    o->verify_owner=owner;o->verify_after=after;o->verify_deadline=deadline;o->lifetime=*life;
    /* The service task's own time history: domain steps on other tasks use
     * last_now under the owner lock. */
    uint64_t t;
    if(o->ops.now(o->context,&t)!=1 || !a733_monotonic_advance(t,&o->verify_last))return A733_DOMAIN_RETAINED;
    if(o->verify_stage==V_WAIT) {
        if(t<after)return A733_DOMAIN_PENDING;
        o->verify_stage=V_TOKEN;
    }
    if(o->verify_stage==V_TOKEN) {
        enum a733_rail_result r=a733_rail_acquire(o->rails,owner,D2|D4|C2|C5,0,deadline,&o->supply);
        if(r==A733_RAIL_BUSY)return A733_DOMAIN_PENDING;
        if(r!=A733_RAIL_PENDING)return A733_DOMAIN_RETAINED;
        o->verify_stage=V_READ;
    }
    while(o->verify_stage>=V_READ && o->verify_stage<V_RELEASE) {
        const unsigned i=o->verify_stage-V_READ;
        if(!o->request.id) {
            enum a733_pmic_status s=a733_pmic_submit(p,owner,&o->supply,supplies[i].rail,deadline,&o->request);
            if(s==A733_PMIC_BUSY || (s==A733_PMIC_PENDING && !o->request.id))return A733_DOMAIN_PENDING;
            if(s==A733_PMIC_EXPIRED){o->verify_stage=V_RELEASE;break;}
            if(s!=A733_PMIC_PENDING)return A733_DOMAIN_RETAINED;
        }
        struct a733_pmic_snapshot snapshot;
        enum a733_pmic_status s=a733_pmic_result(p,&o->request,&snapshot);
        if(s==A733_PMIC_PENDING || s==A733_PMIC_BUSY)return A733_DOMAIN_PENDING;
        if(s!=A733_PMIC_COMPLETE && s!=A733_PMIC_EXPIRED)return A733_DOMAIN_RETAINED;
        enum a733_pmic_status reaped=a733_pmic_reap(p,&o->request);
        if(reaped==A733_PMIC_BUSY)return A733_DOMAIN_PENDING;
        if(reaped!=A733_PMIC_COMPLETE)return A733_DOMAIN_RETAINED;
        o->request=(struct a733_pmic_request){0};
        /* An expired read leaves this and the remaining bits clear. */
        if(s==A733_PMIC_EXPIRED){o->verify_stage=V_RELEASE;break;}
        if(snapshot.rail==supplies[i].rail && snapshot.enabled && snapshot.decoded &&
           snapshot.programmed_uv==supplies[i].uv)a733_power_authority_set(word(o),supplies[i].bit);
        ++o->verify_stage;
    }
    if(o->verify_stage==V_RELEASE) {
        enum a733_rail_result r=a733_rail_release(o->rails,&o->supply,UINT64_MAX);
        if(r==A733_RAIL_BUSY)return A733_DOMAIN_PENDING;
        if(r!=A733_RAIL_READY)return A733_DOMAIN_RETAINED;
        o->supply=(struct a733_rail_token){0};
        a733_power_authority_set(word(o),A733_POWER_SUPPLY_DONE);
        o->verify_stage=V_DONE;
    }
    return A733_DOMAIN_READY;
}
