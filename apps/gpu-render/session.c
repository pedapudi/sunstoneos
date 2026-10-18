#include "session.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <stddef.h>
static struct a733_board_session sealed_session;
static unsigned session_sealed;
const struct a733_board_session *gpu_application_session(void)
{return session_sealed?&sealed_session:NULL;}
int gpu_application_seal_session(const struct a733_smp_contract *c)
{
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=12||session_sealed||!a733_smp_contract_valid(c,a733_smp_counter()))return 0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(sealed_session);++i)((uint8_t *)&sealed_session)[i]=source[i];
    uint8_t digest[32];storage_digest_bytes(&sealed_session,sizeof(sealed_session),digest);
    const uint8_t *bound=gpu_application_contract_digest(c);
    if(!bound)return 0;
    for(unsigned i=0;i<32;++i)if(digest[i]!=bound[i])return 0;
    session_sealed=1;
    return a733_cpu_coherence_publish((uintptr_t)&sealed_session,sizeof(sealed_session),c->startup_deadline)==1&&
        a733_cpu_coherence_publish((uintptr_t)&session_sealed,sizeof(session_sealed),c->startup_deadline)==1;
}
