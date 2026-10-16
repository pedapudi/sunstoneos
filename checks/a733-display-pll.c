#include "a733_display_pll.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void pll_reference(const uint32_t *,uint32_t,uint32_t *,uint32_t *,uint32_t *);
struct fixture {
    struct a733_clock_leases registry;
    struct a733_display_pll_config config;
    struct a733_display_pll pll;
    uint32_t phy[128],host[256],lcd[256],top[64],ccu[0x2000/4];
    uint32_t enabled[128],multiplied[128],initial[128];
    uint64_t now,last_lock,enabled_time,multiplier_time;
    unsigned held,retained,leaves,reads,writes,auths,locks,barriers;
    unsigned fail_auth,uncertain_write,corrupt_write,late_write,lose_lock,enter_busy;
    unsigned final_time_fault,post_leave_observations;
};
static int now(void *ctx,uint64_t *n)
{
    struct fixture *f=ctx;*n=f->now;
    if(f->final_time_fault && f->leaves && ++f->post_leave_observations==2) {
        *n=f->config.deadline;atomic_store(&f->registry.busy,1);
    }
    return 1;
}
static int enter(void *ctx,uint64_t deadline)
{struct fixture *f=ctx;assert(f->now<deadline);if(f->enter_busy)return 0;assert(!f->held);f->held=1;return 1;}
static void leave(void *ctx)
{struct fixture *f=ctx;assert(f->held);f->held=0;++f->leaves;}
static void retain(void *ctx)
{struct fixture *f=ctx;assert(f->held);++f->retained;}
static int authorize(void *ctx,const struct a733_display_pll_config *c)
{
    struct fixture *f=ctx;assert(f->held);++f->auths;
    assert(c->reservation.handoff==101 && c->reservation.inactive_epoch==102);
    assert(c->reservation.reference_generation==103 && c->reservation.rail_generation==104);
    assert(c->initial.id==201 && c->initial.generation==202);
    assert(!memcmp(&c->reservation.token,&f->config.reservation.token,sizeof(c->reservation.token)));
    return f->fail_auth?0:1;
}
static uint32_t *word(struct fixture *f,uintptr_t a)
{
    if(a>=0x05509000 && a<0x05509200)return &f->phy[(a-0x05509000)/4];
    if(a>=0x05508000 && a<0x05508400)return &f->host[(a-0x05508000)/4];
    if(a>=0x05502000 && a<0x05502400)return &f->lcd[(a-0x05502000)/4];
    if(a>=0x05500000 && a<0x05500100)return &f->top[(a-0x05500000)/4];
    assert(a>=0x02002000 && a<0x02004000);return &f->ccu[(a-0x02002000)/4];
}
static int read32(void *ctx,uintptr_t a,uint32_t *v)
{
    struct fixture *f=ctx;assert(f->held && !f->retained);++f->reads;*v=*word(f,a);
    if(a==0x055090e0) {
        if(f->last_lock)assert(f->now-f->last_lock>=5);
        f->last_lock=f->now;++f->locks;
        if(f->lose_lock && f->locks==2)*v=0;
    }
    return 1;
}
static int write32(void *ctx,uintptr_t a,uint32_t v)
{
    struct fixture *f=ctx;assert(f->held && !f->retained);
    assert(a==0x05509104 || a==0x05509108);
    ++f->writes;*word(f,a)=v;
    if(f->writes==7){memcpy(f->enabled,f->phy,sizeof(f->phy));f->enabled_time=f->now;}
    if(f->writes==8){assert(f->locks>=3);assert(f->now-f->last_lock>=25);
        memcpy(f->multiplied,f->phy,sizeof(f->phy));f->multiplier_time=f->now;}
    if(f->writes==9)assert(f->now-f->multiplier_time>=20);
    if(f->corrupt_write)*word(f,a)^=0x80;
    if(f->late_write)f->now=f->config.deadline;
    return f->uncertain_write?0:1;
}
static int barrier(void *ctx)
{struct fixture *f=ctx;assert(f->held && !f->retained);++f->barriers;return 1;}
static void setup(struct fixture *f,unsigned enabled,uint32_t hs,uint32_t ls,uint32_t target_hs,uint32_t target_ls)
{
    memset(f,0,sizeof(*f));f->now=1;
    assert(a733_clock_leases_init(&f->registry,now,f));
    struct a733_clock_token token;
    assert(a733_clock_acquire(&f->registry,77,A733_CLOCK_PERI0|A733_CLOCK_FABRIC,
        A733_CLOCK_VO0|A733_CLOCK_DISPLL1,10000,&token)==A733_CLOCK_OK);
    struct a733_dsi_pll_plan plan;assert(a733_dsi_pll_plan(hs,ls,&plan));
    f->config=(struct a733_display_pll_config){
        .ops={now,enter,leave,retain,authorize,read32,write32,barrier},
        .context=f,.registry=&f->registry,
        .reservation={token,101,102,103,104},
        .initial={.id=201,.generation=202,.hs_hz=hs,.ls_hz=ls,
            .words={plan.factors|0x00800080,0x00120381,0x01372818,0x80000000,
                    0x1f123456,0x01004000,0x172}},
        .hs_hz=target_hs,.ls_hz=target_ls,.deadline=10000};
    if(enabled){f->config.initial.words[0]|=0x80700000;f->config.initial.words[1]|=0x601000;}
    static const unsigned offsets[]={0x104,0x108,0x10c,0x124,0x130,0x134,0x138};
    for(unsigned i=0;i<7;++i)f->phy[offsets[i]/4]=f->config.initial.words[i];
    f->phy[0xe0/4]=0x04000000;
    f->ccu[0x1588/4]=0x82000000;f->ccu[0x158c/4]=0x10001;f->ccu[0x150c/4]=0x10001;
    f->top[0xc/4]=0x20;
    memcpy(f->initial,f->phy,sizeof(f->phy));
    assert(a733_display_pll_init(&f->pll,&f->config));
}
static void tick(struct fixture *f)
{a733_display_pll_poll(&f->pll);++f->now;}
static void finish(struct fixture *f,enum a733_display_pll_state state)
{
    for(unsigned i=0;f->pll.state==A733_DISPLAY_PLL_PENDING && i<10000;++i)tick(f);
    assert(f->pll.state==state);
    unsigned reads=f->reads,writes=f->writes,auth=f->auths;
    for(unsigned i=0;i<3;++i)tick(f);
    assert(reads==f->reads && writes==f->writes && auth==f->auths);
}
static void restore(struct fixture *f,const struct fixture *saved)
{
    memcpy(f,saved,sizeof(*f));f->registry.context=f;
    f->config.context=f;f->config.registry=&f->registry;
    f->pll.config.context=f;f->pll.config.registry=&f->registry;
    f->pll.self=&f->pll; /* Model checkpoint rebinding, never a runtime copy. */
}
static void fault_cases(const struct fixture *saved)
{
    struct fixture f;
    restore(&f,saved);f.uncertain_write=1;tick(&f);
    assert(f.pll.error==A733_DISPLAY_PLL_IO && f.writes==1 && f.retained==1 && f.registry.failed);
    finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);
    restore(&f,saved);f.corrupt_write=1;finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);
    assert(f.pll.error==A733_DISPLAY_PLL_VERIFY && f.writes==1);
    restore(&f,saved);f.late_write=1;tick(&f);
    assert(f.pll.error==A733_DISPLAY_PLL_TIME && f.writes==1 && f.retained==1);
    restore(&f,saved);atomic_store(&f.registry.busy,1);tick(&f);
    assert(!f.writes && f.pll.state==A733_DISPLAY_PLL_PENDING);
    atomic_store(&f.registry.busy,0);f.fail_auth=1;
    /* Saved checkpoint is immediately before the first write. The prior
     * successful authentication remains valid under the held mutex; failure
     * at the next authentication must retain the already attempted write. */
    finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);
    assert(f.pll.error==A733_DISPLAY_PLL_AUTHORITY && f.writes==1);
}
static void final_retention(void)
{
    struct fixture f;setup(&f,0,480000000,120000000,600000000,100000000);
    f.final_time_fault=1;
    for(unsigned i=0;!f.pll.fault && i<10000;++i)tick(&f);
    assert(f.pll.fault && f.pll.error==A733_DISPLAY_PLL_TIME);
    assert(f.pll.state==A733_DISPLAY_PLL_PENDING && !f.registry.failed);
    assert(f.post_leave_observations==2 && f.leaves==1 && !f.held && !f.retained);
    unsigned reads=f.reads,writes=f.writes,auths=f.auths,barriers=f.barriers;
    /* While registry metadata is busy, ownership remains reserved and the
     * failed operation cannot publish readiness or invoke another callback. */
    tick(&f);assert(f.pll.state==A733_DISPLAY_PLL_PENDING);
    atomic_store(&f.registry.busy,0);tick(&f);
    assert(f.pll.state==A733_DISPLAY_PLL_FAULT_RETAINED && f.registry.failed);
    assert(f.reads==reads && f.writes==writes && f.auths==auths && f.barriers==barriers);
    assert(f.leaves==1 && f.post_leave_observations==2 && !f.retained);
    finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);
    puts("display PLL: final time failure waits for registry retention without publishing readiness");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"final-retain")){final_retention();return 0;}
    assert(argc==1);
    struct fixture f,saved;
    static const uint32_t rates[][4]={{480000000,120000000,600000000,100000000},
        {840000000,140000000,480000000,80000000}};
    for(unsigned i=0;i<2;++i){
        setup(&f,i,rates[i][0],rates[i][1],rates[i][2],rates[i][3]);
        if(!i){
            f.enter_busy=1;tick(&f);assert(!f.held && !f.reads);f.enter_busy=0;
            while(f.pll.writes_attempted==0 && f.pll.substep!=2)tick(&f);
            assert(!f.writes && f.held);memcpy(&saved,&f,sizeof(saved));
        }
        f.lose_lock=!i;finish(&f,A733_DISPLAY_PLL_READY_RETAINED);
        assert(f.writes==14 && f.leaves==1 && !f.held && !f.retained);
        assert(f.locks==(i?6u:8u));
        assert(a733_clock_token_status(&f.registry,&f.config.reservation.token)==A733_CLOCK_OK);
        uint32_t enabled[128],multiplied[128],final[128];
        pll_reference(f.initial,f.pll.plan.target.factors,enabled,multiplied,final);
        assert(!memcmp(enabled,f.enabled,sizeof(enabled)));
        assert(!memcmp(multiplied,f.multiplied,sizeof(multiplied)));
        assert(!memcmp(final,f.phy,sizeof(final)));
        struct a733_clock_token other={0};
        assert(a733_clock_acquire(&f.registry,88,A733_CLOCK_DISPLL1,0,10000,&other)==A733_CLOCK_BUSY);
    }
    fault_cases(&saved);
    setup(&f,0,480000000,120000000,600000000,100000000);
    f.phy[0x104/4]^=0x800000;finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);assert(!f.writes);
    setup(&f,0,480000000,120000000,600000000,100000000);
    f.fail_auth=1;finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);assert(!f.reads && !f.writes);
    setup(&f,0,480000000,120000000,600000000,100000000);
    assert(a733_clock_release(&f.registry,&f.config.reservation.token,10000)==A733_CLOCK_OK);
    finish(&f,A733_DISPLAY_PLL_FAULT_RETAINED);assert(!f.reads && !f.writes);
    restore(&f,&saved);f.now=f.config.deadline;tick(&f);
    assert(f.pll.error==A733_DISPLAY_PLL_TIME && !f.writes && f.retained);
    setup(&f,0,480000000,120000000,600000000,100000000);
    struct a733_display_pll_plan plan,unchanged;memset(&plan,0x5a,sizeof(plan));unchanged=plan;
    struct a733_display_pll_profile profile=f.config.initial;
    profile.id=0;assert(!a733_display_pll_plan(&profile,600000000,100000000,&plan));
    assert(!memcmp(&plan,&unchanged,sizeof(plan)));
    profile=f.config.initial;profile.words[2]|=0x80000000;
    assert(!a733_display_pll_plan(&profile,600000000,100000000,&plan));
    profile=f.config.initial;profile.words[0]|=0x100000;
    assert(!a733_display_pll_plan(&profile,600000000,100000000,&plan));
    /* Both endpoints are legal. Source M2-before-M3 temporarily yields
     * LS=1.8GHz, so this transition must be refused before any hardware access. */
    profile=f.config.initial;profile.ls_hz=480000000;
    struct a733_dsi_pll_plan initial;assert(a733_dsi_pll_plan(profile.hs_hz,profile.ls_hz,&initial));
    profile.words[0]=(profile.words[0]&~0x3f0fff3f)|initial.factors;
    assert(a733_dsi_pll_plan(600000000,360000000,&initial));
    assert(!a733_display_pll_plan(&profile,600000000,360000000,&plan));
    puts("display PLL: source checkpoints, rate transitions, real registry exclusion and retained faults passed");
}
