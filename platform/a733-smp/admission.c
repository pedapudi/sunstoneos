#include "boot.h"
#include <stddef.h>
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    if(!c || c->magic!=A733_SMP_CONTRACT_MAGIC || c->version!=1 ||
       c->bytes!=sizeof(*c) || !c->epoch || !c->generation ||
       c->inverse_generation!=~c->generation || !c->lease)return 0;
    for(unsigned i=0;i<32;++i)if(c->build_id[i]!=a733_smp_build_id[i])return 0;
    for(unsigned i=0;i<6;++i)if(c->reserved[i])return 0;
    /* Limit one immutable watchdog lease to the documented four-second mode.
     * Deadlines reserve at least one second for reset after diagnostic stop. */
    if(c->armed_at>now || now>=c->startup_deadline ||
       c->startup_deadline>=c->diagnostic_deadline ||
       c->diagnostic_deadline>=c->expires_at ||
       c->expires_at-c->armed_at!=4*A733_SMP_COUNTER_HZ ||
       c->expires_at-c->diagnostic_deadline<A733_SMP_COUNTER_HZ ||
       c->startup_deadline-now>A733_SMP_COUNTER_HZ)return 0;
    if(c->watchdog_mode!=0x41 || c->watchdog_configuration!=1 ||
       c->watchdog_output!=2 || c->fel_flag!=UINT64_C(0x5aa5a55a) ||
       c->redirect_flag!=UINT64_C(0xfa50392f) || c->redirect_entry!=0x20 || c->alternate_flag)return 0;
    return 1;
}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1 || core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg || address==cfg+4 || address==cfg+8 ||
       address==UINT32_C(0x07050200)+core*4 ||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write && address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
