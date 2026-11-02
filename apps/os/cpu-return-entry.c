/* Initial-state guard for the single eight-worker first-functional profile.
 * Rail values are the enabled/decoded PMIC snapshots obtained by the owner's
 * immediately preceding read-only inspections. The fixed profile has one
 * clock writer; entry authority excludes every external clock/rail writer.
 * This guard establishes configured predicates; it does not qualify B1. */
#include "cpu-return.h"
#include "a733_cpu_clock.h"
#include <stdatomic.h>

__attribute__((used)) const uint64_t os_cpu_return_entry_profile[4]={
    UINT64_C(0x31544e4552505543),1,4,8
};
uint32_t os_cpu_return_entry_refusal;
/* The time at which the lease registry was first seen busy in this run of consecutive passes; zero when it was not. */
uint64_t os_cpu_return_entry_busy_since;
#define OS_BUSY_LIMIT_US 100000u
static int refuse(uint32_t line,uint32_t detail)
{os_cpu_return_entry_refusal=detail<<12|(line&0xfff);return -1;}
#define REFUSE(detail) refuse(__LINE__,(uint32_t)(detail))
int os_cpu_return_entry_guard(const struct a733_cpu_clock *c,
    const struct a733_thermal_policy *p,uint64_t now,
    int (*read)(void *,uintptr_t,uint32_t *),void *cookie)
{
    os_cpu_return_entry_refusal=0;
    if(*(volatile const uint64_t *)os_cpu_return_entry_profile!=UINT64_C(0x31544e4552505543)||
       !c||!p||!read)return REFUSE(0);
    /* The clock lease registry is a try-lock: another core inside it at this moment (the NPU start-up takes its clocks
     * there) says busy, which is a pass to try again, not a refusal. */
    const int thermal=os_cpu_return_thermal_ready();
    if(thermal==2&&p->initialized){
        /* Bounded: a registry that stays busy for 100 ms is a fault, and the session deadline ends the guard in any case
         * (the thermal task stops being the owner once the board is no longer live). */
        if(!os_cpu_return_entry_busy_since)os_cpu_return_entry_busy_since=now?now:1;
        if(now-os_cpu_return_entry_busy_since<=OS_BUSY_LIMIT_US)return 0;
        return REFUSE(0x7ff);
    }
    os_cpu_return_entry_busy_since=0;
    if(!p->initialized||thermal!=1)return REFUSE(p->initialized|os_cpu_return_thermal_state()<<1);
    if(c->failed||c->inherited!=3)return REFUSE(c->inherited);
    /* The PMIC inspection's DCDC5/DCDC3 readings, in 10 mV units to 1540 mV. */
    if(c->rail_uv[0]!=800000||c->rail_uv[1]!=800000)
        return REFUSE((c->rail_uv[0]/10000u&0x3ff)<<10|(c->rail_uv[1]/10000u&0x3ff));
    if(c->config.grant.clusters!=3||c->config.grant.max_mhz[0]!=1794||
       c->config.grant.max_mhz[1]!=2002||c->config.grant.changes<4||c->changes!=c->config.grant.changes)
        return REFUSE(c->changes);
    if(p->cpu_critical_millicelsius!=70000)return REFUSE(0);
    /* Missing first samples are pending. A bad/stale/non-NORMAL set refuses
     * admission; the ordinary terminal failure policy retains the board. */
    if(a733_thermal_policy_seen(p)!=31)return 0;
    if(a733_thermal_policy_level(p)!=A733_THERMAL_NORMAL||p->stale||p->critical)
        return REFUSE(a733_thermal_policy_level(p));
    for(unsigned i=0;i<5;++i)
        if(now<p->seen_us[i]||now-p->seen_us[i]>1000000)return REFUSE(i);
    struct a733_cpu_clock_report live;
    if(!a733_cpu_clock_report(read,cookie,&live))return REFUSE(0);
    if(!live.binned||live.bin!=4)return REFUSE(live.bin);
    if(live.dcxo_hz!=26000000||(live.oscillator&0x10000002)!=0x10000002)return REFUSE(live.oscillator&0xfffff);
    if(live.sid_word!=c->report.sid_word)return REFUSE(live.sid_word&0xfffff);
    static const uint32_t pll[3]={0xf8802600,0xf8802700,0xf8802600};
    static const uint32_t mux[3]={0x03010000,0x03000000,0x03010105};
    for(unsigned i=0;i<3;++i){
        const struct a733_cpu_clock_domain *d=&live.domain[i];
        if(!d->decoded||d->pll!=pll[i]||d->select!=mux[i])return REFUSE(i);
        const uintptr_t base=UINT64_C(0x08871000)+i*0x1000;
        uint32_t pat0,pat1,lfm;
        if(!read(cookie,base+4,&pat0)||!read(cookie,base+8,&pat1)||!read(cookie,base+0x18,&lfm)||
           (pat0&(UINT32_C(3)<<29))||(pat1>>31)||(lfm>>31))return REFUSE(0x10|i);
    }
    return 1;
}
