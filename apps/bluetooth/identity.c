#include "bluetooth.h"
#include "a733_board_application.h"
#include "a733_cpu_coherence.h"
#include "boot.h"
static struct board_radio_chip_id identity;
/* This replaces only the application's existing EL3 publication hook. */
int board_identity_prepare(uint64_t deadline)
{
    const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    const struct bluetooth_profile *p=(const void *)BLUETOOTH_PROFILE_ADDRESS;
    uint64_t el,sctlr;__asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el3":"=r"(el),"=r"(sctlr));
    if(el!=12||(sctlr&5)||identity.secure_read_record||!a733_board_session_valid(s,a733_smp_counter())||
        s->workload!=2||!s->sid_authority||s->sid_authority!=p->sid_authority||
        !bluetooth_profile_valid(p,s->sid_evidence))return 0;
    for(unsigned i=0;i<4;++i){__asm__ volatile("dsb sy":::"memory");
        identity.words[i]=*(volatile const uint32_t *)(uintptr_t)(0x03006200+4*i);}
    __asm__ volatile("dsb sy":::"memory");identity.secure_read_record=s->sid_authority;
    return a733_cpu_coherence_publish((uintptr_t)&identity,sizeof(identity),deadline)==1&&
        a733_cpu_coherence_publish(BLUETOOTH_PROFILE_ADDRESS,0x100000,deadline)==1;
}
const struct board_radio_chip_id *bluetooth_chip_id(void)
{return identity.secure_read_record?&identity:0;}
/* Unused network code remains linked from the shared board image source list. */
const struct board_radio_chip_id *board_radio_chip_id(void){return bluetooth_chip_id();}
