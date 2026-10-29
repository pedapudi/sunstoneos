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
int os_cpu_return_entry_guard(const struct a733_cpu_clock *c,
    const struct a733_thermal_policy *p,uint64_t now,
    int (*read)(void *,uintptr_t,uint32_t *),void *cookie)
{
    if(*(volatile const uint64_t *)os_cpu_return_entry_profile!=UINT64_C(0x31544e4552505543)||
       !c||!p||!read||!p->initialized||!os_cpu_return_thermal_ready()||
       c->failed||c->inherited!=3||c->rail_uv[0]!=800000||c->rail_uv[1]!=800000||
       c->config.grant.clusters!=3||c->config.grant.max_mhz[0]!=1794||
       c->config.grant.max_mhz[1]!=2002||c->config.grant.changes<4||c->changes!=c->config.grant.changes||
       p->cpu_critical_millicelsius!=70000)return -1;
    /* Missing first samples are pending. A bad/stale/non-NORMAL set refuses
     * admission; the ordinary terminal failure policy retains the board. */
    if(a733_thermal_policy_seen(p)!=31)return 0;
    if(a733_thermal_policy_level(p)!=A733_THERMAL_NORMAL||p->stale||p->critical)return -1;
    for(unsigned i=0;i<5;++i)
        if(now<p->seen_us[i]||now-p->seen_us[i]>1000000)return -1;
    struct a733_cpu_clock_report live;
    if(!a733_cpu_clock_report(read,cookie,&live)||!live.binned||live.bin!=4||
       live.dcxo_hz!=26000000||(live.oscillator&0x10000002)!=0x10000002||live.sid_word!=c->report.sid_word)return -1;
    static const uint32_t pll[3]={0xf8802600,0xf8802700,0xf8802600};
    static const uint32_t mux[3]={0x03010000,0x03000000,0x03010105};
    for(unsigned i=0;i<3;++i){
        const struct a733_cpu_clock_domain *d=&live.domain[i];
        if(!d->decoded||d->pll!=pll[i]||d->select!=mux[i])return -1;
        const uintptr_t base=UINT64_C(0x08871000)+i*0x1000;
        uint32_t pat0,pat1,lfm;
        if(!read(cookie,base+4,&pat0)||!read(cookie,base+8,&pat1)||!read(cookie,base+0x18,&lfm)||
           (pat0&(UINT32_C(3)<<29))||(pat1>>31)||(lfm>>31))return -1;
    }
    return 1;
}
