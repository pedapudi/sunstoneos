/* CPU cluster record of the integrated OS image with the Cortex-A76 cores
 * admitted (docs/designs/cpu-a76-dvfs.md). Before the scheduler, the thermal
 * start reads the clock configuration once and refuses a CPU-B cluster that
 * is not on its PLL at boot0's 1014 MHz. The recognizer prints the
 * record before and after each recognition (apps/mimus/recorded.cc).
 *
 * With SUNSTONE_A733_OS_CPU_CLOCK the thermal task also owns the CPU clock
 * driver. It inspects both clusters read-only, then requests each cluster
 * that the session's grant names at the grant's highest point once (and, for
 * a grant of both clusters, the DSU), and afterwards lets the driver follow
 * the thermal level. A grant that names
 * no cluster leaves every clock and voltage unchanged. */
#include "os.h"
#include "../board/application.h"
#include "../mimus-board/application.h"
#include "a733_cpu_clock.h"
#include "a733_mmio.h"
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
    atomic_store_explicit(&record,pack(points,mv,0),memory_order_release);
    return 1;
}
/* EL0-safe; the recognizer task. */
int os_cpu_clock_line(char *line,size_t bytes)
{
    uint64_t w=atomic_load_explicit(&record,memory_order_acquire);
    int n=snprintf(line,bytes,"CLOCK cpu_l_mhz=%u cpu_b_mhz=%u cpu_l_mv=%u cpu_b_mv=%u transitions=%u "
        "dsu_mhz=%u sid=%08x bin=%u core=%u pattern=%u parked_max_us=%llu\n",(unsigned)(w&0x7ff),
        (unsigned)(w>>11&0x7ff),(unsigned)(w>>22&0x7ff),(unsigned)(w>>33&0x7ff),(unsigned)(w>>44&0xfffff),
        (unsigned)mhz(A733_CPU_DSU),(unsigned)report.sid_word,report.binned?report.bin:99u,
        (unsigned)portGET_CORE_ID(),(unsigned)atomic_load_explicit(&pattern_bits,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&parked_max_us,memory_order_relaxed));
    return n>0&&(size_t)n<bytes?n:0;
}
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
/* Each pass runs one driver burst: further polls while a cluster is parked,
 * up to 200 us, as the USB0 service loop bounds its passes. */
#define BURST_US 200u
/* Request deadline: the driver's 90 ms before the PLL, two PMIC requests
 * and the ramp. */
#define REQUEST_US 120000u
static struct {
    struct a733_cpu_clock driver;
    unsigned phase,cluster,count;
    atomic_uint ready;
} owner;
enum { START, INSPECT, REQUEST, FOLLOW };
static int clock_now(void *v,uint64_t *out)
{return v==&owner&&a733_counter_clock_now(&providers.shared.counter,out);}
static int clock_read(void *v,uintptr_t a,uint32_t *out){return v==&owner&&read_word(NULL,a,out);}
/* A mux write completes before the driver's settle spin (section 1.2 of
 * docs/performance-architecture.md); the PLL words need no barrier. */
static int clock_write(void *v,uintptr_t a,uint32_t value)
{
    if(v!=&owner||!a733_cpu_clock_address(a,1))return 0;
    a733_mmio_write32(a,value);
    if((a&0xfff)==0x01c)a733_mmio_dsb_sy();
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
    atomic_store_explicit(&parked_max_us,c->parked_max_us,memory_order_relaxed);
    atomic_store_explicit(&record,pack(points,mv,owner.count),memory_order_release);
}
/* EL0-safe: the recognizer waits for this before each timed run. */
int os_cpu_clock_ready(void){return (int)atomic_load_explicit(&owner.ready,memory_order_acquire);}
static uint64_t later(uint64_t us)
{uint64_t now;return a733_counter_clock_now(&providers.shared.counter,&now)?now+us:0;}
int os_cpu_clock_step(const struct a733_thermal_policy *policy)
{
    struct a733_cpu_clock *c=&owner.driver;
    const struct os_session *s=os_session_admitted();
    if(owner.phase==START){
        struct a733_cpu_clock_config config;
        if(!s||!a733_cpu_clock_grant_load(&s->cpu_clock,s->board.epoch,s->board.generation,&config)||
           !a733_cpu_clock_init(c,&config,&clock_ops,&owner,&providers.shared.clocks,&providers.shared.rails,
               &providers.shared.pmic,policy))return -1;
        owner.phase=INSPECT;owner.cluster=A733_CPU_LITTLE;
        if(a733_cpu_clock_inspect(c,A733_CPU_LITTLE,later(REQUEST_US))!=A733_CPU_CLOCK_PENDING)return -1;
        return 1;
    }
    enum a733_cpu_clock_status r=a733_cpu_clock_burst(c,BURST_US);
    if(r==A733_CPU_CLOCK_RETAINED||r==A733_CPU_CLOCK_INVALID)return -1;
    if(r==A733_CPU_CLOCK_PENDING)return 1;
    if(r==A733_CPU_CLOCK_COMPLETE&&owner.phase!=INSPECT)++owner.count;
    if(r!=A733_CPU_CLOCK_READY)publish();
    if(owner.phase==INSPECT){
        if(r!=A733_CPU_CLOCK_COMPLETE)return -1;
        if(owner.cluster==A733_CPU_LITTLE){
            owner.cluster=A733_CPU_BIG;
            return a733_cpu_clock_inspect(c,A733_CPU_BIG,later(REQUEST_US))==A733_CPU_CLOCK_PENDING?1:-1;
        }
        owner.phase=REQUEST;owner.cluster=0;
    }
    if(owner.phase==REQUEST){
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
            enum a733_cpu_clock_status q=a733_cpu_clock_request(c,(enum a733_cpu_domain)d,target,later(REQUEST_US));
            ++owner.cluster;
            if(q==A733_CPU_CLOCK_PENDING)return 1;
            if(q==A733_CPU_CLOCK_RETAINED||q==A733_CPU_CLOCK_INVALID)return -1;
        }
        owner.phase=FOLLOW;atomic_store_explicit(&owner.ready,1,memory_order_release);
    }
    return 1;
}
#endif
