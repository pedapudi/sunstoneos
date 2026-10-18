#include "snapshot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <string.h>
/* Written only by the primary EL3 hook, before provider construction/SMP. */
static struct {
    struct camera_session session;
    unsigned attempted,ready;
} admitted;
int camera_session_prepare(const struct a733_smp_contract *contract)
{
    if(admitted.attempted)return 0;
    admitted.attempted=1;
    if(!contract||contract->magic!=CAMERA_CONTRACT_MAGIC||
       contract->reserved[0]!=A733_STORAGE_SESSION_ADDRESS||
       contract->reserved[1]!=sizeof(admitted.session))return 0;
    const volatile uint8_t *source=(const void *)A733_STORAGE_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(admitted.session);++i)
        ((uint8_t *)&admitted.session)[i]=source[i];
    uint8_t digest[32];
    storage_digest_bytes(&admitted.session,sizeof(admitted.session),digest);
    if(memcmp(digest,&contract->reserved[2],32)||
       !camera_session_valid(&admitted.session,a733_smp_counter()))return 0;
    admitted.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),contract->startup_deadline)!=1){
        admitted.ready=0;return 0;
    }
    return 1;
}
const struct camera_session *camera_admitted_session(void)
{return admitted.ready?&admitted.session:NULL;}
