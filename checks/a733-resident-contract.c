/* Host cases for the resident and FEL-launched SMP contract admission rules. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../platform/a733-smp-cached/boot.h"
const unsigned char a733_smp_build_id[32]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
#include "../platform/a733-smp-cached/admission.c"
#define HZ A733_SMP_COUNTER_HZ
static struct a733_smp_contract nominal(uint64_t now)
{
    struct a733_smp_contract c;memset(&c,0,sizeof(c));
    c.magic=A733_SMP_CONTRACT_MAGIC;c.version=1;c.bytes=sizeof(c);c.epoch=7;c.generation=3;
    c.inverse_generation=~UINT64_C(3);memcpy(c.build_id,a733_smp_build_id,32);
    c.armed_at=now-HZ/10;c.startup_deadline=now+HZ/2;c.expires_at=c.armed_at+4*HZ;c.lease=HZ;
    c.watchdog_mode=0x41;c.watchdog_configuration=1;c.watchdog_output=2;
#ifdef SUNSTONE_A733_RESIDENT
    c.diagnostic_deadline=A733_SMP_NO_DEADLINE;
#else
    c.diagnostic_deadline=c.expires_at-2*HZ;c.fel_flag=0x5aa5a55a;c.redirect_flag=0xfa50392f;c.redirect_entry=0x20;
#endif
    return c;
}
int main(void)
{
    const uint64_t now=100*HZ;
    struct a733_smp_contract c=nominal(now);
    assert(a733_smp_contract_valid(&c,now));
    unsigned refused=0;
#define REFUSE(change) do{c=nominal(now);change;assert(!a733_smp_contract_valid(&c,now));++refused;}while(0)
    REFUSE(c.magic^=1);
    REFUSE(c.inverse_generation=0);
    REFUSE(c.build_id[5]^=1);
    REFUSE(c.startup_deadline=now);
    REFUSE(c.startup_deadline=now+2*HZ);
    REFUSE(c.expires_at+=1);
    REFUSE(c.watchdog_mode=0);
    REFUSE(c.armed_at=now+1);
#ifdef SUNSTONE_A733_RESIDENT
    /* A FEL-launched contract cannot admit a resident image. */
    REFUSE(c.diagnostic_deadline=c.expires_at-2*HZ);
    REFUSE(c.fel_flag=0x5aa5a55a);
    REFUSE(c.redirect_flag=0xfa50392f;c.redirect_entry=0x20);
    REFUSE(c.magic=UINT64_C(0x4153373343414331));
    printf("PASS resident contract: nominal admitted, %u refusals\n",refused);
#else
    /* A resident contract cannot admit a FEL-launched image. */
    REFUSE(c.diagnostic_deadline=A733_SMP_NO_DEADLINE);
    REFUSE(c.fel_flag=0);
    REFUSE(c.magic=UINT64_C(0x4153373352455331));
    printf("PASS FEL contract: nominal admitted, %u refusals\n",refused);
#endif
    return 0;
}
