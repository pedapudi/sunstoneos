#include "application.h"
#include "../board/session.h"
#include "a733_board_application.h"
#include "a733_cpu_coherence.h"
#include "boot.h"
static struct board_radio_chip_id identity;
static struct bluetooth_profile profile;
static unsigned attempted;
/* This replaces only the application's existing EL3 publication hook. */
int board_identity_prepare(uint64_t deadline)
{
    if(attempted)return 0;
    attempted=1;
    const struct a733_board_session *s=board_session_snapshot();
    uint64_t el,sctlr;__asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el3":"=r"(el),"=r"(sctlr));
    if(el!=12||(sctlr&5)||!a733_board_session_valid(s,a733_smp_counter()))return 0;
    const volatile struct bluetooth_profile *source=(const void *)BLUETOOTH_PROFILE_ADDRESS;
    uint32_t bytes=source->bytes;
    if(bytes!=624&&bytes!=752&&bytes!=sizeof(profile))return 0;
    for(uint32_t i=0;i<bytes;++i)((uint8_t *)&profile)[i]=((const volatile uint8_t *)source)[i];
    if(profile.bytes!=bytes||!bluetooth_boot_profile(s,&profile)||
       a733_cpu_coherence_publish((uintptr_t)&profile,sizeof(profile),deadline)!=1)return 0;
    for(unsigned i=0;i<4;++i){__asm__ volatile("dsb sy":::"memory");
        identity.words[i]=*(volatile const uint32_t *)(uintptr_t)(0x03006200+4*i);}
    __asm__ volatile("dsb sy":::"memory");identity.secure_read_record=s->sid_authority;
    if(a733_cpu_coherence_publish((uintptr_t)&identity,sizeof(identity),deadline)==1&&
       a733_cpu_coherence_publish(BLUETOOTH_PROFILE_ADDRESS,0x100000,deadline)==1)return 1;
    identity.secure_read_record=0;return 0;
}
const struct bluetooth_profile *bluetooth_admitted_profile(void)
{return identity.secure_read_record?&profile:0;}
const struct board_radio_chip_id *bluetooth_chip_id(void)
{return identity.secure_read_record?&identity:0;}
/* Unused network code remains linked from the shared board image source list. */
const struct board_radio_chip_id *board_radio_chip_id(void){return bluetooth_chip_id();}
