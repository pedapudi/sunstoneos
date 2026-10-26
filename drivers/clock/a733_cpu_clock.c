/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_cpu_clock.h"
#include "a733_clock_reference.h"
#include "a733_range.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define CPUPLL UINT32_C(0x08870000)
#define OSC UINT32_C(0x07090160)
#define SID_DVFS UINT32_C(0x0300624c)
#define PLL_ON UINT32_C(0xf8000000) /* Enable, LDO, lock enable, lock, output. */
#define UPDATE BIT(26)
#define LOCK BIT(28)
#define MUX (UINT32_C(7)<<24)
#define DIVP (UINT32_C(3)<<16)
#define FIELD_N (UINT32_C(255)<<8)
#define MUX_PLL 3u
#define UPDATE_US 10000u  /* Pinned driver: update bit clears within 10 ms. */
#define LOCK_US 70000u    /* Pinned driver: lock within 70 ms. */
#define UNPARK_US 10000u  /* Margin for the owner polls that observe lock and reselect the PLL. */
#define PAT0 0x4u         /* Pattern words and LFM word, offsets from the PLL word. */
#define PAT1 0x8u
#define LFM 0x18u
#define PAT0_TYPE (BIT(29)|BIT(30)) /* Bits the pinned probe sets: triangular pattern, */
#define PAT1_LFM BIT(31)            /* lfm_en and the LFM ramp enable. */
#define LFM_ON BIT(31)
#define RAMP_UV_US 250u   /* Board DTS regulator-ramp-delay of DCDC3/DCDC5. */
/* The vendor regulator driver waits |dV| / ramp rounded up to whole
 * milliseconds (axp2101-regulator.c axp_set_voltage_time, linux-orangepi
 * 2ac08e8c lines 383-409). */
#define RAMP_ROUND_US 1000u
/* boot0 waits 20 us after parking a CPU mux on the oscillator and after
 * selecting the PLL again (boot0_sdcard_orangepizero3w.fex@0x2114, the CPU
 * PLL routine of docs/reference-backsolve-cpu.md). */
#define SETTLE_US 20u
/* Lock counts once three reads 3 us apart all show it, then 20 us pass
 * before the PLL drives a clock: the vendor CCU helper with
 * CONFIG_AW_STANDARD_CCU=y, which the A733 board kernel configuration sets
 * (ccu_common.c ccu_helper_wait_for_lock, lines 47-64), and boot0's three
 * lock reads and 20 us wait. */
#define LOCK_READS 3u
#define LOCK_GAP_US 3u
#define LOCK_SETTLE_US 20u
#define SETTLE_READS 100000u /* A counter that stops is a fault. */
#define BURST_POLLS 100000u  /* Polls per burst; a counter that stops is a fault. */
#define RELEASE_US 100000u
#define THERMAL_US 500000u
enum { USER, LOWER, RESTORE }; /* Who started the transition. */
enum { S_IDLE,S_CLOCK,S_RAIL,S_INSPECT,S_READ,S_VOLTAGE,S_WRITTEN,S_SETTLE,S_PARK,S_UPDATE,
    S_LOCK,S_UNPARK,S_RELEASE };

/* opp-microvolt-26m-vf* rows of sun60iw2p1-cpu-vf.dtsi V0.9, in millivolts. */
static const struct a733_cpu_opp little[]={
    {416,{800,800,800,800,800,800,800,800,800}},
    {728,{800,0,0,0,0,0,0,0,0}},
    {780,{0,800,800,800,800,800,800,800,800}},
    {1014,{800,800,800,800,800,800,800,800,800}},
    {1092,{0,800,0,0,0,0,0,0,0}},
    {1196,{800,850,800,800,800,800,800,800,800}},
    {1300,{0,900,800,0,0,0,0,0,0}},
    {1404,{800,950,850,860,840,800,840,800,800}},
    {1508,{0,1000,900,0,880,820,0,820,820}},
    {1586,{900,0,0,0,0,0,0,0,0}},
    {1612,{0,1050,950,0,920,860,0,860,840}},
    {1716,{0,0,1000,0,960,900,0,900,860}},
    {1794,{1000,0,1050,0,1000,950,0,950,900}},
};
static const struct a733_cpu_opp big[]={
    {416,{800,800,800,800,800,800,800,800,800}},
    {728,{800,0,0,0,0,0,0,0,0}},
    {780,{0,800,800,800,800,800,800,800,800}},
    {1014,{800,800,800,800,800,800,800,800,800}},
    {1196,{800,800,800,800,800,800,800,800,800}},
    {1300,{0,800,0,0,0,0,0,0,0}},
    {1404,{800,850,800,820,800,800,800,800,800}},
    {1508,{800,900,800,860,820,800,800,800,800}},
    {1586,{850,0,0,0,0,0,0,0,0}},
    {1612,{0,950,850,900,840,820,850,820,820}},
    {1690,{900,0,0,0,0,0,0,0,0}},
    {1716,{0,1000,900,0,870,850,0,850,840}},
    {1794,{950,1050,950,0,920,900,0,900,870}},
    {1898,{1000,0,1000,0,970,940,0,940,900}},
    {1950,{0,0,0,0,1000,970,0,970,930}},
    {1976,{1050,0,0,0,0,0,0,0,0}},
    {2002,{0,0,1050,0,1050,1000,0,1000,960}},
};
static const struct a733_cpu_opp dsu[]={
    {312,{800,800,800,800,800,800,800,800,800}},
    {546,{800,0,0,0,0,0,0,0,0}},
    {598,{0,800,800,800,800,800,800,800,800}},
    {780,{800,800,800,800,800,800,800,800,800}},
    {832,{0,800,0,0,0,0,0,0,0}},
    {910,{800,850,800,800,800,800,800,800,800}},
    {988,{0,900,800,0,0,0,0,0,0}},
    {1066,{800,950,850,860,840,800,840,800,800}},
    {1144,{0,1000,900,0,880,820,0,820,820}},
    {1196,{900,0,0,0,0,0,0,0,0}},
    {1222,{0,1050,950,0,920,860,0,860,840}},
    {1248,{0,0,1000,0,960,900,0,900,860}},
    {1274,{0,0,1050,0,1000,950,0,950,900}},
    {1352,{1000,0,0,0,0,0,0,0,0}},
};
const struct a733_cpu_opp *a733_cpu_clock_opps(enum a733_cpu_domain d,unsigned *count)
{
    static const struct { const struct a733_cpu_opp *rows; unsigned count; } tables[]={
        {little,sizeof(little)/sizeof(*little)},{big,sizeof(big)/sizeof(*big)},{dsu,sizeof(dsu)/sizeof(*dsu)}};
    if(!count || (unsigned)d>=A733_CPU_DOMAINS)return 0;
    *count=tables[d].count;return tables[d].rows;
}
uint32_t a733_cpu_clock_opp_mv(const struct a733_cpu_opp *o,unsigned bin)
{
    if(!o || bin>A733_CPU_VF_ENVELOPE)return 0;
    if(bin<A733_CPU_VF_BINS)return o->mv[bin];
    uint32_t mv=0;
    for(unsigned i=0;i<A733_CPU_VF_BINS;++i){if(!o->mv[i])return 0;if(o->mv[i]>mv)mv=o->mv[i];}
    return mv;
}
uint32_t a733_cpu_clock_dsu_top(unsigned bin,uint32_t little_mhz,uint32_t big_mhz)
{
    uint32_t ceiling=0,top=0;
    for(unsigned i=0;i<sizeof(little)/sizeof(*little);++i)
        if(little[i].mhz==little_mhz)ceiling=a733_cpu_clock_opp_mv(&little[i],bin);
    uint64_t cap=(uint64_t)(little_mhz>big_mhz ? little_mhz : big_mhz)*1000000u*3u/4u;
    cap-=cap%26000000u;
    for(unsigned i=0;ceiling && i<sizeof(dsu)/sizeof(*dsu);++i) {
        uint32_t mv=a733_cpu_clock_opp_mv(&dsu[i],bin);
        if(mv && mv<=ceiling && (uint64_t)dsu[i].mhz*1000000u<=cap && dsu[i].mhz>top)top=dsu[i].mhz;
    }
    return top;
}
int a733_cpu_clock_bin(uint32_t word,unsigned *bin)
{
    /* vf_mapping_table code -> column; 0x11 and 0x12 have no column. */
    static const uint8_t codes[]={0x01,0x02,0x03,0x04,0x05,0x06,0x14,0x15};
    unsigned code=(word>>16)&255;if(!code)code=(word>>8)&255;
    if(!bin)return 0;
    if(!code){*bin=A733_CPU_VF_ENVELOPE;return 1;}
    for(unsigned i=0;i<sizeof(codes);++i)if(codes[i]==code){*bin=i+1;return 1;}
    return 0;
}
static uintptr_t pll_word(unsigned d){return CPUPLL+UINT32_C(0x1000)*(d+1);}
/* Index of the supply in rail_uv: DCDC5 feeds CPU-L and the DSU, DCDC3 CPU-B. */
static unsigned supply(unsigned d){return d==A733_CPU_BIG;}
static uintptr_t select_word(unsigned d){return pll_word(d)+0x1c;}
void a733_cpu_clock_decode(uint32_t dcxo,uint32_t pll,uint32_t select,struct a733_cpu_clock_domain *out)
{
    if(!out)return;
    *out=(struct a733_cpu_clock_domain){.pll=pll,.select=select,.n=(pll>>8)&255,
        .p=(select>>16)&3,.source=(select>>24)&7};
    if(out->source<=2){out->hz=dcxo;out->decoded=1;}
    else if(out->source==MUX_PLL && out->n && (pll&PLL_ON)==PLL_ON) {
        out->hz=((uint64_t)dcxo*out->n)>>out->p;out->decoded=1;
    }
}
int a733_cpu_clock_report(int (*read)(void *,uintptr_t,uint32_t *),void *cookie,struct a733_cpu_clock_report *out)
{
    uint32_t words[2*A733_CPU_DOMAINS];
    if(!read || !out)return 0;
    *out=(struct a733_cpu_clock_report){0};
    if(read(cookie,OSC,&out->oscillator)!=1 || read(cookie,SID_DVFS,&out->sid_word)!=1)return 0;
    for(unsigned d=0;d<A733_CPU_DOMAINS;++d)
        if(read(cookie,pll_word(d),&words[2*d])!=1 || read(cookie,select_word(d),&words[2*d+1])!=1)return 0;
    out->dcxo_hz=a733_clock_dcxo_hz(out->oscillator);
    out->binned=a733_cpu_clock_bin(out->sid_word,&out->bin);
    for(unsigned d=0;d<A733_CPU_DOMAINS;++d)
        a733_cpu_clock_decode(out->dcxo_hz,words[2*d],words[2*d+1],&out->domain[d]);
    return 1;
}
int a733_cpu_clock_address(uintptr_t a,int write)
{
    for(unsigned d=0;d<A733_CPU_DOMAINS;++d) {
        if(a==pll_word(d) || a==select_word(d))return 1;
        if(!write && (a==pll_word(d)+PAT0 || a==pll_word(d)+PAT1 || a==pll_word(d)+LFM))return 1;
    }
    return !write && (a==OSC || a==SID_DVFS);
}
/* Core identity as the secondary-core coherence check requires it: MPIDR
 * with only RES1, U, MT and Aff1 set, and the core type that Aff1 implies. */
static int core_of(const struct a733_cpu_cycle_sample *s,unsigned *core)
{
    *core=(unsigned)(s->mpidr>>8)&7u;
    return !(s->mpidr&~UINT64_C(0xc1000700)) &&
        (s->midr&UINT64_C(0xffffffffff0ffff0))==(*core<6 ? UINT64_C(0x410fd050) : UINT64_C(0x410fd0b0));
}
int a733_cpu_clock_timing(const struct a733_cpu_cycle_sample *b,const struct a733_cpu_cycle_sample *e,
    uint32_t hz,const struct a733_cpu_clock_report *report,struct a733_cpu_timing *out)
{
    unsigned core;
    if(!b || !e || !out || !hz || !core_of(b,&core) || b->mpidr!=e->mpidr || b->midr!=e->midr ||
       e->ticks<=b->ticks || e->cycles<b->cycles || e->ticks-b->ticks>UINT64_MAX/hz)return 0;
    uint64_t ticks=e->ticks-b->ticks,cycles=e->cycles-b->cycles,whole=cycles/ticks;
    if(whole>UINT64_MAX/hz)return 0;
    /* cycles x hz / ticks without a 128-bit division. */
    struct a733_cpu_timing t={.core=core,.cluster=core<6 ? A733_CPU_LITTLE : A733_CPU_BIG,.cycles=cycles,
        .ticks=ticks,.measured_hz=whole*hz+cycles%ticks*hz/ticks};
    if(report && report->domain[t.cluster].decoded)t.configured_hz=report->domain[t.cluster].hz;
    *out=t;return 1;
}

static const struct a733_cpu_opp *row(unsigned d,uint32_t mhz)
{
    unsigned count;const struct a733_cpu_opp *t=a733_cpu_clock_opps(d,&count);
    for(unsigned i=0;t && i<count;++i)if(t[i].mhz==mhz)return &t[i];
    return 0;
}
int a733_cpu_clock_grant_load(const struct a733_cpu_clock_grant_record *r,uint64_t epoch,
    uint64_t generation,struct a733_cpu_clock_config *out)
{
    unsigned approved=0;
    if(!r || !out || !epoch || !generation || r->magic!=A733_CPU_CLOCK_GRANT_MAGIC ||
       r->version!=A733_CPU_CLOCK_GRANT_VERSION || r->bytes!=sizeof(*r) || !r->owner ||
       (r->clusters&~UINT32_C(3)))return 0;
    for(unsigned i=0;i<sizeof(r->approval);++i)approved|=r->approval[i];
    if(!r->clusters) {
        if(r->epoch || r->generation || r->max_mhz[0] || r->max_mhz[1] || r->changes || approved)return 0;
        *out=(struct a733_cpu_clock_config){.owner=r->owner};return 1;
    }
    if(r->epoch!=epoch || r->generation!=generation || !r->changes ||
       r->changes>A733_CPU_CLOCK_GRANT_CHANGES || !approved)return 0;
    for(unsigned d=0;d<2;++d)
        if((r->clusters&BIT(d)) ? !row(d,r->max_mhz[d]) : r->max_mhz[d]!=0)return 0;
    *out=(struct a733_cpu_clock_config){.owner=r->owner,.grant={.session=epoch,.clusters=r->clusters,
        .max_mhz={r->max_mhz[0],r->max_mhz[1]},.changes=r->changes}};
    return 1;
}
int a733_cpu_clock_init(struct a733_cpu_clock *c,const struct a733_cpu_clock_config *config,
    const struct a733_cpu_clock_ops *ops,void *cookie,struct a733_clock_leases *clocks,
    struct a733_rail_leases *rails,struct a733_pmic *pmic,const struct a733_thermal_policy *policy)
{
    if(!c || c->self || !config || !ops || !ops->now || !ops->read || !ops->write || !config->owner ||
       !clocks || !clocks->initialized || !rails || rails->self!=rails || rails->binding.clocks!=clocks ||
       !pmic || pmic->self!=pmic || pmic->rails!=rails || (config->grant.clusters&~UINT32_C(3)) ||
       !policy || !policy->initialized ||
       !a733_ranges_disjoint(c,sizeof(*c),config,sizeof(*config)) || !a733_ranges_disjoint(c,sizeof(*c),ops,sizeof(*ops)) ||
       !a733_ranges_disjoint(c,sizeof(*c),clocks,sizeof(*clocks)) || !a733_ranges_disjoint(c,sizeof(*c),rails,sizeof(*rails)) ||
       !a733_ranges_disjoint(c,sizeof(*c),pmic,sizeof(*pmic)) ||
       !a733_ranges_disjoint(c,sizeof(*c),policy,sizeof(*policy)))return 0;
    *c=(struct a733_cpu_clock){.self=c,.config=*config,.ops=*ops,.cookie=cookie,.clocks=clocks,
        .rails=rails,.pmic=pmic,.policy=policy,.changes=config->grant.changes};
    return 1;
}
/* The policy's level, with CRITICAL kept once seen. */
static unsigned level(struct a733_cpu_clock *c)
{
    unsigned now=a733_thermal_policy_level(c->policy);
    if(now==A733_THERMAL_CRITICAL)c->critical=1;
    return c->critical ? A733_THERMAL_CRITICAL : now;
}
/* A throttle lowering needs only a level below CRITICAL. Any other
 * transition needs a valid reading from the domain's sensor, read before the
 * level, and a level that admits the point. The DSU has no sensor of its own
 * and uses CPUL, beside it on DCDC5. */
static int permitted(struct a733_cpu_clock *c,unsigned cluster,uint32_t mhz,unsigned thermal)
{
    static const unsigned sensor[3]={3,0,3}; /* Physical channels CPUL, CPUB and CPUL. */
    unsigned seen=a733_thermal_policy_seen(c->policy),now=level(c);
    if(now==A733_THERMAL_CRITICAL)return 0;
    if(thermal==LOWER)return 1;
    return (seen&BIT(sensor[cluster])) && (now==A733_THERMAL_NORMAL || (thermal==USER &&
        mhz<=(cluster==A733_CPU_DSU ? A733_CPU_CLOCK_DSU_THROTTLE_MHZ : A733_CPU_CLOCK_THROTTLE_MHZ)));
}
/* Lowest DSU point at or above the inherited DSU rate that the bin supports. */
static uint32_t dsu_mv(uint64_t hz,unsigned bin)
{
    for(unsigned i=0;i<sizeof(dsu)/sizeof(*dsu);++i) {
        uint32_t mv=a733_cpu_clock_opp_mv(&dsu[i],bin);
        if(mv && (uint64_t)dsu[i].mhz*1000000u>=hz)return mv;
    }
    return 0;
}
/* A supported inherited operating point, including the shared DSU voltage.
 * Read-only inspection may report other rates, but cannot adopt them for
 * automatic thermal transitions. Rechecked under leases before each change. */
static uint32_t inherited_mv(const struct a733_cpu_clock *c)
{
    const struct a733_cpu_clock_report *r=&c->report;
    const struct a733_cpu_clock_domain *d=&r->domain[c->cluster];
    const uint32_t *words=c->words[c->cluster];
    unsigned nmax=c->cluster==A733_CPU_LITTLE ? 84u : 86u;
    if((words[0]&PAT0_TYPE) || (words[1]&PAT1_LFM) || (words[2]&LFM_ON) ||
       (r->oscillator&UINT32_C(0x10000002))!=UINT32_C(0x10000002) || r->dcxo_hz!=26000000u ||
       !r->binned || !d->decoded || (d->pll&(PLL_ON|UPDATE))!=PLL_ON ||
       d->source!=MUX_PLL || d->n<26u || d->n>nmax || d->hz%1000000u)return 0;
    uint32_t mv=a733_cpu_clock_opp_mv(row(c->cluster,(uint32_t)(d->hz/1000000u)),r->bin);
    if(!mv)return 0;
    if(c->cluster==A733_CPU_LITTLE){
        const struct a733_cpu_clock_domain *ds=&r->domain[A733_CPU_DSU];
        uint32_t shared=ds->decoded ? dsu_mv(ds->hz,r->bin) : 0;
        if(!shared)return 0;
        if(shared>mv)mv=shared;
    }
    return mv;
}
/* A CPU-L grant also admits the DSU, which shares DCDC5; inspection bounds
 * the DSU point by the CPU-L maximum's voltage. */
static int granted(const struct a733_cpu_clock *c,unsigned cluster,uint32_t mhz)
{
    const struct a733_cpu_clock_grant *g=&c->config.grant;
    if(cluster==A733_CPU_DSU)return g->session && (g->clusters&BIT(A733_CPU_LITTLE)) && c->changes;
    return g->session && (g->clusters&BIT(cluster)) && c->changes && mhz<=g->max_mhz[cluster];
}
static int time_now(struct a733_cpu_clock *c)
{
    uint64_t now;
    return c->ops.now(c->cookie,&now)==1 && a733_monotonic_advance(now,&c->last_now);
}
static enum a733_rail rail_of(unsigned cluster){return cluster==A733_CPU_BIG ? A733_RAIL_DCDC3 : A733_RAIL_DCDC5;}
/* Uncertain state: keep both leases. Retention is metadata and is retried
 * while either manager reports BUSY. The PMIC provider retains its own. */
static enum a733_cpu_clock_status fault(struct a733_cpu_clock *c)
{
    c->failed=1;
    if(c->rail.id && !(c->retained&1) && a733_rail_retain(c->rails,&c->rail)!=A733_RAIL_BUSY)c->retained|=1;
    if(c->clock.id && !(c->retained&2) && a733_clock_retain(c->clocks,&c->clock)!=A733_CLOCK_BUSY)c->retained|=2;
    return A733_CPU_CLOCK_RETAINED;
}
/* End a transition before or after its writes by releasing both leases. */
static enum a733_cpu_clock_status finish(struct a733_cpu_clock *c,enum a733_cpu_clock_status result)
{
    c->result=result;c->state=S_RELEASE;c->timer=c->last_now+RELEASE_US;return A733_CPU_CLOCK_PENDING;
}
static int rd(struct a733_cpu_clock *c,uintptr_t a,uint32_t *v)
{return a733_cpu_clock_address(a,0) && c->ops.read(c->cookie,a,v)==1;}
static int put(struct a733_cpu_clock *c,uintptr_t a,uint32_t v)
{return a733_cpu_clock_address(a,1) && c->ops.write(c->cookie,a,v)==1;}
static int wr(struct a733_cpu_clock *c,uintptr_t a,uint32_t v)
{uint32_t back;return put(c,a,v) && rd(c,a,&back) && back==v;}
/* Spin on the time source for at least us microseconds. The time source
 * counts whole microseconds, so the first reading may already be up to 1 us
 * past its value: waiting for us + 1 more guarantees us. */
static int settle(struct a733_cpu_clock *c,uint32_t us)
{
    if(!time_now(c))return 0;
    const uint64_t until=c->last_now+us+1u;
    for(unsigned i=0;i<SETTLE_READS;++i){
        if(!time_now(c))return 0;
        if(c->last_now>=until)return 1;
    }
    return 0;
}
static enum a733_cpu_clock_status start(struct a733_cpu_clock *c,unsigned cluster,uint32_t mhz,
    uint64_t deadline,unsigned thermal)
{
    c->cluster=cluster;c->target_mhz=mhz;c->deadline=deadline;c->thermal=thermal;
    c->clock=(struct a733_clock_token){0};c->rail=(struct a733_rail_token){0};
    c->request=(struct a733_pmic_request){0};c->after=S_IDLE;c->state=S_CLOCK;c->inspect_only=0;c->spend=0;
    return A733_CPU_CLOCK_PENDING;
}
enum a733_cpu_clock_status a733_cpu_clock_request(struct a733_cpu_clock *c,
    enum a733_cpu_domain cluster,uint32_t mhz,uint64_t deadline)
{
    if(!c || c->self!=c || (unsigned)cluster>=A733_CPU_DOMAINS)return A733_CPU_CLOCK_INVALID;
    if(c->failed)return A733_CPU_CLOCK_RETAINED;
    if(c->state!=S_IDLE)return A733_CPU_CLOCK_BUSY;
    if(!granted(c,cluster,mhz) ||
       !row(cluster,mhz) || !permitted(c,cluster,mhz,USER))return A733_CPU_CLOCK_REFUSED;
    if(!time_now(c))return fault(c);
    if(c->last_now>=deadline)return A733_CPU_CLOCK_EXPIRED;
    /* The owner's choice replaces any pending restore of this cluster. */
    --c->changes;c->throttled&=~BIT(cluster);c->throttle_failed&=~BIT(cluster);
    return start(c,cluster,mhz,deadline,USER);
}
enum a733_cpu_clock_status a733_cpu_clock_inspect(struct a733_cpu_clock *c,enum a733_cpu_domain cluster,
    uint64_t deadline)
{
    if(!c || c->self!=c || (unsigned)cluster>=A733_CPU_DOMAINS)return A733_CPU_CLOCK_INVALID;
    if(c->failed)return A733_CPU_CLOCK_RETAINED;
    if(c->state!=S_IDLE)return A733_CPU_CLOCK_BUSY;
    if(!time_now(c))return fault(c);
    if(c->last_now>=deadline)return A733_CPU_CLOCK_EXPIRED;
    enum a733_cpu_clock_status r=start(c,cluster,0,deadline,USER);
    c->inspect_only=1;return r;
}
/* Inherited state and the target point, checked before any write. */
static enum a733_cpu_clock_status inspect(struct a733_cpu_clock *c)
{
    struct a733_cpu_clock_report *r=&c->report;
    uint32_t pat0,pat1,lfm;
    if(!a733_cpu_clock_report(c->ops.read,c->cookie,r) || !rd(c,pll_word(c->cluster)+PAT0,&pat0) ||
       !rd(c,pll_word(c->cluster)+PAT1,&pat1) || !rd(c,pll_word(c->cluster)+LFM,&lfm))return fault(c);
    c->words[c->cluster][0]=pat0;c->words[c->cluster][1]=pat1;c->words[c->cluster][2]=lfm;
    if(c->inspect_only) {
        enum a733_pmic_status s=a733_pmic_submit(c->pmic,c->config.owner,&c->rail,rail_of(c->cluster),c->deadline,&c->request);
        if(s==A733_PMIC_BUSY)return A733_CPU_CLOCK_PENDING;
        if(s==A733_PMIC_EXPIRED)return finish(c,A733_CPU_CLOCK_EXPIRED);
        if(s!=A733_PMIC_PENDING || !c->request.id)return fault(c);
        c->state=S_READ;return A733_CPU_CLOCK_PENDING;
    }
    const struct a733_cpu_clock_domain *d=&r->domain[c->cluster];
    const struct a733_cpu_opp *o=row(c->cluster,c->target_mhz);
    const unsigned nmax=c->cluster==A733_CPU_LITTLE ? 84u : 86u; /* 26 MHz VCO range. */
    uint32_t mv=a733_cpu_clock_opp_mv(o,r->bin);
    if(c->cluster==A733_CPU_LITTLE) {
        const struct a733_cpu_clock_domain *s=&r->domain[A733_CPU_DSU];
        uint32_t shared=s->decoded ? dsu_mv(s->hz,r->bin) : 0;
        if(!shared)return finish(c,A733_CPU_CLOCK_REFUSED);
        if(shared>mv)mv=shared;
    }
    if(c->cluster==A733_CPU_DSU && mv) {
        /* The vendor dsufreq rule: at most three quarters of the faster
         * cluster, rounded down to the 26 MHz step, and no point whose
         * voltage exceeds that of CPU-L's granted maximum, because the DSU
         * writes no voltage of its own (sunxi-dsufreq.c lines 178-195,
         * 197-228 and 284-295). The present DCDC5 voltage is checked after
         * the snapshot. */
        const struct a733_cpu_clock_domain *l=&r->domain[A733_CPU_LITTLE],*b=&r->domain[A733_CPU_BIG];
        uint64_t fast=l->hz>b->hz ? l->hz : b->hz,cap=fast*3u/4u;
        cap-=cap%26000000u;
        uint32_t ceiling=a733_cpu_clock_opp_mv(row(A733_CPU_LITTLE,c->config.grant.max_mhz[A733_CPU_LITTLE]),r->bin);
        if(!l->decoded || !b->decoded || (uint64_t)c->target_mhz*1000000u>cap || mv>ceiling)
            return finish(c,A733_CPU_CLOCK_REFUSED);
    }
    c->p=c->target_mhz<676u ? 1u : 0u;
    c->n=(c->target_mhz<<c->p)/26u;
    /* With a pattern or frequency modulation enabled, N alone may not define
     * the rate and lock may not mean the final rate. */
    if((pat0&PAT0_TYPE) || (pat1&PAT1_LFM) || (lfm&LFM_ON) ||
       (r->oscillator&UINT32_C(0x10000002))!=UINT32_C(0x10000002) || r->dcxo_hz!=26000000u ||
       !r->binned || !o || !a733_cpu_clock_opp_mv(o,r->bin) || !permitted(c,c->cluster,c->target_mhz,c->thermal) ||
       (d->pll&(PLL_ON|UPDATE))!=PLL_ON || d->source!=MUX_PLL || d->n<26u || d->n>nmax ||
       c->n*26u!=c->target_mhz<<c->p || c->n<26u || c->n>nmax)return finish(c,A733_CPU_CLOCK_REFUSED);
    if(c->thermal!=USER && (c->inherited&BIT(c->cluster)) &&
       (!inherited_mv(c) || d->hz!=(uint64_t)c->applied_mhz[c->cluster]*1000000u ||
        (c->thermal==LOWER && d->hz<=(uint64_t)c->target_mhz*1000000u)))
        return finish(c,A733_CPU_CLOCK_REFUSED);
    c->to_uv=mv*1000u;c->raise=d->hz<(uint64_t)c->target_mhz*1000000u;c->select=d->select;
    if(d->hz==(uint64_t)c->target_mhz*1000000u){c->applied_mhz[c->cluster]=c->target_mhz;return finish(c,A733_CPU_CLOCK_COMPLETE);}
    enum a733_pmic_status s=a733_pmic_submit(c->pmic,c->config.owner,&c->rail,rail_of(c->cluster),c->deadline,&c->request);
    if(s==A733_PMIC_BUSY)return A733_CPU_CLOCK_PENDING;
    if(s==A733_PMIC_EXPIRED)return finish(c,A733_CPU_CLOCK_EXPIRED);
    if(s!=A733_PMIC_PENDING || !c->request.id)return fault(c);
    c->state=S_READ;return A733_CPU_CLOCK_PENDING;
}
/* Wait for the current PMIC request. 1: result copied and reaped. 2: the
 * request expired before bus admission, so it had no bus effects; reaped.
 * With floor, a completed read of an enabled, decoded selector first records
 * the rail's boot baseline in the ledger and returns it in *floor, before the
 * request is reaped: a busy ledger leaves the request outstanding, so the
 * next poll repeats both steps. */
static int collect(struct a733_cpu_clock *c,uint32_t *floor,enum a733_cpu_clock_status *out)
{
    enum a733_pmic_status s=a733_pmic_result(c->pmic,&c->request,&c->snapshot);
    if(s==A733_PMIC_PENDING || s==A733_PMIC_BUSY){*out=A733_CPU_CLOCK_PENDING;return 0;}
    if(s!=A733_PMIC_COMPLETE && s!=A733_PMIC_EXPIRED){*out=fault(c);return 0;}
    if(s==A733_PMIC_COMPLETE && c->snapshot.rail!=rail_of(c->cluster)){*out=fault(c);return 0;}
    if(floor && s==A733_PMIC_COMPLETE && c->snapshot.enabled && c->snapshot.decoded) {
        enum a733_rail_result f=a733_rail_floor(c->rails,&c->rail,rail_of(c->cluster),
            c->snapshot.programmed_uv,floor);
        if(f==A733_RAIL_BUSY){*out=A733_CPU_CLOCK_PENDING;return 0;}
        if(f!=A733_RAIL_READY){*out=fault(c);return 0;}
    }
    enum a733_pmic_status reaped=a733_pmic_reap(c->pmic,&c->request);
    if(reaped==A733_PMIC_BUSY){*out=A733_CPU_CLOCK_PENDING;return 0;}
    if(reaped!=A733_PMIC_COMPLETE){*out=fault(c);return 0;}
    c->request=(struct a733_pmic_request){0};
    return s==A733_PMIC_EXPIRED ? 2 : 1;
}
static enum a733_cpu_clock_status voltage(struct a733_cpu_clock *c)
{
    struct a733_pmic_snapshot target;
    uint8_t code=(uint8_t)((c->to_uv/1000u-500u)/10u);
    if(!a733_pmic_decode(rail_of(c->cluster),0,code,&target) || target.programmed_uv!=c->to_uv)return fault(c);
    enum a733_pmic_status s=a733_pmic_submit_write(c->pmic,c->config.owner,&c->rail,
        rail_of(c->cluster),(uint8_t)(c->selector&127u),code,c->deadline,&c->request);
    if(s==A733_PMIC_BUSY)return A733_CPU_CLOCK_PENDING;
    if(s!=A733_PMIC_PENDING || !c->request.id)return fault(c);
    c->state=S_WRITTEN;return A733_CPU_CLOCK_PENDING;
}
static enum a733_cpu_clock_status written(struct a733_cpu_clock *c)
{
    enum a733_cpu_clock_status out;
    int k=collect(c,0,&out);
    if(!k)return out;
    /* Expired before the bus: the voltage is unchanged. A raise has not yet
     * touched the PLL; a lowering keeps its lower rate at the higher voltage. */
    if(k==2) {
        if(c->after!=S_PARK)c->applied_mhz[c->cluster]=c->target_mhz;
        return finish(c,A733_CPU_CLOCK_EXPIRED);
    }
    if(!c->snapshot.enabled || !c->snapshot.decoded || c->snapshot.programmed_uv!=c->to_uv)return fault(c);
    c->rail_uv[supply(c->cluster)]=c->to_uv;
    uint32_t delta=c->to_uv>c->from_uv ? c->to_uv-c->from_uv : c->from_uv-c->to_uv;
    uint32_t ramp=(delta+RAMP_UV_US-1)/RAMP_UV_US;
    c->timer=c->last_now+(ramp+RAMP_ROUND_US-1)/RAMP_ROUND_US*RAMP_ROUND_US;c->state=S_SETTLE;
    return A733_CPU_CLOCK_PENDING;
}
static enum a733_cpu_clock_status release(struct a733_cpu_clock *c)
{
    if(c->last_now>=c->timer)return fault(c);
    if(c->rail.id) {
        enum a733_rail_result r=a733_rail_release(c->rails,&c->rail,c->timer);
        if(r==A733_RAIL_BUSY || r==A733_RAIL_PENDING)return A733_CPU_CLOCK_PENDING;
        if(r!=A733_RAIL_READY)return fault(c);
        c->rail=(struct a733_rail_token){0};
    }
    if(c->clock.id) {
        enum a733_clock_result r=a733_clock_release(c->clocks,&c->clock,c->timer);
        if(r==A733_CLOCK_BUSY)return A733_CPU_CLOCK_PENDING;
        if(r!=A733_CLOCK_OK)return fault(c);
        c->clock=(struct a733_clock_token){0};
    }
    /* A completed throttle lowering is restored at NORMAL; one that did not
     * complete is not retried in this THROTTLE episode. */
    if(c->thermal==LOWER && c->result==A733_CPU_CLOCK_COMPLETE)c->throttled|=BIT(c->cluster);
    else if(c->thermal==LOWER)c->throttle_failed|=BIT(c->cluster);
    if(c->thermal==USER && !c->inspect_only && c->result==A733_CPU_CLOCK_COMPLETE)
        c->inherited&=~BIT(c->cluster);
    c->state=S_IDLE;return c->result;
}
enum a733_cpu_clock_status a733_cpu_clock_poll(struct a733_cpu_clock *c)
{
    if(!c || c->self!=c)return A733_CPU_CLOCK_INVALID;
    if(c->failed)return fault(c);
    if(!time_now(c))return fault(c);
    if(c->state==S_IDLE) {
        unsigned now=level(c);
        for(unsigned i=0;i<2;++i) {
            if(now==A733_THERMAL_THROTTLE && c->applied_mhz[i]>A733_CPU_CLOCK_THROTTLE_MHZ &&
               !(c->throttle_failed&BIT(i))) {
                /* An inherited point's lowering consumes its change at the
                 * first clock write (S_PARK), not when it starts. */
                if((c->inherited&BIT(i)) && !granted(c,i,A733_CPU_CLOCK_THROTTLE_MHZ))continue;
                c->restore_mhz[i]=c->applied_mhz[i];
                enum a733_cpu_clock_status r=start(c,i,A733_CPU_CLOCK_THROTTLE_MHZ,c->last_now+THERMAL_US,LOWER);
                c->spend=!!(c->inherited&BIT(i));
                return r;
            }
            if(now!=A733_THERMAL_NORMAL)continue;
            /* A new THROTTLE episode may try a failed lowering again. A restore
             * is attempted once and consumes a change, as a request does. */
            c->throttle_failed&=~BIT(i);
            if(!(c->throttled&BIT(i)))continue;
            c->throttled&=~BIT(i);
            if(granted(c,i,c->restore_mhz[i]) && c->restore_mhz[i]>c->applied_mhz[i]) {
                --c->changes;return start(c,i,c->restore_mhz[i],c->last_now+THERMAL_US,RESTORE);
            }
        }
        return A733_CPU_CLOCK_READY;
    }
    if(c->state==S_RELEASE)return release(c);
    /* The PMIC provider bounds its requests with the same deadline. Once the
     * PLL has been touched, expiry is a fault. Before that, any confirmed
     * voltage covers the present rate, so expiry ends the transition. A
     * lowering whose voltage write completed finishes its ramp wait. */
    if(c->last_now>=c->deadline && c->state!=S_READ && c->state!=S_WRITTEN &&
       (c->state!=S_SETTLE || c->after==S_PARK)) {
        if(c->state==S_UPDATE || c->state==S_LOCK || c->state==S_UNPARK)return fault(c);
        if(c->state==S_VOLTAGE && c->after==S_RELEASE)c->applied_mhz[c->cluster]=c->target_mhz;
        return finish(c,A733_CPU_CLOCK_EXPIRED);
    }
    switch(c->state) {
    case S_CLOCK: {
        /* CPU-L holds the DSU rate that its voltage covers; the DSU holds
         * both cluster rates that bound it. */
        static const a733_clock_mask_t own[3]={A733_CLOCK_CPU_L,A733_CLOCK_CPU_B,A733_CLOCK_CPU_DSU};
        static const a733_clock_mask_t shared[3]={A733_CLOCK_CPU_DSU,0,A733_CLOCK_CPU_L|A733_CLOCK_CPU_B};
        enum a733_clock_result r=a733_clock_acquire(c->clocks,c->config.owner,
            shared[c->cluster],own[c->cluster],c->deadline,&c->clock);
        if(r==A733_CLOCK_BUSY)return A733_CPU_CLOCK_PENDING;
        if(r==A733_CLOCK_EXPIRED)return finish(c,A733_CPU_CLOCK_EXPIRED);
        if(r!=A733_CLOCK_OK)return fault(c);
        c->state=S_RAIL;return A733_CPU_CLOCK_PENDING;
    }
    case S_RAIL: {
        enum a733_rail_result r;
        if(!c->rail.id) {
            r=a733_rail_acquire(c->rails,c->config.owner,0,A733_RAIL_BIT(rail_of(c->cluster)),c->deadline,&c->rail);
            if(r==A733_RAIL_BUSY)return A733_CPU_CLOCK_PENDING;
            if(r==A733_RAIL_EXPIRED)return finish(c,A733_CPU_CLOCK_EXPIRED);
            if(r!=A733_RAIL_PENDING)return fault(c);
        }
        r=a733_rail_poll(c->rails,&c->rail);
        if(r==A733_RAIL_BUSY || r==A733_RAIL_PENDING)return A733_CPU_CLOCK_PENDING;
        if(r!=A733_RAIL_READY)return fault(c);
        c->state=S_INSPECT;return A733_CPU_CLOCK_PENDING;
    }
    case S_INSPECT:return inspect(c);
    case S_READ: {
        /* The rail's first reading is its boot baseline, kept by the ledger:
         * the target never falls below it, whatever the operating point. */
        enum a733_cpu_clock_status out;
        uint32_t floor=0;
        int k=collect(c,&floor,&out);
        if(!k)return out;
        if(k==2)return finish(c,A733_CPU_CLOCK_EXPIRED);
        if(!c->snapshot.enabled || !c->snapshot.decoded)return finish(c,A733_CPU_CLOCK_REFUSED);
        c->rail_uv[supply(c->cluster)]=c->snapshot.programmed_uv;
        if(c->inspect_only){
            uint32_t mv=inherited_mv(c);
            c->applied_mhz[c->cluster]=mv && c->rail_uv[supply(c->cluster)]>=mv*1000u ?
                (uint32_t)(c->report.domain[c->cluster].hz/1000000u) : 0;
            c->inherited|=BIT(c->cluster);
            c->snapshot=(struct a733_pmic_snapshot){0};return finish(c,A733_CPU_CLOCK_COMPLETE);
        }
        c->from_uv=c->snapshot.programmed_uv;c->selector=c->snapshot.selector;
        c->snapshot=(struct a733_pmic_snapshot){0};
        if(c->to_uv<floor)c->to_uv=floor;
        /* The DSU changes only its clock: the present DCDC5 voltage must
         * already cover the target point. */
        if(c->cluster==A733_CPU_DSU){
            if(c->to_uv>c->from_uv)return finish(c,A733_CPU_CLOCK_REFUSED);
            c->to_uv=c->from_uv;
        }
        /* Inherited thermal control has clock-only authority. Preserve the
         * observed voltage even on restoration; insufficient source or target
         * voltage refuses before either PMIC or clock writes. */
        if(c->thermal!=USER && (c->inherited&BIT(c->cluster))){
            uint32_t mv=inherited_mv(c);
            if(!mv || c->from_uv<mv*1000u || c->from_uv<c->to_uv)
                return finish(c,A733_CPU_CLOCK_REFUSED);
            c->to_uv=c->from_uv;
        }
        /* Raise: voltage first. Lower: frequency first, then voltage unless
         * the thermal path keeps it. Enough voltage needs no write. */
        if(c->raise && c->to_uv>c->from_uv){c->state=S_VOLTAGE;c->after=S_PARK;}
        else {c->state=S_PARK;c->after=!c->raise && c->thermal!=LOWER && c->to_uv<c->from_uv ? S_VOLTAGE : S_RELEASE;}
        return A733_CPU_CLOCK_PENDING;
    }
    case S_VOLTAGE:return voltage(c);
    case S_WRITTEN:return written(c);
    case S_SETTLE:
        if(c->last_now<c->timer)return A733_CPU_CLOCK_PENDING;
        c->snapshot=(struct a733_pmic_snapshot){0};
        if(c->after==S_PARK){c->state=S_PARK;c->after=S_RELEASE;return A733_CPU_CLOCK_PENDING;}
        c->applied_mhz[c->cluster]=c->target_mhz;return finish(c,A733_CPU_CLOCK_COMPLETE);
    case S_PARK: {
        /* PMIC completion and ramp waits can span later thermal samples.
         * Recheck before the first clock write; a completed upward ramp is
         * safe for the unchanged rate and needs no voltage rollback. Once
         * parked, finish the existing bounded update/unpark sequence. */
        if(!permitted(c,c->cluster,c->target_mhz,c->thermal))return finish(c,A733_CPU_CLOCK_REFUSED);
        /* Park on the DCXO, then write N and request the update. A deadline
         * that cannot cover both PLL waits ends the transition first: nothing
         * is touched yet, and any confirmed voltage covers the present rate. */
        uint32_t pll,select;
        if(c->deadline-c->last_now<UPDATE_US+LOCK_US+UNPARK_US)return finish(c,A733_CPU_CLOCK_EXPIRED);
        /* No other transition ran since start, so the grant still covers it. */
        c->changes-=c->spend;c->spend=0;
        if(!rd(c,pll_word(c->cluster),&pll) || (pll&(PLL_ON|UPDATE))!=PLL_ON ||
           !rd(c,select_word(c->cluster),&select) || select!=c->select ||
           !wr(c,select_word(c->cluster),c->select&~MUX))return fault(c);
        c->park_at=c->last_now;c->locks=0;
        if(!settle(c,SETTLE_US))return fault(c);
        pll=(pll&~FIELD_N)|(c->n<<8);
        if(!put(c,pll_word(c->cluster),pll) || !put(c,pll_word(c->cluster),pll|UPDATE))return fault(c);
        c->timer=c->last_now+UPDATE_US;c->state=S_UPDATE;return A733_CPU_CLOCK_PENDING;
    }
    case S_UPDATE:
    case S_LOCK: {
        uint32_t pll;
        if(!rd(c,pll_word(c->cluster),&pll) || ((pll>>8)&255)!=c->n)return fault(c);
        if(c->state==S_UPDATE ? (pll&UPDATE)!=0 : (pll&LOCK)==0) {
            c->locks=0;
            return c->last_now<c->timer ? A733_CPU_CLOCK_PENDING : fault(c);
        }
        if(c->state==S_UPDATE){c->timer=c->last_now+LOCK_US;c->state=S_LOCK;return A733_CPU_CLOCK_PENDING;}
        /* A lock read sooner than LOCK_GAP_US after the last counted one
         * does not count. Readings are whole microseconds, so one more
         * guarantees the gap, as in settle(). */
        if(!c->locks || c->last_now>=c->lock_at+LOCK_GAP_US+1u){c->lock_at=c->last_now;++c->locks;}
        if(c->locks<LOCK_READS)return c->last_now<c->timer ? A733_CPU_CLOCK_PENDING : fault(c);
        if(!settle(c,LOCK_SETTLE_US))return fault(c);
        c->state=S_UNPARK;return A733_CPU_CLOCK_PENDING;
    }
    case S_UNPARK: {
        const uint32_t parked=(c->select&~(MUX|DIVP))|(c->p<<16);
        uint32_t pll;struct a733_cpu_clock_domain d;
        if(!wr(c,select_word(c->cluster),parked) || !wr(c,select_word(c->cluster),parked|(MUX_PLL<<24)) ||
           !time_now(c))return fault(c);
        const uint64_t parked_us=c->last_now-c->park_at;
        if(parked_us>c->parked_max_us)c->parked_max_us=parked_us;
        if(!settle(c,SETTLE_US) || !rd(c,pll_word(c->cluster),&pll))return fault(c);
        a733_cpu_clock_decode(c->report.dcxo_hz,pll,parked|(MUX_PLL<<24),&d);
        if(!d.decoded || d.hz!=(uint64_t)c->target_mhz*1000000u)return fault(c);
        if(c->after==S_VOLTAGE){c->state=S_VOLTAGE;c->after=S_RELEASE;return A733_CPU_CLOCK_PENDING;}
        c->applied_mhz[c->cluster]=c->target_mhz;return finish(c,A733_CPU_CLOCK_COMPLETE);
    }
    default:return fault(c);
    }
}
int a733_cpu_clock_parked(const struct a733_cpu_clock *c)
{return c && c->self==c && (c->state==S_UPDATE || c->state==S_LOCK || c->state==S_UNPARK);}
enum a733_cpu_clock_status a733_cpu_clock_burst(struct a733_cpu_clock *c,uint64_t budget_us)
{
    enum a733_cpu_clock_status r=a733_cpu_clock_poll(c);
    if(r!=A733_CPU_CLOCK_PENDING)return r;
    const uint64_t begin=c->last_now;
    for(unsigned i=0;r==A733_CPU_CLOCK_PENDING && a733_cpu_clock_parked(c) && c->last_now-begin<budget_us;++i) {
        if(i==BURST_POLLS)return fault(c);
        r=a733_cpu_clock_poll(c);
    }
    return r;
}
