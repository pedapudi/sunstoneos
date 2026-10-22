/* SPDX-License-Identifier: GPL-2.0-only */
/* PCK600 domains over modeled registers, in two parts.
 *
 * The per-consumer display provider (domains 1 and 10): an OFF domain receives
 * the pinned sun60iw2 delay row and one ON8 request at its own block,
 * preserving other command bits; an ON domain is adopted without writes.
 * Other domains, incoherent initial state and a status that stays OFF past
 * the 10 ms bound are refused.
 *
 * The domain owner (domains 1, 4, 5 and 10) with the actual PMIC provider,
 * rail ledger and clock registry: supply verification once per boot, row
 * requests that read no PMIC register, one DSB SY per transition, refusal on
 * a mismatched supply or an incoherent domain before any write, bounded
 * acknowledgement waits, retention with fault bits, and domain-5 power-off
 * only with the GPU gate and resets closed.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units
 * plus drivers/power/a733_de_sys_power.c and a733_power_domains.c. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_de_sys_power.h"
#include "a733_power_authority_domains.h"

/* --- Part 1: per-consumer display provider ------------------------------- */
static struct { uintptr_t address; uint32_t value; } trace[16];
static unsigned trace_count,status_delay,authorize_busy;
static uint32_t command,status,delays[5];
static uintptr_t base;
static uint64_t now_us,step_us;

static int authorize(void *v){(void)v;if(authorize_busy){--authorize_busy;return 0;}return 1;}
static int now(void *v,uint64_t *out){(void)v;now_us+=step_us;*out=now_us;return 1;}
static int enter(void *v,uint64_t d){(void)v;assert(d>now_us);return 1;}
static void leave(void *v){(void)v;}
static uint32_t *legacy_word(uintptr_t a)
{
    static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
    assert(a>=base && a<base+0x1000);
    if(a==base)return &command;
    if(a==base+8)return &status;
    for(unsigned i=0;i<5;++i)if(a==base+offsets[i])return &delays[i];
    assert(0);return NULL;
}
static int legacy_read(void *v,uintptr_t a,uint32_t *out)
{
    (void)v;
    if(a==base+8 && (command&15)==8 && (status&15)==0 && !status_delay--)status=(status&~15u)|8;
    *out=*legacy_word(a);return 1;
}
static int legacy_write(void *v,uintptr_t a,uint32_t value)
{
    (void)v;assert(a!=base+8 && trace_count<16);
    trace[trace_count].address=a;trace[trace_count++].value=value;*legacy_word(a)=value;return 1;
}
static const struct a733_de_sys_power_ops ops={authorize,now,enter,leave,legacy_read,legacy_write};

static void reset(unsigned domain,uint32_t initial_command,uint32_t initial_status)
{
    base=UINT32_C(0x07060000)+domain*UINT32_C(0x1000);
    trace_count=0;status_delay=0;authorize_busy=0;now_us=100;step_us=1;
    command=initial_command;status=initial_status;
    for(unsigned i=0;i<5;++i)delays[i]=0;
}
static int run(struct a733_de_sys_power *p)
{
    int r=0;
    for(unsigned i=0;i<100000 && !r;++i)r=a733_de_sys_power_poll(p);
    return r;
}
static void legacy(void)
{
    _Static_assert(A733_DE_SYS_PCK_BASE==UINT32_C(0x07061000),"DE_SYS block");
    _Static_assert(A733_PCK_DOMAIN_BASE(10)==UINT32_C(0x0706a000),"VO1 block");
    static const unsigned domains[]={1,10};
    for(unsigned i=0;i<2;++i){
        /* OFF: delay row, then ON8 with bit 8 of the command word preserved. */
        struct a733_de_sys_power p={0};
        reset(domains[i],0x100,0);status_delay=3;authorize_busy=2;
        assert(a733_de_sys_power_init_domain(&p,&ops,NULL,1000000,domains[i]) && p.base==base);
        assert(run(&p)==1 && a733_de_sys_power_ready(&p) && !p.inherited_on && p.write_attempted);
        const uint32_t expected[][2]={{0x170,0x1f1f1f},{0x174,0x1f1f},{0xc00,0x08080808},
            {0xc04,0x0808},{0xc10,8},{0,0x108}};
        assert(trace_count==6);
        for(unsigned j=0;j<6;++j)
            assert(trace[j].address==base+expected[j][0] && trace[j].value==expected[j][1]);
        /* ON: adopted without any write. */
        struct a733_de_sys_power q={0};
        reset(domains[i],8,8);
        assert(a733_de_sys_power_init_domain(&q,&ops,NULL,1000000,domains[i]));
        assert(run(&q)==1 && a733_de_sys_power_ready(&q) && q.inherited_on && !trace_count);
        /* Incoherent initial command/status is refused before any write. */
        struct a733_de_sys_power r={0};
        reset(domains[i],8,0);status_delay=UINT32_MAX;
        assert(a733_de_sys_power_init_domain(&r,&ops,NULL,1000000,domains[i]));
        assert(run(&r)==-1 && !trace_count && !a733_de_sys_power_ready(&r));
        /* Status still OFF after 10 ms fails without a second request. */
        struct a733_de_sys_power s={0};
        reset(domains[i],0,0);status_delay=UINT32_MAX;step_us=50;
        assert(a733_de_sys_power_init_domain(&s,&ops,NULL,1000000,domains[i]));
        assert(run(&s)==-1 && trace_count==6 && now_us<20000);
        assert(a733_de_sys_power_poll(&s)==-1 && trace_count==6);
    }
    /* Only domains 1 and 10 are accepted; the old initializer is DE_SYS. */
    for(unsigned d=0;d<16;++d)if(d!=1 && d!=10){
        struct a733_de_sys_power p={0};
        assert(!a733_de_sys_power_init_domain(&p,&ops,NULL,1000000,d) && !p.self);
    }
    struct a733_de_sys_power p={0};
    assert(a733_de_sys_power_init(&p,&ops,NULL,1000000) && p.base==A733_DE_SYS_PCK_BASE);
}

/* --- Part 2: the domain owner --------------------------------------------- */
#define CCU UINT32_C(0x02002000)
#define PCK(d) (UINT32_C(0x07060000)+UINT32_C(0x1000)*(d))
static struct pmic_model pm;
static struct a733_power_domains owner;
struct reg { uintptr_t address; uint32_t value; };
static struct {
    struct reg regs[64];unsigned count;
    unsigned ack[11];        /* Status reads after a command before status follows it. */
    unsigned pending[11];
    uint64_t ticks;
    unsigned reads,writes,barriers;
    struct reg log[64];unsigned logged; /* Writes; address 0 marks a barrier. */
} rm;
static uint32_t *reg(uintptr_t a)
{
    for(unsigned i=0;i<rm.count;++i)if(rm.regs[i].address==a)return &rm.regs[i].value;
    assert(rm.count<64);rm.regs[rm.count].address=a;rm.regs[rm.count].value=0;return &rm.regs[rm.count++].value;
}
static int dom_now(void *v,uint64_t *out){(void)v;*out=pm.now;return 1;}
static uint64_t dom_ticks(void *v){(void)v;return rm.ticks++;}
static int dom_read(void *v,uintptr_t a,uint32_t *out)
{
    (void)v;++rm.reads;
    for(unsigned d=1;d<=10;++d)if(a==PCK(d)+8 && rm.pending[d] && rm.ack[d]!=UINT32_MAX && !--rm.pending[d])
        *reg(a)=(*reg(a)&~15u)|(*reg(PCK(d))&15u);
    *out=*reg(a);return 1;
}
static int dom_write(void *v,uintptr_t a,uint32_t value)
{
    (void)v;++rm.writes;assert(rm.logged<64);rm.log[rm.logged++]=(struct reg){a,value};
    *reg(a)=value;
    for(unsigned d=1;d<=10;++d)if(a==PCK(d)) {
        rm.pending[d]=rm.ack[d];
        if(!rm.ack[d])*reg(PCK(d)+8)=(*reg(PCK(d)+8)&~15u)|(value&15u);
    }
    return 1;
}
static void dom_barrier(void *v){(void)v;++rm.barriers;assert(rm.logged<64);rm.log[rm.logged++]=(struct reg){0,0};}
static const struct a733_power_domain_ops domain_ops={dom_now,dom_ticks,dom_read,dom_write,dom_barrier};
static unsigned stops(void)
{unsigned n=0;for(unsigned i=0;i<pm.trace_count;++i)n+=pm.trace[i].address==0x0708300c && pm.trace[i].value==0x58;return n;}
static unsigned authority(void){return atomic_load(&pm.rails.authority.word);}
/* Boot state: every domain OFF with its gates and the PCK clock on, supplies
 * at their expected settings, provider READY. */
static void boot(void)
{
    pmic_model_setup(&pm);
    pm.pmic[0x30]=13;pm.pmic[0x33]=13; /* CLDO2 and CLDO5 at 1,800 mV. */
    for(unsigned i=0;i<64 && a733_pmic_live(&pm.provider,&pm.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&pm.provider,&pm.lifetime)==A733_PMIC_READY);
    memset(&rm,0,sizeof(rm));
    *reg(0x070101ac)=1;*reg(CCU+0x5c0)=0xff;*reg(CCU+0x5e0)=UINT32_MAX;
    memset(&owner,0,sizeof(owner));
    assert(a733_power_domains_init(&owner,&domain_ops,&rm,&pm.rails,1));
}
/* Supply verification driven as the PMIC service task would drive it. */
static void verify(void)
{
    const uint64_t after=pm.now+A733_POWER_SUPPLY_SETTLE_US,deadline=pm.now+100000;
    struct a733_rail_token none={0};struct a733_power_row early={0};
    assert(a733_power_domains_verify(&owner,&pm.provider,&pm.lifetime,700,after,deadline)==A733_DOMAIN_PENDING);
    assert(!stops() && a733_power_domain_request(&owner,1,&none,&early)==A733_DOMAIN_PENDING);
    pm.now=after;
    enum a733_power_domain_result r=A733_DOMAIN_PENDING;
    for(unsigned i=0;i<400 && r==A733_DOMAIN_PENDING;++i) {
        r=a733_power_domains_verify(&owner,&pm.provider,&pm.lifetime,700,after,deadline);
        if(r==A733_DOMAIN_PENDING)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
    }
    assert(r==A733_DOMAIN_READY && (authority()&A733_POWER_SUPPLY_DONE));
}
static struct a733_rail_token token(uint64_t holder,a733_rail_mask_t rails)
{
    struct a733_rail_token t;
    assert(a733_rail_acquire(&pm.rails,holder,rails,0,pm.now+1000000,&t)==A733_RAIL_PENDING);
    return t;
}
#define D2 A733_RAIL_BIT(A733_RAIL_DCDC2)
#define D4 A733_RAIL_BIT(A733_RAIL_DCDC4)
#define C2 A733_RAIL_BIT(A733_RAIL_CLDO2)
#define C5 A733_RAIL_BIT(A733_RAIL_CLDO5)
static void owner_cases(void)
{
    /* Verification: four snapshots after the ramp, then no PMIC read again. */
    boot();
    assert(!a733_power_npu_supply_uv(&pm.rails));
    verify();
    const unsigned verified=A733_POWER_DCDC2_VERIFIED|A733_POWER_DCDC4_VERIFIED|
        A733_POWER_CLDO2_VERIFIED|A733_POWER_CLDO5_VERIFIED;
    assert((authority()&verified)==verified);
    assert(a733_power_npu_supply_uv(&pm.rails)==800000);
    const unsigned transactions=stops();
    printf("Supply verification: %u PMIC transactions, once per boot\n",transactions);
    assert(transactions==24);
    /* GPU row: delay row, ON command, one DSB SY, READY in one call. */
    struct a733_rail_token gpu=token(10,D4);struct a733_power_row g={0};
    assert(a733_power_domain_check(&owner,5)==A733_DOMAIN_READY && !rm.writes);
    rm.ack[5]=10;
    assert(a733_power_domain_request(&owner,5,&gpu,&g)==A733_DOMAIN_READY);
    assert(stops()==transactions && rm.barriers==1 && rm.logged==7);
    const uint32_t row[][2]={{0x170,0x1f1f1f},{0x174,0x1f1f},{0xc00,0x08080808},{0xc04,0x0808},{0xc10,8},{0,8}};
    for(unsigned i=0;i<6;++i)assert(rm.log[i].address==PCK(5)+row[i][0] && rm.log[i].value==row[i][1]);
    assert(!rm.log[6].address);
    /* The token is pinned while the row is counted. */
    assert(a733_rail_release(&pm.rails,&gpu,pm.now+1000)==A733_RAIL_BUSY);
    /* A second domain-1 row after the first: no command, no barrier, no PMIC read. */
    struct a733_rail_token fb=token(11,D2),g2d=token(12,D2);struct a733_power_row f={0},h={0};
    assert(a733_power_domain_request(&owner,1,&fb,&f)==A733_DOMAIN_READY && rm.barriers==2);
    unsigned writes=rm.writes;
    assert(a733_power_domain_request(&owner,1,&g2d,&h)==A733_DOMAIN_READY);
    assert(rm.writes==writes && rm.barriers==2 && stops()==transactions);
    /* Domain 1 stays on when its rows drop. */
    assert(a733_power_domain_release(&owner,&f)==A733_DOMAIN_READY && !f.id);
    assert(a733_power_domain_release(&owner,&h)==A733_DOMAIN_READY && rm.writes==writes);
    assert(a733_rail_release(&pm.rails,&fb,pm.now+1000)==A733_RAIL_READY);
    /* Domain-5 power-off: refused with the GPU clock gate or a reset open,
     * then one step. */
    *reg(CCU+0x0b20)=1u<<31;
    assert(a733_power_domain_release(&owner,&g)==A733_DOMAIN_REFUSED && g.id && rm.writes==writes);
    *reg(CCU+0x0b20)=0;*reg(CCU+0x0b24)=1;
    assert(a733_power_domain_release(&owner,&g)==A733_DOMAIN_REFUSED && g.id);
    *reg(CCU+0x0b24)=0;rm.ack[5]=0;
    assert(a733_power_domain_release(&owner,&g)==A733_DOMAIN_READY && !g.id && rm.barriers==3);
    assert(rm.log[rm.logged-2].address==PCK(5) && rm.log[rm.logged-2].value==0);
    assert(a733_rail_release(&pm.rails,&gpu,pm.now+1000)==A733_RAIL_READY);

    /* A mismatched DCDC4 refuses the GPU row without any write. */
    boot();pm.pmic[0x15]=31;verify();
    assert(!(authority()&A733_POWER_DCDC4_VERIFIED) && (authority()&A733_POWER_DCDC2_VERIFIED));
    gpu=token(10,D4);g=(struct a733_power_row){0};
    assert(a733_power_domain_request(&owner,5,&gpu,&g)==A733_DOMAIN_REFUSED && !rm.writes && !g.id);
    /* A token without the domain's supply is refused. */
    assert(a733_power_domain_request(&owner,1,&gpu,&g)==A733_DOMAIN_INVALID);

    /* Incoherent domains 4 and 1 are refused before any consumer reset or
     * clock write, and the owner writes nothing. */
    for(unsigned i=0;i<2;++i) {
        const unsigned d=i ? 1 : 4;
        boot();verify();*reg(PCK(d))=8;
        assert(a733_power_domain_check(&owner,d)==A733_DOMAIN_RETAINED && !rm.writes);
        assert(authority()&(d==4 ? A733_POWER_DOMAIN4_FAULT : A733_POWER_DOMAIN1_FAULT));
        struct a733_rail_token t=token(20,D2);struct a733_power_row r={0};
        assert(a733_power_domain_request(&owner,d,&t,&r)==A733_DOMAIN_RETAINED && !rm.writes);
    }
    /* A missing MBUS gate or PCK clock refuses the row before any write. */
    boot();verify();*reg(CCU+0x5e0)&=~(1u<<18);
    struct a733_rail_token npu=token(21,D2);struct a733_power_row n={0};
    assert(a733_power_domain_check(&owner,4)==A733_DOMAIN_REFUSED);
    assert(a733_power_domain_request(&owner,4,&npu,&n)==A733_DOMAIN_REFUSED && !rm.writes && !n.id);
    *reg(CCU+0x5e0)=UINT32_MAX;*reg(0x070101ac)=0;
    assert(a733_power_domain_request(&owner,4,&npu,&n)==A733_DOMAIN_REFUSED && !rm.writes);
    /* A live GPU found ON at adoption is FAILED. */
    boot();verify();*reg(PCK(5))=8;*reg(PCK(5)+8)=8;*reg(CCU+0x0b20)=1u<<31;
    assert(a733_power_domain_check(&owner,5)==A733_DOMAIN_RETAINED && (authority()&A733_POWER_DOMAIN5_FAULT));
    /* An inherited ON domain with closed gates is adopted with no write. */
    boot();verify();*reg(PCK(5))=8;*reg(PCK(5)+8)=8;
    gpu=token(10,D4);g=(struct a733_power_row){0};
    assert(a733_power_domain_request(&owner,5,&gpu,&g)==A733_DOMAIN_READY && !rm.writes && !rm.barriers);

    /* A slow acknowledgement: AGAIN after the spin bound, READY next tick. */
    boot();verify();rm.ack[4]=70;
    npu=token(21,D2);n=(struct a733_power_row){0};
    assert(a733_power_domain_request(&owner,4,&npu,&n)==A733_DOMAIN_AGAIN && n.id);
    pm.now+=1000;
    assert(a733_power_domain_request(&owner,4,0,&n)==A733_DOMAIN_READY && rm.barriers==1);
    /* The owner lock taken by another core: BUSY, then progress. */
    struct a733_rail_token npu2=token(22,D2);struct a733_power_row n2={0};
    atomic_store(&owner.busy,1);
    assert(a733_power_domain_request(&owner,4,&npu2,&n2)==A733_DOMAIN_BUSY && !n2.id);
    atomic_store(&owner.busy,0);
    assert(a733_power_domain_request(&owner,4,&npu2,&n2)==A733_DOMAIN_READY);

    /* A DisplayPort row on a domain 10 that never acknowledges: one AGAIN per
     * tick, FAILED within 10 ms plus one tick; the fault bit then refuses an
     * HDMI access. A second requester observes FAILED. */
    boot();verify();rm.ack[10]=UINT32_MAX;
    struct a733_rail_token dp=token(30,D2|C5),hdmi=token(31,D2|C2);
    struct a733_power_row p={0},q={0};
    const uint64_t start=pm.now;unsigned again=0;
    enum a733_power_domain_result r=a733_power_domain_request(&owner,10,&dp,&p);
    assert(a733_power_authority_admits(&pm.rails.authority,A733_POWER_HDMI));
    assert(a733_power_domain_request(&owner,10,&hdmi,&q)==A733_DOMAIN_AGAIN && q.id);
    while(r==A733_DOMAIN_AGAIN){++again;pm.now+=1000;r=a733_power_domain_request(&owner,10,0,&p);}
    assert(r==A733_DOMAIN_RETAINED && pm.now-start<=A733_POWER_DOMAIN_WAIT_US+1000 && again<=11);
    assert(!a733_power_authority_admits(&pm.rails.authority,A733_POWER_HDMI));
    assert(a733_power_domain_request(&owner,10,0,&q)==A733_DOMAIN_RETAINED);
    assert(a733_power_domain_release(&owner,&q)==A733_DOMAIN_RETAINED);
    assert(a733_rail_release(&pm.rails,&dp,pm.now+1000)==A733_RAIL_BUSY && rm.barriers==1);
    printf("Unacknowledged domain 10: FAILED after %llu us and %u retries\n",
        (unsigned long long)(pm.now-start),again);
    /* A domain-10 token needs CLDO2 or CLDO5; the held one must be verified. */
    boot();pm.pmic[0x33]=12;verify();
    dp=token(30,D2|C5);hdmi=token(31,D2|C2);struct a733_rail_token bare=token(32,D2);
    p=q=(struct a733_power_row){0};
    assert(a733_power_domain_request(&owner,10,&bare,&p)==A733_DOMAIN_INVALID);
    assert(a733_power_domain_request(&owner,10,&dp,&p)==A733_DOMAIN_REFUSED && !rm.writes);
    assert(a733_power_domain_request(&owner,10,&hdmi,&q)==A733_DOMAIN_READY);
    /* A provider fault before verification finishes refuses rows. */
    boot();
    a733_power_authority_set(&pm.rails.authority,A733_POWER_PROVIDER_FAULT);
    gpu=token(10,D4);g=(struct a733_power_row){0};
    assert(a733_power_domain_request(&owner,5,&gpu,&g)==A733_DOMAIN_RETAINED && !rm.writes);
}
int main(void)
{
    legacy();owner_cases();
    puts("PASS PCK domains: display provider ON sequence, adoption and refusals; owner supply "
        "verification once per boot, rows without PMIC reads, one DSB SY per transition, supply and "
        "coherence refusals before writes, bounded acknowledgement, retention with fault bits and "
        "domain-5 power-off only with the GPU closed");
    return 0;
}
