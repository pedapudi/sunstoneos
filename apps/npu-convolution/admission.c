#include "session.h"
#include "boot.h"
#include "cached.h"
#include "result.h"
#include "a733_storage_digest.h"
#include <string.h>
static struct npu_session session;
static unsigned copied,authenticated;
static uint8_t authenticated_digest[32];
const uint64_t a733_npu_session_layout[8]={sizeof(struct npu_session),
    offsetof(struct npu_session,boot),sizeof(struct a733_npu_application_boot),
    offsetof(struct npu_session,pll),sizeof(struct a733_npu_pll_config),
    offsetof(struct a733_npu_application_boot,npu),offsetof(struct a733_npu_application_boot,pmic),
    offsetof(struct a733_npu_application_boot,pins)};
static int present(const uint8_t p[32])
{unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int npu_session_valid(const struct npu_session *s)
{
    if(!s || s->magic!=NPU_SESSION_MAGIC || s->version!=1 || s->bytes!=sizeof(*s) ||
       !s->epoch || !s->generation || !s->lease || !s->supervisor || !s->whole_platform ||
       !present(s->image) || !present(s->evidence) || !present(s->clock) ||
       !present(s->power) || !present(s->supervision))return 0;
    const struct a733_npu_application_boot *b=&s->boot;
    const struct a733_npu_application_profile *p=&b->profile;
    if(p->epoch!=s->epoch || p->generation!=s->generation || p->lease!=s->lease ||
       !p->npu_evidence || !p->command_admission || !p->usb_evidence || !p->pmic_evidence ||
       !p->pin_evidence || !p->npu_owner || !p->pmic_owner || !p->usb_owner || !p->bridge_owner ||
       p->npu_owner==p->pmic_owner || p->npu_owner==p->usb_owner || p->npu_owner==p->bridge_owner ||
       p->pmic_owner==p->usb_owner || p->pmic_owner==p->bridge_owner || p->usb_owner==p->bridge_owner ||
       p->npu_established!=A733_NPU_HANDOFF_SUPERVISED_REQUIRED ||
       p->usb_established!=A733_NPU_APPLICATION_USB_SUPERVISED ||
       b->npu.owner!=p->npu_owner || b->npu.handoff!=p->npu_evidence || b->npu.deadline_us ||
       b->pmic.owner!=p->pmic_owner || b->pmic.identity!=p->pmic_evidence || b->pmic.startup_deadline ||
       s->pll.mode!=A733_NPU_PLL_ENABLED_RETUNE || s->pll.deadline_us ||
       s->pll.owner!=p->npu_owner || s->pll.handoff!=p->npu_evidence)return 0;
    struct a733_npu_pll_config pll=s->pll;pll.deadline_us=1;
    struct a733_npu_platform_config local=b->npu;local.deadline_us=1;
    a733_clock_mask_t shared,exclusive,local_shared;
    if(!a733_npu_pll_scopes(&pll,&shared,&exclusive) ||
       !a733_npu_platform_scopes(&local,&local_shared) ||
       (shared|A733_CLOCK_PLL_NPU)!=local_shared)return 0;
    const uint32_t masks[]={0x0300001f,0x8700001f,0x8700001f};
    for(unsigned i=0;i<3;++i)if((pll.consumers[i]&masks[i])!=local.fabric[i])return 0;
    return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct npu_session *s=(const void *)NPU_SESSION_ADDRESS;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12)return 0;
    uint8_t digest[32];
    if(!c || c->magic!=NPU_CONTRACT_MAGIC || c->version!=3 || c->bytes!=sizeof(*c) ||
       !c->epoch || !c->generation || c->inverse_generation!=~c->generation || !c->lease ||
       memcmp(c->build_id,a733_smp_build_id,32) || !c->reserved[0] || c->reserved[1])return 0;
    storage_digest_bytes(s,sizeof(*s),digest);
    if(memcmp(digest,&c->reserved[2],32) || !npu_session_valid(s) ||
       memcmp(s->image,a733_smp_build_id,32) || c->epoch!=s->epoch || c->generation!=s->generation ||
       c->lease!=s->lease || c->reserved[0]!=s->supervisor || c->armed_at>now ||
       c->armed_at>UINT64_MAX-NPU_WINDOW_TICKS ||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ || now>=c->startup_deadline ||
       c->diagnostic_deadline!=c->armed_at+NPU_WINDOW_TICKS || c->expires_at!=c->diagnostic_deadline)return 0;
    if(c->watchdog_mode || c->watchdog_configuration || c->watchdog_output ||
       c->fel_flag || c->redirect_flag || c->redirect_entry || c->alternate_flag)return 0;
    /* EL3 boot publishes this receipt before the existing cache-adoption and
     * eight-core barriers. EL1 never reads the protected monitor contract. */
    memcpy(authenticated_digest,digest,32);authenticated=1;return 1;
}
int npu_session_copy(void)
{
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
