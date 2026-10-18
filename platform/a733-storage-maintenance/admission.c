#include "boot.h"
#include "session.h"
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const uint8_t *s=(const uint8_t *)A733_STORAGE_SESSION_ADDRESS;
    if(!c || c->magic!=A733_STORAGE_CONTRACT_MAGIC || c->version!=1 ||
       c->bytes!=sizeof(*c) || !c->epoch || !c->generation ||
       c->inverse_generation!=~c->generation || !c->lease ||
       c->reserved[0]!=A733_STORAGE_SESSION_ADDRESS || c->reserved[1]!=A733_STORAGE_SESSION_BYTES ||
       a733_storage_wire64(s)!=A733_STORAGE_SESSION_MAGIC ||
       a733_storage_wire64(s+8)!=1 || a733_storage_wire64(s+16)!=A733_STORAGE_SESSION_BYTES ||
       a733_storage_wire64(s+24)!=c->epoch || a733_storage_wire64(s+32)!=c->generation ||
       a733_storage_wire64(s+40)!=c->lease || !a733_storage_wire_bounds(s,now))return 0;
    for(unsigned i=0;i<32;++i)if(c->build_id[i]!=a733_smp_build_id[i] ||
       ((const uint8_t *)&c->reserved[2])[i]!=s[184+i])return 0;
    uint64_t before=a733_storage_wire64(s+640),after=a733_storage_wire64(s+648);
    if(after<before || after-before>24000 || now<after || c->armed_at!=before ||
       before>UINT64_MAX-A733_SMP_COUNTER_HZ || c->startup_deadline!=before+A733_SMP_COUNTER_HZ ||
       now>=c->startup_deadline || c->startup_deadline>=c->diagnostic_deadline ||
       c->diagnostic_deadline!=a733_storage_wire64(s+256)*24 ||
       c->expires_at!=a733_storage_wire64(s+136)*24)return 0;
    return c->watchdog_mode==0x41 && c->watchdog_configuration==1 && c->watchdog_output==2 &&
       c->fel_flag==0x5aa5a55a && c->redirect_flag==0xfa50392f && c->redirect_entry==0x20 && !c->alternate_flag;
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
