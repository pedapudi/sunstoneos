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
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    const struct a733_npu_thermal_boot *t=&b->thermal;
    unsigned parent=t->apb1>>24,div=(t->apb1&31)+1;
    uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
    if(!t->owner || !t->authority || t->owner==p->npu_owner || t->owner==p->pmic_owner ||
       t->owner==p->usb_owner || t->owner==p->bridge_owner ||
       !present(t->initial) || !present(t->electrical) || !present(t->clocks) || !present(t->supervision) ||
       (t->apb1&~0x0300001fu) || !hz || hz%div || t->apb1_hz!=hz/div || t->configure_adc_clock>1)return 0;
    for(unsigned i=0;i<sizeof(t->reserved);++i)if(t->reserved[i])return 0;
#endif
    a733_clock_mask_t shared;
    return p->npu_evidence && p->command_admission && p->usb_evidence && p->pmic_evidence &&
       p->pin_evidence && p->npu_owner && p->pmic_owner && p->usb_owner && p->bridge_owner &&
       p->npu_owner!=p->pmic_owner && p->npu_owner!=p->usb_owner && p->npu_owner!=p->bridge_owner &&
       p->pmic_owner!=p->usb_owner && p->pmic_owner!=p->bridge_owner && p->usb_owner!=p->bridge_owner &&
       b->pmic.owner==p->pmic_owner && b->pmic.identity==p->pmic_evidence && !b->pmic.startup_deadline &&
       a733_npu_platform_scopes(&b->npu,&shared);
}
