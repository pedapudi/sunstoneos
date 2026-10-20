#include "session.h"
static int present(const uint8_t p[32])
{unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int npu_session_valid(const struct npu_session *s)
{
    if(!s || s->magic!=NPU_SESSION_MAGIC || s->version!=NPU_SESSION_VERSION || s->bytes!=sizeof(*s) ||
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
       (s->pll.mode!=A733_NPU_PLL_ENABLED_RETUNE && s->pll.mode!=A733_NPU_PLL_LIVE_ENABLE) || s->pll.deadline_us ||
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
