#include "boot.h"
#include "a733_admission_refusal.h"
#include <stddef.h>
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#include "a733_device_memory_layout.h"
#define CONTRACT_VERSION 2u
#else
#define CONTRACT_VERSION 1u
#endif
/* Zero when the contract is admitted, otherwise the named refusal of include/a733_admission_refusal.h. */
unsigned a733_smp_contract_refusal(const struct a733_smp_contract *c,uint64_t now)
{
    if(!c)return A733_REFUSAL(A733_REFUSAL_CONTRACT_FIELD,0);
    if(c->magic!=A733_SMP_CONTRACT_MAGIC)return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_MAGIC);
    if(c->version!=CONTRACT_VERSION)return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_VERSION);
    if(c->bytes!=sizeof(*c))return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_SIZE);
    if(!c->epoch || !c->generation || c->inverse_generation!=~c->generation || !c->lease)
        return A733_REFUSAL(A733_REFUSAL_CONTRACT_FIELD,A733_FIELD_IDENTITY);
    for(unsigned i=0;i<32;++i)if(c->build_id[i]!=a733_smp_build_id[i])return A733_REFUSAL(A733_REFUSAL_BUILD_ID,0);
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    const struct a733_device_memory_layout layout = { A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START, A733_DEVICE_WB_END, A733_DEVICE_NC_START, A733_DEVICE_NC_END };
    if(!a733_device_memory_handoff_valid(&layout,c->reserved))return A733_REFUSAL(A733_REFUSAL_DEVICE_MEMORY,0);
#else
    for(unsigned i=0;i<6;++i)if(c->reserved[i])return A733_REFUSAL(A733_REFUSAL_DEVICE_MEMORY,0);
#endif
#ifdef SUNSTONE_A733_RESIDENT
    /* Resident lifetime: no diagnostic deadline and no FEL return. The armed
     * four-second watchdog is the bounded-failure mechanism, so its first
     * window must end after startup; a supervising task renews it only while
     * every service progresses. Startup keeps the one-second bound. */
    if(c->armed_at>now)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_ARMED);
    if(now>=c->startup_deadline || c->startup_deadline-now>A733_SMP_COUNTER_HZ)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_STARTUP);
    if(c->diagnostic_deadline!=A733_SMP_NO_DEADLINE)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_DIAGNOSTIC);
    if(c->expires_at-c->armed_at!=4*A733_SMP_COUNTER_HZ || c->startup_deadline>=c->expires_at)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_EXPIRY);
    if(c->watchdog_mode!=0x41 || c->watchdog_configuration!=1 ||
       c->watchdog_output!=2 || c->fel_flag || c->redirect_flag ||
       c->redirect_entry || c->alternate_flag)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_WATCHDOG_FIELDS);
    return 0;
#endif
    /* Limit one immutable watchdog lease to the documented four-second mode.
     * Deadlines reserve at least one second for reset after diagnostic stop. */
    if(c->armed_at>now)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_ARMED);
    if(now>=c->startup_deadline || c->startup_deadline-now>A733_SMP_COUNTER_HZ)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_STARTUP);
    if(c->startup_deadline>=c->diagnostic_deadline || c->diagnostic_deadline>=c->expires_at ||
       c->expires_at-c->diagnostic_deadline<A733_SMP_COUNTER_HZ)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_DIAGNOSTIC);
    if(c->expires_at-c->armed_at!=4*A733_SMP_COUNTER_HZ)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_EXPIRY);
    if(c->watchdog_mode!=0x41 || c->watchdog_configuration!=1 ||
       c->watchdog_output!=2 || c->fel_flag!=UINT64_C(0x5aa5a55a) ||
       c->redirect_flag!=UINT64_C(0xfa50392f) || c->redirect_entry!=0x20 || c->alternate_flag)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_WATCHDOG_FIELDS);
    return 0;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{return !a733_smp_contract_refusal(c,now);}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1 || core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg || address==cfg+4 || address==cfg+8 ||
       address==UINT32_C(0x07050200)+core*4 ||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write && address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
