/* EL3 publishes application inputs and registered initialization before CPU
 * mappings take effect. For a radio workload it also reads the four chip-ID
 * words and publishes the radio profile span, as the board image does for its
 * radio workloads. For a session that grants the headset, the headset library
 * (os_headset_identity) reads the Bluetooth profile and publishes it, and
 * this hook reads and publishes the chip-ID words, as the Bluetooth image
 * does (apps/bluetooth/identity.c). The profile span holds the radio profile
 * at 0x43000000, the Bluetooth profile at OS_HEADSET_PROFILE_ADDRESS and the
 * shared firmware from 0x43010000; a headset session publishes the whole
 * 1 MiB span. Cached start.S clears BSS before this hook,
 * so the records below keep their values. */
#include "os.h"
#include "../board/identity.h"
#include "../board/session.h"
#include "a733_cpu_coherence.h"
#include "a733_mmio.h"
#include "boot.h"
#include "application.h"
/* The headset library's profile copy: absent (null) in an image that does not link it. */
extern int os_headset_identity(const struct a733_board_session *,uint64_t) __attribute__((weak));
static struct board_radio_chip_id identity;
static unsigned attempted;
#ifdef SUNSTONE_A733_OS_GPU
extern const unsigned char __start_sunstone_region_initializers[];
extern const unsigned char __stop_sunstone_region_initializers[];
/* The caller has checked EL3 with translation and caches disabled. Validate
 * the complete application registration before the first store. */
__attribute__((noinline)) int os_application_initialize_regions(uint64_t deadline)
{
    const struct ss_manifest *m=ss_application.manifest;
    const uintptr_t begin=(uintptr_t)__start_sunstone_region_initializers;
    const uintptr_t end=(uintptr_t)__stop_sunstone_region_initializers;
    if(end<begin || (begin&7) || (end-begin)%sizeof(struct ss_region_initializer))return 0;
    const size_t count=(end-begin)/sizeof(struct ss_region_initializer);
    if(count>SS_REGIONS || (count&&(!m || m->region_count>SS_REGIONS)))return 0;
    const struct ss_region_initializer *initializers=(const void *)begin;
    unsigned seen=0;
    for(size_t i=0;i<count;++i){
        const struct ss_region_initializer *init=&initializers[i];
        if(init->reserved || init->region>=m->region_count || (seen&(1u<<init->region)))return 0;
        const struct ss_region *r=&m->regions[init->region];
        const uint64_t bytes=init->zero_prefix_bytes;
        if(r->kind!=SS_REGION_WORK || !(r->access&SS_ACCESS_READ) ||
           !(r->access&SS_ACCESS_GPU_WRITE) || (r->access&SS_ACCESS_WRITE) ||
           !bytes || (bytes&7) || bytes>r->bytes || (r->address&7) ||
           r->address<UINT64_C(0x44000000) || r->address>=UINT64_C(0x47000000) ||
           r->bytes>UINT64_C(0x47000000)-r->address)return 0;
        seen|=1u<<init->region;
    }
    for(size_t i=0;i<count;++i){
        const struct ss_region_initializer *init=&initializers[i];
        const uintptr_t address=(uintptr_t)m->regions[init->region].address;
        volatile uint64_t *words=(volatile uint64_t *)address;
#pragma clang loop unroll(disable)
        for(uint64_t j=0;j<init->zero_prefix_bytes/8;++j)words[j]=0;
        if(a733_cpu_coherence_publish(address,init->zero_prefix_bytes,deadline)!=1)return 0;
    }
    return 1;
}
#endif
int board_identity_prepare(uint64_t deadline)
{
    if(attempted)return 0;
    attempted=1;
    const struct a733_board_session *s=board_session_snapshot();
    uint64_t el,sctlr;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el3":"=r"(el),"=r"(sctlr));
    if(el!=12||(sctlr&5)||!a733_board_session_valid(s,a733_smp_counter())||!os_workload_valid(s->workload))return 0;
    const struct ss_manifest *m=ss_application.manifest;
    for(unsigned i=0;m&&i<m->region_count;++i)
        if(m->regions[i].kind==SS_REGION_INPUT&&
           a733_cpu_coherence_publish(m->regions[i].address,m->regions[i].bytes,deadline)!=1)return 0;
#ifdef SUNSTONE_A733_OS_GPU
    if(!os_application_initialize_regions(deadline))return 0;
#endif
    /* The sealed session's device grant; the session decode later ties it to the workload. */
    const uint8_t *sealed=board_session_sealed();
    const struct ss_session_header *h=sealed?(const void *)(sealed+SS_SESSION_PREFIX_BYTES):NULL;
    if(h&&(h->devices&SS_DEVICE_AUDIO_HEADSET)){
        if(!os_headset_identity||!os_headset_identity(s,deadline))return 0;
    }else{
        /* Only the radio station and the headset read the profile span and the chip ID; any other session leaves them
         * unread. */
        if(!os_radio_workload(s->workload))return 1;
        if(a733_cpu_coherence_publish(s->radio_profile_address,s->radio_profile_bytes,deadline)!=1||
           a733_cpu_coherence_publish(0x43010000,0xf0000,deadline)!=1)return 0;
    }
    /* The session binds secure SID ownership and exclusion evidence. These
     * four read-only words are the entire admitted access. */
    for(unsigned i=0;i<4;++i){
        a733_mmio_dsb_sy();
        identity.words[i]=a733_mmio_read32(0x03006200+4*i);
    }
    a733_mmio_dsb_sy();
    identity.secure_read_record=s->sid_authority;
    if(a733_cpu_coherence_publish((uintptr_t)&identity,sizeof(identity),deadline)==1)return 1;
    identity.secure_read_record=0;return 0;
}
const struct board_radio_chip_id *board_radio_chip_id(void)
{return identity.secure_read_record?&identity:0;}
