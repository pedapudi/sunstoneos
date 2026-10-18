#include "boot.h"
#include <stddef.h>
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    if(!c || c->magic!=A733_SMP_CONTRACT_MAGIC || c->version!=3 ||
       c->bytes!=sizeof(*c) || !c->epoch || !c->generation ||
       c->inverse_generation!=~c->generation || !c->lease)return 0;
    for(unsigned i=0;i<32;++i)if(c->build_id[i]!=a733_smp_build_id[i])return 0;
    /* The reservation and admission digest bind real external evidence. They
     * cannot establish physical ownership merely by being nonzero. */
    uint64_t digest=c->reserved[2]|c->reserved[3]|c->reserved[4]|c->reserved[5];
    if(!c->reserved[0] || c->reserved[1] || !digest)return 0;
    /* armed_at is the session start in this separate ABI, never a timer arm.
     * One second bounds startup; twenty seconds bound result observation. */
    if(c->armed_at>now || c->armed_at>UINT64_MAX-20*A733_SMP_COUNTER_HZ ||
       now>=c->startup_deadline || c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ ||
       c->diagnostic_deadline!=c->armed_at+20*A733_SMP_COUNTER_HZ ||
       c->expires_at!=c->diagnostic_deadline)return 0;
    if(c->watchdog_mode || c->watchdog_configuration || c->watchdog_output ||
       c->fel_flag || c->redirect_flag || c->redirect_entry || c->alternate_flag)return 0;
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
