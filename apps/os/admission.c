/* OS contract and session admission over the board's own checks. The board
 * validator runs unchanged on the board prefix; this unit adds the OS
 * magic numbers, the extension and a digest over the complete session. */
#include "os.h"
#define a733_smp_contract_valid os_board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
#ifdef SUNSTONE_BOARD_RESIDENT
_Static_assert(A733_SMP_CONTRACT_MAGIC==A733_OS_RESIDENT_CONTRACT_MAGIC,"resident OS contract magic");
#else
_Static_assert(A733_SMP_CONTRACT_MAGIC==A733_OS_CONTRACT_MAGIC,"supervised OS contract magic");
#endif
int os_session_valid(const struct os_session *s)
{
    if(!s||!a733_board_session_valid(&s->board,0)||!os_workload_valid(s->board.workload)||
       s->magic!=A733_OS_SESSION_MAGIC||s->version!=OS_SESSION_VERSION||s->bytes!=sizeof(*s)||
       !s->thermal_owner||!s->thermal_authority||!present(s->thermal_initial,32)||
       !present(s->thermal_electrical,32)||!present(s->thermal_clocks,32)||
       !present(s->thermal_supervision,32)||s->configure_adc_clock>1||s->alarm_mask!=31||
       s->critical_mask!=31||s->reserved0||present(s->reserved,6)||(s->apb1&~UINT32_C(0x0300001f)))
        return 0;
    /* The thermal owner is distinct from every board owner. */
    uint64_t owners[]={s->board.pmic_owner,s->board.usb0_owner,s->board.typec_owner,s->board.usb2_owner};
    for(unsigned i=0;i<4;++i)if(owners[i]==s->thermal_owner)return 0;
    unsigned parent=s->apb1>>24,div=(s->apb1&31)+1;
    uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
    if(!hz||hz%div||s->apb1_hz!=hz/div)return 0;
#ifdef SUNSTONE_A733_OS_HEADSET
    /* As in the Bluetooth image, the SID authority and the SHA-256 of the
     * headset profile at 0x43000000 (apps/os/identity.c). */
    if(!s->board.sid_authority||!present(s->board.sid_evidence,32))return 0;
#endif
    for(unsigned i=0;i<5;++i)if(!s->limits[i][0]||s->limits[i][0]>=s->limits[i][1]||s->limits[i][1]>4095||
       !s->limits[i][2]||s->limits[i][2]>s->limits[i][0])return 0;
#ifdef SUNSTONE_A733_OS_AUDIO
    /* The I2S image's DMA0 handoff rules, bound to this session:
     * one service channel, the board fabric, the board lease as handoff and
     * evidence identity, and a deadline that only the image derives. */
    const struct os_audio_session *a=&s->audio;
    const struct a733_dma_platform_config *d=&a->dma;
    if(a->magic!=OS_AUDIO_SESSION_MAGIC||a->version!=OS_AUDIO_SESSION_VERSION||a->bytes!=sizeof(*a)||
       !present(a->evidence,32)||!d->owner||d->owner==s->thermal_owner||d->handoff!=s->board.lease||
       a->dma_evidence!=s->board.lease||d->deadline_us||a->dma_established!=OS_AUDIO_DMA_ESTABLISHED||
       a->dma_owned_channels!=0xffff||a->dma_service_channels!=1||a->dma_controller!=0x04601000||
       memcmp(d->fabric,s->board.fabric,sizeof(d->fabric))||memcmp(d->rate_hz,s->board.fabric_hz,sizeof(d->rate_hz))||
       a->audio_cold>1||a->capture_samples<OS_AUDIO_MIN_SAMPLES||a->capture_samples>OS_AUDIO_MAX_SAMPLES||
       present(a->reserved,7))
        return 0;
    for(unsigned i=0;i<4;++i)if(owners[i]==d->owner)return 0;
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    /* The grant names this session; its owner holds only the CPU clock and
     * rail leases. */
    struct a733_cpu_clock_config cpu;
    if(!a733_cpu_clock_grant_load(&s->cpu_clock,s->board.epoch,s->board.generation,&cpu)||
       cpu.owner==s->thermal_owner)return 0;
    for(unsigned i=0;i<4;++i)if(owners[i]==cpu.owner)return 0;
#ifdef SUNSTONE_A733_OS_AUDIO
    if(cpu.owner==s->audio.dma.owner)return 0;
#endif
#endif
    return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct os_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||c->magic!=A733_SMP_CONTRACT_MAGIC||c->reserved[0]!=A733_BOARD_SESSION_ADDRESS||
       c->reserved[1]!=sizeof(*s)||!os_session_valid(s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    /* The board check binds every remaining field against the board prefix. */
    struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
