#ifndef SUNSTONE_NPU_APPLICATION_IRQ_H
#define SUNSTONE_NPU_APPLICATION_IRQ_H
struct npu_service;
/* Bind once, after service construction and before starting the scheduler. */
int npu_application_irq_bind(struct npu_service *);
/* Called only for INTID97; the SRE port owns acknowledge and combined EOI. */
void npu_application_irq(void);
#endif
