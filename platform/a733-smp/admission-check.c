#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32]={1,2,3,4};
int main(void)
{
    struct a733_smp_contract c={.magic=A733_SMP_CONTRACT_MAGIC,.version=1,.bytes=sizeof(c),
        .epoch=5,.generation=9,.inverse_generation=~UINT64_C(9),.armed_at=100,
        .startup_deadline=A733_SMP_COUNTER_HZ,.diagnostic_deadline=2*A733_SMP_COUNTER_HZ,
        .expires_at=100+4*A733_SMP_COUNTER_HZ,.lease=7,.watchdog_mode=0x41,
        .watchdog_configuration=1,.watchdog_output=2,.fel_flag=0x5aa5a55a,
        .redirect_flag=0xfa50392f,.redirect_entry=0x20};
    memcpy(c.build_id,a733_smp_build_id,32);
    assert(a733_smp_contract_valid(&c,101));
    for(unsigned i=0;i<sizeof(c);++i) {
        struct a733_smp_contract bad=c;
        ((unsigned char *)&bad)[i]^=0x80;
        /* Opaque epoch/lease/generation and admissible deadline changes are
         * intentional inputs; identity, fixed ABI and reserved bits must fail. */
        int strict=i<24 || (i>=40 && i<88) || i>=128;
        if(strict)assert(!a733_smp_contract_valid(&bad,101));
    }
    assert(!a733_smp_contract_valid(&c,99));
    assert(!a733_smp_contract_valid(&c,c.startup_deadline));
    struct a733_smp_contract bad=c;bad.diagnostic_deadline=bad.expires_at;
    assert(!a733_smp_contract_valid(&bad,101));
    bad=c;bad.watchdog_configuration=0x101;assert(!a733_smp_contract_valid(&bad,101));
    for(unsigned core=1;core<8;++core) {
        uintptr_t cfg=0x08001000+core*0x1000;
        uintptr_t addresses[]={cfg,cfg+4,cfg+8,0x07050200+core*4,0x07050220+core*4};
        for(unsigned i=0;i<5;++i) {
            assert(a733_smp_power_address(core,addresses[i],0));
            assert(a733_smp_power_address(core,addresses[i],1));
            assert(!a733_smp_power_address(core,addresses[i]+1,1));
            assert(!a733_smp_power_address(core==7?1:core+1,addresses[i],1));
        }
        assert(a733_smp_power_address(core,0x07052008+core*0x1000,0));
        assert(!a733_smp_power_address(core,0x07052008+core*0x1000,1));
        assert(!a733_smp_power_address(core,0x02050014,1));
    }
    assert(!a733_smp_power_address(0,0x08001000,1));
    assert(!a733_smp_power_address(8,0x08009000,1));
    puts("PASS contract tampering, deadlines, watchdog source refusal and per-core MMIO boundaries");
}
