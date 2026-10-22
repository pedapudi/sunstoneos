#include "snapshot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <string.h>
#ifdef SUNSTONE_PWM_SINGLE_CORE
/* The boot-CPU image fills this state at EL3 before EL1 clears .bss, so it
 * keeps it in the retained section that the monitor clears beforehand. */
#define PWM_SESSION_RETAINED __attribute__((section(".pwm_runtime_retained")))
#else
#define PWM_SESSION_RETAINED
#endif
/* Written only by the primary EL3 hook, before provider construction/SMP. */
static struct {
    struct pwm_session session;
    unsigned attempted,ready;
} admitted PWM_SESSION_RETAINED;
int pwm_session_prepare(const struct a733_smp_contract *contract)
{
    if(admitted.attempted)return 0;
    admitted.attempted=1;
    if(!contract||contract->magic!=A733_PWM_CONTRACT_MAGIC||
       contract->reserved[0]!=A733_BOARD_SESSION_ADDRESS||
       contract->reserved[1]!=sizeof(admitted.session))return 0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(admitted.session);++i)
        ((uint8_t *)&admitted.session)[i]=source[i];
    uint8_t digest[32];
    storage_digest_bytes(&admitted.session,sizeof(admitted.session),digest);
    if(memcmp(digest,&contract->reserved[2],32)||
       !pwm_session_valid(&admitted.session))return 0;
    admitted.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),contract->startup_deadline)!=1){
        admitted.ready=0;return 0;
    }
    return 1;
}
const struct pwm_session *pwm_admitted_session(void)
{return admitted.ready?&admitted.session:NULL;}
