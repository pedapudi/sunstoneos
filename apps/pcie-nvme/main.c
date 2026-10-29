#include "pcie.h"
#include "snapshot.h"
#include "boot.h"
#if SUNSTONE_PCIE_NVME_MSIX
#include "irq.h"
#endif
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==pcie_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct pcie_session *s=pcie_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pins->authorized[A733_PIN_D]|=UINT32_C(15)<<20;return 1;
}
int board_application_start(void)
{
 const struct pcie_session *s=pcie_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pcie.session=*s;
 if(!pcie_session_valid(&pcie.session)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 pcie.handle=xTaskCreateStatic(pcie_task,"pcie",2048,NULL,4,pcie.stack,&pcie.task);
#if SUNSTONE_PCIE_NVME_MSIX
 if(!pcie_nvme_irq_bind(pcie.handle))return 0;
#endif
 return pcie.handle&&pcie_construct();
}
