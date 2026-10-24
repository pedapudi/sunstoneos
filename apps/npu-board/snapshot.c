#include "snapshot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "../board/session.h"
#include <string.h>
#ifndef __aarch64__
int a733_cpu_coherence_publish(uint64_t,uint64_t,uint64_t);
#endif
/* Primary EL3 copies the page once, hashes the copy and publishes it before
 * any provider or secondary core runs; later writes to the page reach nothing. */
static struct {struct npu_board_session session;unsigned attempted,ready;} admitted;
int npu_board_session_prepare(const struct a733_smp_contract *contract)
{
    if(!contract||admitted.attempted||contract->magic!=NPU_BOARD_CONTRACT_MAGIC)return 0;
    admitted.attempted=1;
    const struct npu_board_boot_contract *record=npu_board_boot_record(contract);
    if(!record)return 0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(admitted.session);++i)((uint8_t *)&admitted.session)[i]=source[i];
    uint8_t digest[32];storage_digest_bytes(&admitted.session,sizeof(admitted.session),digest);
    if(memcmp(digest,record->session_digest,32)||!npu_board_session_valid(&admitted.session))return 0;
    admitted.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),contract->startup_deadline)==1)return 1;
    admitted.ready=0;return 0;
}
const struct npu_board_session *npu_board_admitted_session(void)
{return admitted.ready?&admitted.session:NULL;}
int npu_board_prepare(const struct a733_smp_contract *contract)
{
    if(!a733_smp_contract_valid(contract,a733_smp_counter()))return 0;
    const struct npu_board_boot_contract *record=npu_board_boot_record(contract);
    if(!record)return 0;
    struct a733_smp_contract board_view=*contract;
    board_view.bytes=sizeof(board_view);board_view.reserved[0]=A733_BOARD_SESSION_ADDRESS;
    board_view.reserved[1]=sizeof(admitted.session);
    memcpy(&board_view.reserved[2],record->session_digest,32);
    return board_session_prepare(&board_view);
}
