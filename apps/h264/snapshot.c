#include "a733_h264_application.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "boot.h"
#ifdef SUNSTONE_H264_FULLHD
#include "fullhd-boot.h"
#include "../board/session.h"
#endif
#include <string.h>
#ifndef __aarch64__
/* Host admission fixture supplies this explicit publication boundary. */
int a733_cpu_coherence_publish(uint64_t,uint64_t,uint64_t);
#endif
/* Written only by the primary EL3 admission hook, before SMP starts. */
static struct {
    struct a733_h264_session session;
    unsigned ready;
} admitted;
int h264_session_prepare(const struct a733_smp_contract *contract)
{
    if(!contract||admitted.ready||contract->magic!=A733_H264_CONTRACT_MAGIC)return 0;
#ifdef SUNSTONE_H264_FULLHD
    const struct h264_fullhd_boot_contract *record=h264_fullhd_boot_record(contract);
    if(!record)return 0;
    const uint8_t *expected=record->session_digest;
#else
    if(contract->reserved[0]!=A733_BOARD_SESSION_ADDRESS||contract->reserved[1]!=sizeof(admitted.session))return 0;
    const uint8_t *expected=(const void *)&contract->reserved[2];
#endif
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(admitted.session);++i)
        ((uint8_t *)&admitted.session)[i]=source[i];
    uint8_t digest[32];
    storage_digest_bytes(&admitted.session,sizeof(admitted.session),digest);
    if(memcmp(digest,expected,32)||
       !a733_h264_session_valid(&admitted.session))return 0;
    admitted.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),
                                 contract->startup_deadline)==1)return 1;
    admitted.ready=0;
    return 0;
}
const struct a733_h264_session *h264_admitted_session(void)
{return admitted.ready?&admitted.session:NULL;}

#ifdef SUNSTONE_H264_FULLHD
int h264_fullhd_board_prepare(const struct a733_smp_contract *contract)
{
    if(!a733_smp_contract_valid(contract,a733_smp_counter()))return 0;
    const struct h264_fullhd_boot_contract *record=h264_fullhd_boot_record(contract);
    if(!record)return 0;
    /* The real board sealer copies856bytes into its own BSS. This stack view
     * is never retained, exposed by an accessor, or used for memory admission. */
    struct a733_smp_contract board_view=*contract;
    board_view.bytes=sizeof(board_view);board_view.reserved[0]=A733_BOARD_SESSION_ADDRESS;
    board_view.reserved[1]=sizeof(admitted.session);
    memcpy(&board_view.reserved[2],record->session_digest,32);
    return board_session_prepare(&board_view);
}
#endif
