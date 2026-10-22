#include "session.h"
#include "boot.h"
#include "cached.h"
#include "result.h"
#include "a733_range.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_NPU_SINGLE_CORE
#define RETAINED __attribute__((section(".npu_runtime_retained")))
#else
#define RETAINED
#endif
static struct npu_session session RETAINED;
static unsigned copied RETAINED,authenticated RETAINED;
static uint8_t authenticated_digest[32] RETAINED;
const uint64_t a733_npu_session_layout[8]={sizeof(struct npu_session),
    offsetof(struct npu_session,boot),sizeof(struct a733_npu_application_boot),
    offsetof(struct npu_session,pll),sizeof(struct a733_npu_pll_config),
    offsetof(struct a733_npu_application_boot,npu),offsetof(struct a733_npu_application_boot,pmic),
    offsetof(struct a733_npu_application_boot,pins)};
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct npu_session *s=(const void *)NPU_SESSION_ADDRESS;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12)return 0;
    uint8_t digest[32];uint64_t window;
    if(!c || c->magic!=NPU_CONTRACT_MAGIC || c->version!=3 || c->bytes!=sizeof(*c) ||
       !c->epoch || !c->generation || c->inverse_generation!=~c->generation || !c->lease ||
       memcmp(c->build_id,a733_smp_build_id,32) || !c->reserved[0] || c->reserved[1])return 0;
    storage_digest_bytes(s,sizeof(*s),digest);
    if(memcmp(digest,&c->reserved[2],32) || !npu_session_valid(s) ||
       memcmp(s->image,a733_smp_build_id,32) || c->epoch!=s->epoch || c->generation!=s->generation ||
       c->lease!=s->lease || c->reserved[0]!=s->supervisor || c->armed_at>now ||
       !a733_deadline_after(c->armed_at,NPU_WINDOW_TICKS,&window) ||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ || now>=c->startup_deadline ||
       c->diagnostic_deadline!=window || c->expires_at!=c->diagnostic_deadline)return 0;
    if(c->watchdog_mode || c->watchdog_configuration || c->watchdog_output ||
       c->fel_flag || c->redirect_flag || c->redirect_entry || c->alternate_flag)return 0;
    /* EL3 boot publishes this receipt before the existing cache-adoption and
     * eight-core barriers. EL1 never reads the protected monitor contract. */
    memcpy(authenticated_digest,digest,32);authenticated=1;return 1;
}
#ifdef SUNSTONE_NPU_SINGLE_CORE
int npu_single_core_seal(const struct a733_smp_contract *c)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12||copied||!authenticated)return 0;
    memcpy(&session,(const void *)NPU_SESSION_ADDRESS,sizeof(session));
    uint8_t digest[32];storage_digest_bytes(&session,sizeof(session),digest);
    if(memcmp(digest,authenticated_digest,32)||!npu_session_valid(&session)||
       memcmp(session.image,a733_smp_build_id,32)||session.epoch!=c->epoch||
       session.generation!=c->generation||session.lease!=c->lease)return 0;
    copied=1;
    /* EL3 is cache disabled; DSB publishes the retained bytes before EL1 adopts WB. */
    __asm__ volatile("dsb sy":::"memory");return 1;
}
#endif
int npu_session_copy(void)
{
#ifdef SUNSTONE_NPU_SINGLE_CORE
    return copied&&a733_smp_device_preparation_admitted();
#endif
    if(copied || !authenticated || !a733_smp_device_preparation_admitted())return 0;
    memcpy(&session,(const void *)NPU_SESSION_ADDRESS,sizeof(session));
    uint8_t digest[32];storage_digest_bytes(&session,sizeof(session),digest);
    if(memcmp(digest,authenticated_digest,32) || !npu_session_valid(&session) ||
       memcmp(session.image,a733_smp_build_id,32) ||
       session.epoch!=a733_observe_identity.epoch || session.generation!=a733_observe_identity.generation ||
       session.lease!=a733_observe_identity.lease)return 0;
    copied=1;return 1;
}
const struct npu_session *npu_session_get(void){return copied ? &session : NULL;}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1 || core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg || address==cfg+4 || address==cfg+8 || address==UINT32_C(0x07050200)+core*4 ||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write && address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
