/* SPDX-License-Identifier: GPL-2.0-only
 * Power-domain owner and PMIC service task (docs/power-domains.md).
 * The PCK600 delay row, command and status sequence is adapted from Allwinner
 * pck600_domains.c, Linux revision 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2021 ALLWINNER, Co. Ltd.
 * Source hashes: sources.lock.json; adaptation scope: apps/g2d/sources.json.
 */
#include "a733_power_domains.h"
#include "a733_range.h"

#define BIT(n) (UINT32_C(1)<<(n))
#define CCU UINT32_C(0x02002000)
#define R_CCU UINT32_C(0x07010000)
#define GENERATION_MASK A733_POWER_ROW_GENERATION_MASK
/* Bounds for spins whose clock might stop: each is far above the reads that
 * the longest spin makes on the board. */
#define SPIN_READS (1u<<18)
#define PMIC_POLLS_PER_STEP 256u

/* Per domain: MBUS gates (CCU 0x5c0 and 0x5e0 bits), supply and fault bits
 * (ccu-sun60iw2.c lines 587-601 and 653-677; board device tree npu-supply
 * line 2932 and gpu-supply line 3421). */
struct profile { unsigned id, ahb, mbus; enum a733_rail rail; unsigned verified, fault; };
static const struct profile profiles[A733_POWER_DOMAINS]={
    {1,5,11,A733_RAIL_DCDC2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN1_FAULT},
    {4,6,18,A733_RAIL_DCDC2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN4_FAULT},
    {5,7,16,A733_RAIL_DCDC4,A733_POWER_DCDC4_VERIFIED,A733_POWER_DOMAIN5_FAULT},
};
static int slot(unsigned id)
{return id==1 ? 0 : id==4 ? 1 : id==5 ? 2 : -1;}
static struct a733_power_authority *word(const struct a733_power_domains *d)
{return &d->pmic->rails->authority;}
static int clock(struct a733_power_domains *d,uint64_t *now)
{
    uint64_t t;
    if(a733_pmic_now(d->pmic,&t)!=1 || !a733_monotonic_advance(t,&d->last_now))return 0;
    *now=t;return 1;
}
static int rd(const struct a733_power_domains *d,uintptr_t a,uint32_t *v)
{return d->ops.read(d->context,a,v)==1;}
static int wr(struct a733_power_domains *d,uintptr_t a,uint32_t v)
{return d->ops.write(d->context,a,v)==1;}
static int bits(const struct a733_power_domains *d,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(d,a,&v) && (v&mask)==expected;}
static void publish(struct a733_power_domain_row *r,unsigned generation,enum a733_power_row_state s)
{
    atomic_store_explicit(&r->state,generation<<3|s,memory_order_release);
    if(r->wake)r->wake(r->wake_context);
}
/* After the authority word changes: every row's consumer may be waiting on it. */
static void wake_rows(const struct a733_power_domains *d)
{
    for(const struct a733_power_domain_row *r=d->rows;r;r=r->next)
        if(r->wake)r->wake(r->wake_context);
}
static unsigned answered(const struct a733_power_domain_row *r)
{return atomic_load_explicit(&r->state,memory_order_relaxed)>>3;}

/* Domain-level reads: both MBUS gates and the PPU bus clock (R_CCU 0x1ac
 * bit 0). For domain 5 also the GPU clock gate (0x0b20 bit 31) and the GPU
 * bus gate and reset (0x0b24 bits 0 and 16), which must be closed whenever
 * the domain changes. */
static int level(const struct a733_power_domains *d,const struct profile *f)
{
    return bits(d,CCU+0x5c0,BIT(f->ahb),BIT(f->ahb)) && bits(d,CCU+0x5e0,BIT(f->mbus),BIT(f->mbus)) &&
        bits(d,R_CCU+0x1ac,1,1) &&
        (f->id!=5 || (bits(d,CCU+0xb20,BIT(31),0) && bits(d,CCU+0xb24,BIT(16)|BIT(0),0)));
}
/* Policy (+0x0) and status (+0x8) bits 3:0 must agree at OFF (0) or ON (8). */
static int coherent(const struct a733_power_domains *d,unsigned id,uint32_t *policy)
{
    uint32_t status;const uintptr_t base=A733_POWER_PCK_DOMAIN(id);
    if(!rd(d,base,policy) || !rd(d,base+8,&status) || (*policy&15)!=(status&15))return -1;
    return (status&15)==0 ? 0 : (status&15)==8 ? 8 : -1;
}
/* Class 3: the domain is uncertain. Its counted rows, their rail pins and
 * the domain stay retained; later requests are refused. */
static void fail_domain(struct a733_power_domains *d,struct a733_power_domain *m)
{
    const struct profile *f=&profiles[slot(m->id)];
    m->state=A733_POWER_DOMAIN_FAILED;a733_power_authority_set(word(d),f->fault);
    for(struct a733_power_domain_row *r=d->rows;r;r=r->next) {
        const unsigned g=(atomic_load_explicit(&r->request,memory_order_acquire)>>1)&GENERATION_MASK;
        if(r->domain==m->id && r->counted && answered(r)!=g)publish(r,g,A733_POWER_ROW_FAILED);
    }
}
static void fail_service(struct a733_power_domains *d)
{
    d->failed=1;
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i)
        if(d->domain[i].state==A733_POWER_DOMAIN_ON_WAIT || d->domain[i].state==A733_POWER_DOMAIN_OFF_WAIT)
            fail_domain(d,&d->domain[i]);
}
/* A transition reached its target. ON answers every counted row waiting for
 * ON; OFF leaves the row to release its pin in the row pass. */
static void reached(struct a733_power_domains *d,struct a733_power_domain *m)
{
    const int on=m->state==A733_POWER_DOMAIN_ON_WAIT;
    m->state=on ? A733_POWER_DOMAIN_ON : A733_POWER_DOMAIN_OFF;
    if(!on)return;
    for(struct a733_power_domain_row *r=d->rows;r;r=r->next) {
        const unsigned q=atomic_load_explicit(&r->request,memory_order_acquire),g=(q>>1)&GENERATION_MASK;
        if(r->domain==m->id && r->counted && (q&1) && answered(r)!=g)publish(r,g,A733_POWER_ROW_ON);
    }
}
/* One status check of a transition: 1 reached, 0 still pending, -1 failed.
 * Status may read the starting value or the target, nothing else. */
static int status(struct a733_power_domains *d,struct a733_power_domain *m,uint64_t now)
{
    if(now>=m->deadline)return -1;
    uint32_t v;const unsigned target=m->state==A733_POWER_DOMAIN_ON_WAIT ? 8 : 0;
    if(!rd(d,A733_POWER_PCK_DOMAIN(m->id)+8,&v))return -1;
    if((v&15)==target)return 1;
    if((v&15)!=(8-target))return -1;
    return 0;
}
/* Spin on status for up to A733_POWER_PCK_SPIN_US after the command; the
 * spin bound is timed from the command, which the write accessor has
 * completed with DSB SY. Afterwards one check per step. */
static void advance(struct a733_power_domains *d,struct a733_power_domain *m,int spin)
{
    for(unsigned n=0;;++n) {
        uint64_t now;
        if(!clock(d,&now)){fail_service(d);return;}
        int r=status(d,m,now);
        if(r>0) {
            /* The status read can cross the bound. Never publish a late
             * acknowledgement or release the rail pin on a late OFF. */
            if(!clock(d,&now)){fail_service(d);return;}
            if(now>=m->deadline){fail_domain(d,m);return;}
            reached(d,m);return;
        }
        if(r<0){fail_domain(d,m);return;}
        if(!spin || n>=SPIN_READS || now-m->command_at>=A733_POWER_PCK_SPIN_US)return;
    }
}
static void command(struct a733_power_domains *d,struct a733_power_domain *m,uint32_t policy,unsigned on)
{
    uint64_t now;
    if(!clock(d,&now)){fail_service(d);fail_domain(d,m);return;}
    /* The service deadline is also an admission bound. An expired session
     * cannot program delays or request another power transition. */
    if(now>=d->config.deadline){fail_domain(d,m);return;}
    const uintptr_t base=A733_POWER_PCK_DOMAIN(m->id);
    if(on || !m->delays_ready) {
        /* The pinned delay row, each word read back (pck600_domains.c lines
         * 126-142 and 547-569). The reference initializes it at registration
         * (line 269), even for an inherited ON domain. Initialize before its
         * first OFF too, while keeping adoption read-only and the existing
         * per-ON writes. 0xc00-0xc10 are absent from the PCK-600 TRM. */
        static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        for(unsigned i=0;i<5;++i) {
            uint32_t v;
            if(!wr(d,base+offsets[i],values[i]) || !rd(d,base+offsets[i],&v) || v!=values[i]){fail_domain(d,m);return;}
        }
        m->delays_ready=1;
        if(!clock(d,&now)){fail_service(d);fail_domain(d,m);return;}
        if(now>=d->config.deadline){fail_domain(d,m);return;}
    }
    if(!wr(d,base,(policy&~UINT32_C(15))|(on ? 8 : 0)) || !clock(d,&now) ||
       !a733_deadline_after(now,A733_POWER_PCK_BOUND_US,&m->deadline)){fail_domain(d,m);return;}
    if(m->deadline>d->config.deadline)m->deadline=d->config.deadline;
    m->command_at=now;m->state=on ? A733_POWER_DOMAIN_ON_WAIT : A733_POWER_DOMAIN_OFF_WAIT;
    advance(d,m,1);
}

/* Row admission, returning 1 when the service must run again within a tick. */
static int admit(struct a733_power_domains *d,struct a733_power_domain_row *r,unsigned g)
{
    struct a733_power_domain *m=&d->domain[slot(r->domain)];const struct profile *f=&profiles[slot(r->domain)];
    if(r->counted) {
        if(m->state==A733_POWER_DOMAIN_ON_WAIT || m->state==A733_POWER_DOMAIN_OFF_WAIT)return 1;
        /* FAILED, or OFF for a row whose pin a failed unpin left counted. */
        publish(r,g,m->state==A733_POWER_DOMAIN_ON ? A733_POWER_ROW_ON : A733_POWER_ROW_FAILED);
        return 0;
    }
    if(d->failed || m->state==A733_POWER_DOMAIN_FAILED){publish(r,g,A733_POWER_ROW_REFUSED);return 0;}
    if(m->state==A733_POWER_DOMAIN_OFF_WAIT)return 1;
    const unsigned w=a733_power_authority_load(word(d));
    /* The step waits for verification: the done bit, or for a settled supply
     * the domain's own verified bit, which is published as soon as its rail
     * is verified. */
    if(!(w&(A733_POWER_SUPPLY_DONE|f->verified)))return 0;
    const struct a733_rail_token *t=r->rail;
    if((w&(A733_POWER_PMIC_TRANSFER|f->fault)) || !(w&f->verified) || !t || t->owner!=r->owner ||
       !((t->shared|t->exclusive)&A733_RAIL_BIT(f->rail)) || !level(d,f)){publish(r,g,A733_POWER_ROW_REFUSED);return 0;}
    uint32_t policy=0;
    if(m->state!=A733_POWER_DOMAIN_ON_WAIT) {
        const int ppu=coherent(d,m->id,&policy);
        if(m->state==A733_POWER_DOMAIN_UNKNOWN && ppu>=0)m->state=ppu ? A733_POWER_DOMAIN_ON : A733_POWER_DOMAIN_OFF;
        else if(ppu<0 || (ppu==8)!=(m->state==A733_POWER_DOMAIN_ON)) {
            /* Adoption of an incoherent domain writes nothing; a domain that
             * changed under its owner is uncertain. Either refuses later rows. */
            m->state=A733_POWER_DOMAIN_FAILED;a733_power_authority_set(word(d),f->fault);
            publish(r,g,A733_POWER_ROW_REFUSED);return 0;
        }
    }
    enum a733_rail_result p=a733_rail_pin(d->pmic->rails,t,A733_RAIL_BIT(f->rail));
    if(p==A733_RAIL_BUSY)return 1;
    if(p!=A733_RAIL_READY){publish(r,g,A733_POWER_ROW_REFUSED);return 0;}
    r->counted=1;++m->count;
    if(m->state==A733_POWER_DOMAIN_ON)publish(r,g,A733_POWER_ROW_ON);
    else if(m->state==A733_POWER_DOMAIN_OFF)command(d,m,policy,1);
    return m->state==A733_POWER_DOMAIN_ON_WAIT;
}
/* Domain 5 release: power-off at count zero, after the GPU gate and reset are
 * reread closed; the pin is dropped once the domain is OFF. */
static int release(struct a733_power_domains *d,struct a733_power_domain_row *r,unsigned g)
{
    struct a733_power_domain *m=&d->domain[slot(r->domain)];const struct profile *f=&profiles[slot(r->domain)];
    if(r->domain!=5){publish(r,g,A733_POWER_ROW_REFUSED);return 0;}
    if(!r->counted){publish(r,g,A733_POWER_ROW_OFF);return 0;}
    if(m->state==A733_POWER_DOMAIN_FAILED){publish(r,g,A733_POWER_ROW_FAILED);return 0;}
    if(m->state==A733_POWER_DOMAIN_ON_WAIT || m->state==A733_POWER_DOMAIN_OFF_WAIT)return 1;
    if(m->state==A733_POWER_DOMAIN_ON && m->count==1) {
        uint32_t policy;
        if(!level(d,f) || coherent(d,m->id,&policy)!=8){fail_domain(d,m);return 0;}
        command(d,m,policy,0);
        if(m->state!=A733_POWER_DOMAIN_OFF)return m->state==A733_POWER_DOMAIN_OFF_WAIT;
    }
    enum a733_rail_result p=a733_rail_unpin(d->pmic->rails,r->rail);
    if(p==A733_RAIL_BUSY)return 1;
    /* An unpin that fails leaves the row counted and its pin uncertain. */
    if(p!=A733_RAIL_READY){publish(r,g,A733_POWER_ROW_FAILED);return 0;}
    r->counted=0;--m->count;publish(r,g,A733_POWER_ROW_OFF);return 0;
}

/* Supply verification once per boot: snapshots of DCDC2 and DCDC4. It may
 * start A733_POWER_SUPPLY_SETTLE_US after the service's first step, or at
 * that step for a settled supply, and its
 * A733_POWER_SUPPLY_BUDGET_US starts when it may start and the provider is
 * READY; before READY the provider's startup deadline bounds it. Any failure
 * sets the done bit at once, so waiting rows are refused within a bounded
 * time. Without a settled supply no verified bit is set then. With one, a
 * rail verified before the failure keeps its published bit, because its own
 * snapshot was correct, and rows on that rail stay admissible; a PMIC
 * transfer fault still refuses every row through its own bit. A request whose result is certain (complete
 * or expired) is then reaped; the token is released after success, or after
 * a failure that leaves no request outstanding. An uncertain request keeps
 * its request and the token, which it may still borrow. Returns 1 while it
 * needs the next tick. */
enum { V_TOKEN, V_SUBMIT2, V_RESULT2, V_SUBMIT4, V_RESULT4, V_RELEASE, V_DONE, V_REAP };
static void verified(struct a733_power_domains *d,unsigned ok)
{
    a733_power_authority_set(word(d),A733_POWER_SUPPLY_DONE|(ok ? d->supply_verified : 0));
    wake_rows(d);
    d->supply_stage=ok ? V_RELEASE : d->supply_request.id ? V_REAP : d->supply_stage>V_TOKEN ? V_RELEASE : V_DONE;
}
static int correct(const struct a733_pmic_snapshot *s,enum a733_rail rail)
{
    return s->rail==rail && !s->page_before && s->chip==3 && !s->revision && !s->page_after &&
        s->enabled==1 && s->decoded==1 && s->programmed_uv==800000 && (s->selector&127)==30;
}
static int verify(struct a733_power_domains *d,uint64_t now)
{
    if(d->supply_stage==V_DONE)return 0;
    struct a733_pmic *p=d->pmic;
    if(d->supply_stage==V_REAP) {
        /* After a failure: a complete or expired request ends its borrow. */
        enum a733_pmic_status r=a733_pmic_reap(p,&d->supply_request);
        if(r==A733_PMIC_BUSY)return 1;
        /* A queued or active request ends at its deadline while the provider
         * runs; after a provider fault it stays uncertain. */
        if(r==A733_PMIC_PENDING && !(a733_power_authority_load(word(d))&A733_POWER_PMIC_TRANSFER))return 1;
        if(r!=A733_PMIC_COMPLETE){d->supply_stage=V_DONE;return 0;}
        d->supply_request=(struct a733_pmic_request){0};d->supply_stage=V_RELEASE;return 1;
    }
    if(d->supply_stage==V_RELEASE) {
        enum a733_rail_result r=a733_rail_release(p->rails,&d->supply_token,d->config.deadline);
        if(r==A733_RAIL_BUSY)return 1;
        d->supply_stage=V_DONE;return 0;
    }
    const unsigned w=a733_power_authority_load(word(d));
    if(w&A733_POWER_PMIC_TRANSFER){verified(d,0);return 0;}
    if(!(w&A733_POWER_PROVIDER_READY) || now<d->verify_at)return 0;
    if(!d->verify_deadline && !a733_deadline_after(now,A733_POWER_SUPPLY_BUDGET_US,&d->verify_deadline)){verified(d,0);return 0;}
    if(now>=d->verify_deadline){verified(d,0);return 0;}
    const unsigned rail4=d->supply_stage>=V_SUBMIT4;const enum a733_rail rail=rail4 ? A733_RAIL_DCDC4 : A733_RAIL_DCDC2;
    switch(d->supply_stage) {
    case V_TOKEN: {
        enum a733_rail_result r=a733_rail_acquire(p->rails,d->config.owner,
            A733_RAIL_BIT(A733_RAIL_DCDC2)|A733_RAIL_BIT(A733_RAIL_DCDC4),0,d->verify_deadline,&d->supply_token);
        if(r==A733_RAIL_BUSY)return 1;
        if(r!=A733_RAIL_PENDING){verified(d,0);return 0;}
        ++d->supply_stage;return 1;
    }
    case V_SUBMIT2:case V_SUBMIT4: {
        enum a733_pmic_status r=a733_pmic_submit(p,d->config.owner,&d->supply_token,rail,d->verify_deadline,&d->supply_request);
        if(r==A733_PMIC_BUSY)return 1;
        if(r!=A733_PMIC_PENDING || !d->supply_request.id){d->supply_request=(struct a733_pmic_request){0};verified(d,0);return 0;}
        ++d->supply_stage;return 1;
    }
    case V_RESULT2:case V_RESULT4: {
        struct a733_pmic_snapshot s;
        enum a733_pmic_status r=a733_pmic_result(p,&d->supply_request,&s);
        if(r==A733_PMIC_BUSY || r==A733_PMIC_PENDING)return 1;
        if(r!=A733_PMIC_COMPLETE){verified(d,0);return 1;}
        if(correct(&s,rail)) {
            if(rail4)d->dcdc4=s;
            d->supply_verified|=rail4 ? A733_POWER_DCDC4_VERIFIED : A733_POWER_DCDC2_VERIFIED;
            /* A settled supply publishes each verified rail at once, so its
             * consumers need not wait for the other rail's snapshot. */
            if(d->settled_supply) {
                a733_power_authority_set(word(d),rail4 ? A733_POWER_DCDC4_VERIFIED : A733_POWER_DCDC2_VERIFIED);
                wake_rows(d);
            }
        }
        r=a733_pmic_reap(p,&d->supply_request);
        if(r==A733_PMIC_BUSY)return 1;
        if(r!=A733_PMIC_COMPLETE){verified(d,0);d->supply_stage=V_DONE;return 0;}
        d->supply_request=(struct a733_pmic_request){0};
        if(++d->supply_stage==V_RELEASE)verified(d,1);
        return 1;
    }
    default:verified(d,0);return 0;
    }
}

/* Every queued PMIC request and provider startup. A bus phase is waited for
 * by spinning on the raw counter without the provider object until the time
 * a733_pmic_work() gives, then polling once, for at most
 * A733_POWER_PMIC_SPIN_US in one step. A selector write with a slow bus
 * needs about 56 polls plus re-spins, within PMIC_POLLS_PER_STEP. After a
 * fault latched while this task holds the I2C mutex, one poll marks the
 * mutex retained. Returns 1 while the service must run again within a
 * tick. */
static int pmic(struct a733_power_domains *d,uint64_t start)
{
    struct a733_pmic *p=d->pmic;
    for(unsigned n=0;n<PMIC_POLLS_PER_STEP;++n) {
        unsigned work=a733_pmic_work(p,0,NULL);
        if(work&A733_PMIC_WORK_FAILED)return (work&A733_PMIC_WORK_RETAIN) && a733_pmic_poll(p)==A733_PMIC_BUSY;
        if(!(work&(A733_PMIC_WORK_STARTUP|A733_PMIC_WORK_QUEUED|A733_PMIC_WORK_ACTIVE)))return 0;
        enum a733_pmic_status r=a733_pmic_poll(p);
        if(r==A733_PMIC_BUSY)return 1;
        if(r==A733_PMIC_RETAINED || r==A733_PMIC_INVALID)return 0;
        uint64_t now,until;if(!clock(d,&now))return 1;
        work=a733_pmic_work(p,now,&until);
        if(!(work&A733_PMIC_WORK_WAITING))continue;
        for(unsigned i=0;now<until;++i) {
            if(now-start>=A733_POWER_PMIC_SPIN_US || i>=SPIN_READS || !clock(d,&now))return 1;
        }
        if(now-start>=A733_POWER_PMIC_SPIN_US)return 1;
    }
    return 1;
}

uint32_t a733_power_service_step(struct a733_power_domains *d)
{
    if(!d || d->self!=d)return UINT32_MAX;
    uint64_t now;
    if(!clock(d,&now)){fail_service(d);return UINT32_MAX;}
    if(!d->running) {
        /* An image with no row needs no supply record and makes no bus read. */
        d->running=1;
        if(!d->rows || !a733_deadline_after(now,d->settled_supply ? 0 : A733_POWER_SUPPLY_SETTLE_US,
           &d->verify_at))verified(d,0);
    }
    const uint64_t start=now;
    int tick=0;
    /* Verification submits, the PMIC pass runs the transfer, and the next
     * verification call collects it, within one step when the bus allows. */
    for(unsigned round=0;round<3;++round) {
        const int v=verify(d,now),busy=pmic(d,start);
        tick|=busy;
        if(!clock(d,&now)){fail_service(d);return UINT32_MAX;}
        if(!v || busy)break;
    }
    tick|=verify(d,now);
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i) {
        struct a733_power_domain *m=&d->domain[i];
        if(m->state==A733_POWER_DOMAIN_ON_WAIT || m->state==A733_POWER_DOMAIN_OFF_WAIT)advance(d,m,0);
    }
    for(struct a733_power_domain_row *r=d->rows;r;r=r->next) {
        const unsigned q=atomic_load_explicit(&r->request,memory_order_acquire),g=(q>>1)&GENERATION_MASK;
        if(answered(r)==g)continue;
        if(d->failed && !r->counted){publish(r,g,A733_POWER_ROW_REFUSED);continue;}
        tick|=q&1 ? admit(d,r,g) : release(d,r,g);
    }
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i)
        tick|=d->domain[i].state==A733_POWER_DOMAIN_ON_WAIT || d->domain[i].state==A733_POWER_DOMAIN_OFF_WAIT;
    const unsigned work=a733_pmic_work(d->pmic,0,NULL);
    if(work&A733_PMIC_WORK_FAILED ? work&A733_PMIC_WORK_RETAIN :
       d->pmic->held || (work&(A733_PMIC_WORK_STARTUP|A733_PMIC_WORK_QUEUED|A733_PMIC_WORK_ACTIVE)))tick=1;
    if(!clock(d,&now)){fail_service(d);return UINT32_MAX;}
    if(tick)return 1;
    uint64_t until=UINT64_MAX;
    if(d->supply_stage!=V_DONE)until=d->verify_at>now ? d->verify_at : now+d->config.tick_us;
    if(d->config.deadline>now && d->config.deadline<until)until=d->config.deadline;
    if(d->config.idle_us && now+d->config.idle_us<until)until=now+d->config.idle_us;
    if(until==UINT64_MAX)return UINT32_MAX;
    const uint64_t ticks=(until-now+d->config.tick_us-1)/d->config.tick_us;
    return ticks<1 ? 1 : ticks>=UINT32_MAX ? UINT32_MAX-1 : (uint32_t)ticks;
}

int a733_power_domains_init(struct a733_power_domains *d,struct a733_pmic *p,
    const struct a733_power_domain_ops *o,void *context,const struct a733_power_service_config *c)
{
    if(!d || d->self || !p || p->self!=p || !o || !o->read || !o->write || !o->notify || !c ||
       !c->owner || !c->deadline || !c->tick_us || !a733_ranges_disjoint(d,sizeof(*d),p,sizeof(*p)) ||
       !a733_ranges_disjoint(d,sizeof(*d),o,sizeof(*o)) || !a733_ranges_disjoint(d,sizeof(*d),c,sizeof(*c)))return 0;
    const struct a733_power_domain_ops ops=*o;const struct a733_power_service_config config=*c;
    *d=(struct a733_power_domains){.pmic=p,.ops=ops,.context=context,.config=config};
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i)d->domain[i].id=profiles[i].id;
    if(!a733_pmic_bind_service(p,ops.notify,context,d))return 0;
    d->self=d;return 1;
}
int a733_power_domains_settled_supply(struct a733_power_domains *d)
{
    if(!d || d->self!=d || d->running)return 0;
    d->settled_supply=1;return 1;
}
int a733_power_service_idle(const struct a733_power_domains *d)
{
    if(!d || d->self!=d)return 0;
    const unsigned work=a733_pmic_work(d->pmic,0,NULL);
    if(d->pmic->held || (work&(A733_PMIC_WORK_QUEUED|A733_PMIC_WORK_ACTIVE)) ||
       (d->supply_stage!=V_DONE && d->supply_request.id))return 0;
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i)
        if(d->domain[i].state==A733_POWER_DOMAIN_ON_WAIT || d->domain[i].state==A733_POWER_DOMAIN_OFF_WAIT)return 0;
    for(const struct a733_power_domain_row *r=d->rows;r;r=r->next)
        if(answered(r)!=((atomic_load_explicit(&r->request,memory_order_acquire)>>1)&GENERATION_MASK))return 0;
    return 1;
}
int a733_power_service_ready(const struct a733_power_domains *d)
{
    if(!d || d->self!=d)return -1;
    const unsigned w=a733_power_authority_load(word(d));
    return w&A733_POWER_PMIC_TRANSFER ? -1 : w&A733_POWER_PROVIDER_READY ? 1 : 0;
}
int a733_power_domain_check(struct a733_power_domains *d,unsigned domain,unsigned *state)
{
    uint32_t policy;
    if(!d || d->self!=d || slot(domain)<0)return 0;
    const struct profile *f=&profiles[slot(domain)];
    if(!bits(d,CCU+0x5c0,BIT(f->ahb),BIT(f->ahb)) || !bits(d,CCU+0x5e0,BIT(f->mbus),BIT(f->mbus)) ||
       !bits(d,R_CCU+0x1ac,1,1))return 0;
    const int status=coherent(d,domain,&policy);
    if(status<0)return 0;
    if(state)*state=(unsigned)status;
    return 1;
}
