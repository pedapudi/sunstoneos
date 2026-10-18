#include "identity.h"
#include "a733_board_application.h"
#include "a733_cpu_coherence.h"
#include "boot.h"
/* Written once by admitted primary EL3 before SMP starts. */
static struct board_radio_chip_id identity;
int board_identity_prepare(uint64_t deadline)
{
    const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    uint64_t el,sctlr;__asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el3":"=r"(el),"=r"(sctlr));
    if(el!=12||(sctlr&5)||!a733_board_session_valid(s,a733_smp_counter())||
       identity.secure_read_record)return 0;
    /* The session digest binds secure SID ownership/exclusion evidence. These
     * four source-defined read-only words are the entire admitted access. */
    for(unsigned i=0;i<4;++i){
        __asm__ volatile("dsb sy":::"memory");
        identity.words[i]=*(volatile const uint32_t *)(uintptr_t)(0x03006200+4*i);
    }
    __asm__ volatile("dsb sy":::"memory");
    if(s->workload&&
       (a733_cpu_coherence_publish(s->radio_profile_address,s->radio_profile_bytes,deadline)!=1||
        a733_cpu_coherence_publish(0x43010000,0xf0000,deadline)!=1))return 0;
    identity.secure_read_record=s->sid_authority;
    return a733_cpu_coherence_publish((uintptr_t)&identity,sizeof(identity),deadline)==1;
}
const struct board_radio_chip_id *board_radio_chip_id(void)
{return identity.secure_read_record?&identity:0;}
