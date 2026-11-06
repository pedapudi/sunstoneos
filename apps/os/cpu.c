/* CPU cluster record of the integrated OS image with the Cortex-A76 cores
 * admitted (docs/designs/cpu-a76-dvfs.md). Before the scheduler, the thermal
 * start reads the clock configuration once and refuses a CPU-B cluster that
 * is not on its PLL at boot0's 1014 MHz. Applications read the clock record through ss_clock_info().
 *
 * When the session grants the CPU clock the thermal task also owns the CPU clock
 * driver. It inspects both clusters read-only, then requests each cluster
 * that the session's grant names at the grant's highest point once (and, for
 * a grant of both clusters, the DSU), and afterwards lets the driver follow
 * the thermal level. A grant that names
 * no cluster leaves every clock and voltage unchanged. */
#include "os.h"
#include "sunstone_crumb.h"
#include "cpu-return.h"
#include "../board/application.h"
#include "application.h"
#include "a733_cpu_clock.h"
#include "a733_mmio.h"
#include "perf.h"
#include "FreeRTOS.h"
#include <stdatomic.h>
#include <stdio.h>
static struct a733_cpu_clock_report report;
/* Applied CPU-L and CPU-B points in MHz and their voltages in mV, 11 bits
 * each, then a 20-bit count of completed transitions. One store-release by
 * the owner and one load-acquire by a reader; aligned 64-bit accesses are
 * single-copy atomic on AArch64. A zero voltage was not read. */
static _Atomic uint64_t record;
/* Written by the owner before the record's store-release and read after its
 * load-acquire; relaxed atomics because a later publication may race a read. */
static _Atomic uint32_t pattern_bits;  /* Pattern or LFM enables seen: bit 0 CPU-L, bit 1 CPU-B. */
static _Atomic uint32_t dsu_mhz;
static _Atomic uint64_t parked_max_us; /* Longest park of any transition, microseconds. */
static uint64_t pack(const uint32_t mhz[2],const uint32_t mv[2],uint32_t count)
{
    return (uint64_t)(mhz[0]&0x7ff)|(uint64_t)(mhz[1]&0x7ff)<<11|(uint64_t)(mv[0]&0x7ff)<<22|
        (uint64_t)(mv[1]&0x7ff)<<33|(uint64_t)(count&0xfffff)<<44;
}
/* Device-nGnRnE reads of the words that a733_cpu_clock_address allows; the
 * thermal start has checked their mappings. */
static int read_word(void *cookie,uintptr_t a,uint32_t *v)
{
    (void)cookie;
    if(!a733_cpu_clock_address(a,0))return 0;
    *v=a733_mmio_read32(a);return 1;
}
static uint32_t mhz(enum a733_cpu_domain d)
{return report.domain[d].decoded?(uint32_t)(report.domain[d].hz/1000000u):0;}
static atomic_uint boot_point;
/* The boot-point check is needed by a session that carries the CPU clock record (the owner reads the clock tree, and a
 * grant starts from boot0's CPU-B point) and by an application placed on the A76 cores. */
int os_cpu_boot_point_needed(void)
{
    const struct os_session *s=os_session_admitted();
    const struct ss_manifest *m=ss_application.manifest;
    return (s&&(s->granted&OS_GRANT_CPU_CLOCK))||(m&&m->cluster==SS_CLUSTER_A76);
}
/* 1 once os_cpu_admit has passed; EL0-safe. */
int os_cpu_boot_point_ok(void){return (int)atomic_load_explicit(&boot_point,memory_order_acquire);}
int os_cpu_admit(void)
{
    if(!a733_cpu_clock_report(read_word,NULL,&report))return 0;
    const struct a733_cpu_clock_domain *big=&report.domain[A733_CPU_BIG];
    /* CPU-B at boot0's 1014 MHz point. The FEL ROM resets only the CPU-L and
     * DSU PLLs on a return, so an earlier image of the same lifetime can leave
     * CPU-B raised; B1's baseline and every grant start from boot0's point
     * (docs/designs/cpu-top-opp.md). */
    if(!big->decoded||big->source!=3||big->hz!=UINT64_C(1014000000))return 0;
    const uint32_t points[2]={mhz(A733_CPU_LITTLE),mhz(A733_CPU_BIG)},mv[2]={0,0};
    atomic_store_explicit(&dsu_mhz,mhz(A733_CPU_DSU),memory_order_relaxed);
    atomic_store_explicit(&record,pack(points,mv,0),memory_order_release);
    atomic_store_explicit(&boot_point,1,memory_order_release);
    return 1;
}
/* EL0-safe; the recognizer task. */
int os_cpu_clock_line(char *line,size_t bytes)
{
    uint64_t w=atomic_load_explicit(&record,memory_order_acquire);
    int n=snprintf(line,bytes,"CLOCK cpu_l_mhz=%u cpu_b_mhz=%u cpu_l_mv=%u cpu_b_mv=%u transitions=%u "
        "dsu_mhz=%u sid=%08x bin=%u core=%u pattern=%u parked_max_us=%llu\n",(unsigned)(w&0x7ff),
        (unsigned)(w>>11&0x7ff),(unsigned)(w>>22&0x7ff),(unsigned)(w>>33&0x7ff),(unsigned)(w>>44&0xfffff),
        (unsigned)atomic_load_explicit(&dsu_mhz,memory_order_relaxed),(unsigned)report.sid_word,report.binned?report.bin:99u,
        (unsigned)portGET_CORE_ID(),(unsigned)atomic_load_explicit(&pattern_bits,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&parked_max_us,memory_order_relaxed));
    return n>0&&(size_t)n<bytes?n:0;
}
/* Each pass runs one driver burst: further polls while a cluster is parked,
 * up to 200 us, as the USB0 service loop bounds its passes. */
#define BURST_US 200u
/* Request deadline: the driver's 90 ms before the PLL, two PMIC requests
 * and the ramp. */
#define REQUEST_US 120000u
static struct {
    struct a733_cpu_clock driver;
    unsigned phase,cluster,count,perf;
    atomic_uint ready;
} owner;
enum { START, INSPECT, REQUEST, FOLLOW };
static unsigned entry_checked;
static unsigned return_request;
static unsigned return_clock_last;
/* Emit once per change of return step, owner phase or driver state. The
 * thermal owner is the sole writer; repeated pending polls add no marker. */
static void return_clock_progress(unsigned step)
{
    unsigned value=step<<12|(owner.phase&3u)<<10|(owner.cluster&3u)<<8|
        (owner.driver.state&255u);
    if(value!=return_clock_last){
        sunstone_crumb(SUNSTONE_CRUMB_CLOCK_RETURN,value);
        return_clock_last=value;
    }
}
static atomic_uint return_done;
/* DCDC5, DCDC3: last confirmed PMIC readbacks at clock close; 0 is unread.
 * Captured after normal ROM return, before restoring image RAM. */
uint32_t os_cpu_return_rail_uv[2];
int os_cpu_return_clock_done(void){return atomic_load_explicit(&return_done,memory_order_acquire);}
/* Capture before retention changes shared-manager fault flags. ESR aux:
 * state[3:0], domain[5:4], origin[7:6], phase[9:8], cursor[11:10],
 * prior stored result (c->result)[14:12]. FAR: provider status[7:0]
 * (0xff local check), target MHz[18:8], flags[31:19]; see NORMAL-RETURN.md. */
void a733_cpu_clock_retained_note(const struct a733_cpu_clock *c,
    unsigned line,const void *here,uint32_t status)
{
    if(c!=&owner.driver)return;
    uint32_t aux=(c->state&15u)|(c->cluster&3u)<<4|(c->thermal&3u)<<6|
        (owner.phase&3u)<<8|(owner.cluster&3u)<<10|(c->result&7u)<<12;
    unsigned authority=a733_power_authority_load(&providers.shared.rails.authority);
    uint32_t value=(status&0xffu)|(c->target_mhz&0x7ffu)<<8|
        (uint32_t)!!c->inspect_only<<19|(uint32_t)!!os_cpu_return_requested()<<20|
        (uint32_t)!!atomic_load_explicit(&providers.shared.counter.failed,memory_order_acquire)<<21|
        (uint32_t)!!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)<<22|
        (uint32_t)!!(authority&A733_POWER_LEDGER_FAULT)<<23|
        (uint32_t)!!(authority&A733_POWER_PROVIDER_FAULT)<<24|
        (uint32_t)!!c->clock.id<<25|(uint32_t)!!c->rail.id<<26|(uint32_t)!!c->request.id<<27|
        (uint32_t)(c->last_now>=c->deadline)<<28|(uint32_t)(c->last_now>=c->timer)<<29|
        (uint32_t)!!a733_cpu_clock_parked(c)<<30|(uint32_t)!!c->critical<<31;
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_CPU_RETAINED<<12|(line&0xfffu)|aux<<16,value,here);
}
void a733_cpu_clock_pll_mismatch_note(const struct a733_cpu_clock *c,
    unsigned line,const void *here,uint32_t pll)
{
    if(c!=&owner.driver)return;
    uint32_t aux=(c->n&255u)|(c->cluster&3u)<<8|(c->state&15u)<<10|
        (uint32_t)!!os_cpu_return_requested()<<14;
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_CPU_PLL<<12|(line&0xfffu)|aux<<16,pll,here);
}
static int clock_now(void *v,uint64_t *out)
{return v==&owner&&a733_counter_clock_now(&providers.shared.counter,out);}
static int clock_read(void *v,uintptr_t a,uint32_t *out){return v==&owner&&read_word(NULL,a,out);}
/* Writes complete before the driver measures peripheral settling intervals
 * (section 1.2 of docs/performance-architecture.md). */
static int clock_write(void *v,uintptr_t a,uint32_t value)
{
    if(v!=&owner||!a733_cpu_clock_write_allowed(a,value))return 0;
    a733_mmio_write32(a,value);
    a733_mmio_dsb_sy();
    return 1;
}
static const struct a733_cpu_clock_ops clock_ops={clock_now,clock_read,clock_write};
static void publish(void)
{
    const struct a733_cpu_clock *c=&owner.driver;
    uint32_t points[2],mv[2],pattern=0;
    for(unsigned i=0;i<2;++i){
        points[i]=c->applied_mhz[i]?c->applied_mhz[i]:mhz(i);
        mv[i]=c->rail_uv[i]/1000u;
        if((c->words[i][0]&(UINT32_C(3)<<29))||(c->words[i][1]>>31)||(c->words[i][2]>>31))pattern|=1u<<i;
    }
    atomic_store_explicit(&pattern_bits,pattern,memory_order_relaxed);
    atomic_store_explicit(&dsu_mhz,c->applied_mhz[A733_CPU_DSU]?c->applied_mhz[A733_CPU_DSU]:mhz(A733_CPU_DSU),memory_order_relaxed);
    atomic_store_explicit(&parked_max_us,c->parked_max_us,memory_order_relaxed);
    atomic_store_explicit(&record,pack(points,mv,owner.count),memory_order_release);
}
/* Thermal task: 1 while the owner has work that needs its next pass within a tick (a session that grants the clock and
 * a driver that is not idle, or an owner that has not reached FOLLOW). An ungranted session, or a granted one that is
 * following the thermal level, lets the task sleep until its next reading. */
int os_cpu_clock_busy(void)
{
    if(os_cpu_return_requested())return 1; /* the return lowers CPU-B within its bounded window */
    return os_granted(OS_GRANT_CPU_CLOCK)&&(owner.phase!=FOLLOW||owner.driver.state!=0);
}
/* EL0-safe: the recognizer waits for this before each timed run. */
int os_cpu_clock_ready(void){return !os_granted(OS_GRANT_CPU_CLOCK)||(int)atomic_load_explicit(&owner.ready,memory_order_acquire);}
static uint64_t later(uint64_t us)
{uint64_t now;return a733_counter_clock_now(&providers.shared.counter,&now)?now+us:0;}
/* Each refusal records its line, phase, cluster and full status word before
 * the thermal owner's fault() ends the session (include/sunstone_crumb.h). */
static int refused(uint32_t site,const void *here,uint32_t status)
{board_fail_note(site|SUNSTONE_CRUMB_CPU_AUX|((uint32_t)owner.phase<<8|(owner.cluster&0xffu))<<16,status,here);return -1;}
#define REFUSED(status) refused(SUNSTONE_CRUMB_FAIL_SITE(CPU_CLOCK),SUNSTONE_CRUMB_HERE(),(uint32_t)(status))
int os_cpu_clock_step(const struct a733_thermal_policy *policy)
{
    struct a733_cpu_clock *c=&owner.driver;
    /* A session without the CPU clock record reaches no register here: no read of the clock tree, no rail, no PMIC. */
    if(!os_granted(OS_GRANT_CPU_CLOCK))return 0;
    if(owner.phase==START&&!os_cpu_return_requested()){
        /* The session is read once, here: later steps (1 kHz while the driver works) need nothing from it. */
        const struct os_session *s=os_session_admitted();
        struct a733_cpu_clock_config config;
        if(!s||!a733_cpu_clock_grant_load(&s->cpu_clock,s->board.epoch,s->board.generation,&config)||
           !a733_cpu_clock_init(c,&config,&clock_ops,&owner,&providers.shared.clocks,&providers.shared.rails,
               &providers.shared.pmic,policy))return REFUSED(0);
        owner.perf=config.grant.perf!=0;
        owner.phase=INSPECT;owner.cluster=A733_CPU_LITTLE;
        if(a733_cpu_clock_inspect(c,A733_CPU_LITTLE,later(REQUEST_US))!=A733_CPU_CLOCK_PENDING)return REFUSED(0);
        return 1;
    }
    /* No automatic restoration may raise a clock during successful close.
     * Existing thermal lowering remains permitted and bounded. */
    if(os_cpu_return_requested()){
        return_clock_progress(return_request?3u:1u);c->throttled=0;
    }
    /* START has no active driver when close precedes the first owner pass. */
    enum a733_cpu_clock_status r=owner.phase==START ? A733_CPU_CLOCK_READY : a733_cpu_clock_burst(c,BURST_US);
    /* A return may be requested in any phase, including before the raises
     * finish: an application need not wait for ss_clock_ready(). From then
     * on the owner starts no inspection or request of its own. A transition
     * already in flight finishes first. Then CPU-B, if a transition left it
     * above boot0's 1014 MHz, is lowered to that point, which also returns
     * DCDC3 to its floor; the ROM return resets CPU-L and the DSU
     * (docs/designs/cpu-top-opp.md). With CPU-B not raised nothing is
     * written and no change is consumed. An expired raise or a thermal
     * lowering can leave DCDC3 raised at 1014 MHz: report its confirmed
     * readback for the external rail helper, which normalizes both rails. */
    if(os_cpu_return_requested()){
        return_clock_progress(return_request?3u:1u);
        if(r==A733_CPU_CLOCK_RETAINED||r==A733_CPU_CLOCK_INVALID)return REFUSED(r);
        if(r==A733_CPU_CLOCK_PENDING)return 1;
        if(!return_request&&c->applied_mhz[A733_CPU_BIG]>1014){
            return_clock_progress(2);
            if(a733_cpu_clock_request(c,A733_CPU_BIG,1014,later(REQUEST_US))!=A733_CPU_CLOCK_PENDING)return REFUSED(0);
            return_request=1;return 1;
        }
        if(return_request&&r!=A733_CPU_CLOCK_COMPLETE)return REFUSED(r);
        return_clock_progress(4);
        struct a733_cpu_clock_report final;
        if(!a733_cpu_clock_report(read_word,NULL,&final)||!final.domain[A733_CPU_BIG].decoded||
           final.domain[A733_CPU_BIG].source!=3||final.domain[A733_CPU_BIG].hz!=UINT64_C(1014000000)||
           c->clock.id||c->rail.id||c->request.id)return REFUSED(0);
        os_cpu_return_rail_uv[0]=c->rail_uv[0];os_cpu_return_rail_uv[1]=c->rail_uv[1];
        /* The lowering is a change: the published record names CPU-B at 1014 MHz and DCDC3's confirmed readback, as the
         * rails read at close do. */
        if(return_request){++owner.count;publish();}
        atomic_store_explicit(&return_done,1,memory_order_release);
        return_clock_progress(5);return 1;
    }
    /* The performance-mode service (a grant with perf) replaces the one-request-per-cluster
     * start: its boot switch moves the CPU domains to the boot mode, and it
     * then serves USB0 operation 8 (apps/os/perf.c). It sees every burst
     * result, including a retained driver, before the session ends. */
    if(owner.phase==FOLLOW) {
        int ready=os_perf_step(c,policy,(int)r);
        if(ready<0)return REFUSED(0);
        if(ready && !atomic_load_explicit(&owner.ready,memory_order_relaxed))
            atomic_store_explicit(&owner.ready,1,memory_order_release);
    }
    if(r==A733_CPU_CLOCK_RETAINED||r==A733_CPU_CLOCK_INVALID)return REFUSED(r);
    if(r==A733_CPU_CLOCK_PENDING)return 1;
    if(r==A733_CPU_CLOCK_COMPLETE&&owner.phase!=INSPECT)++owner.count;
    if(r!=A733_CPU_CLOCK_READY)publish();
    if(owner.phase==INSPECT){
        if(r!=A733_CPU_CLOCK_COMPLETE)return REFUSED(r);
        if(owner.cluster==A733_CPU_LITTLE){
            owner.cluster=A733_CPU_BIG;
            return a733_cpu_clock_inspect(c,A733_CPU_BIG,later(REQUEST_US))==A733_CPU_CLOCK_PENDING?1:REFUSED(0);
        }
        if(owner.perf){owner.phase=FOLLOW;return 1;}
        owner.phase=REQUEST;owner.cluster=0;
    }
    if(owner.phase==REQUEST){
        /* The returning lifetime checks the initial state once before its first request (apps/os/cpu-return-entry.c). */
        if(!entry_checked&&os_returning()){
            uint64_t now;
            if(!clock_now(&owner,&now))return REFUSED(0);
            int admitted=os_cpu_return_entry_guard(c,policy,now,read_word,NULL);
            if(admitted<0)return refused(SUNSTONE_CRUMB_FAIL_SITE(CPU_CLOCK)|SUNSTONE_CRUMB_CPU_GUARD,SUNSTONE_CRUMB_HERE(),os_cpu_return_entry_refusal);
            if(!admitted)return 1;
            entry_checked=1;
            sunstone_crumb(SUNSTONE_CRUMB_CLOCK_GUARD,0);
        }
        /* One request per granted domain once its sensor has a reading (CPUL
         * is physical channel 3, CPUB channel 0), in the default path's order
         * (docs/designs/cpu-top-opp.md): CPU-B, CPU-L, then the DSU at the
         * vendor dsufreq point for the granted cluster maxima. Only a grant
         * of both clusters (the top points) moves the DSU; a one-cluster grant
         * keeps the boot DSU point, as before. */
        static const unsigned order[3]={A733_CPU_BIG,A733_CPU_LITTLE,A733_CPU_DSU};
        static const unsigned sensor[3]={1u<<0,1u<<3,1u<<3};
        const struct a733_cpu_clock_grant *g=&c->config.grant;
        for(;owner.cluster<3;++owner.cluster){
            const unsigned d=order[owner.cluster];
            const unsigned granted=d==A733_CPU_DSU ? g->clusters==3u : g->clusters&(1u<<d);
            if(!granted)continue;
            if(!(a733_thermal_policy_seen(policy)&sensor[owner.cluster]))return 1;
            uint32_t target=d==A733_CPU_DSU ? 0 : g->max_mhz[d];
            if(d==A733_CPU_DSU){
                const uint32_t big=c->applied_mhz[A733_CPU_BIG] ? c->applied_mhz[A733_CPU_BIG] : mhz(A733_CPU_BIG);
                target=c->report.binned ? a733_cpu_clock_dsu_top(c->report.bin,c->applied_mhz[A733_CPU_LITTLE],big) : 0;
                if(!target)continue;
            }
            sunstone_crumb(SUNSTONE_CRUMB_CLOCK_CHANGE,d<<12|target);
            enum a733_cpu_clock_status q=a733_cpu_clock_request(c,(enum a733_cpu_domain)d,target,later(REQUEST_US));
            ++owner.cluster;
            if(q==A733_CPU_CLOCK_PENDING)return 1;
            if(q==A733_CPU_CLOCK_RETAINED||q==A733_CPU_CLOCK_INVALID)return REFUSED(q);
        }
        owner.phase=FOLLOW;atomic_store_explicit(&owner.ready,1,memory_order_release);
        sunstone_crumb(SUNSTONE_CRUMB_CLOCK_READY,0);
    }
    return 1;
}
