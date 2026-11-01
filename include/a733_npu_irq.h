#ifndef A733_NPU_IRQ_H
#define A733_NPU_IRQ_H
#include <stdint.h>
/* The single-core NPU interrupt binding, platform/a733/npu_irq.c. */
struct npu_service;
/* Bind once, after service construction and before starting the scheduler. */
int npu_application_irq_bind(struct npu_service *);
/* Optional service restart callback, worker only with the kernel critical
 * section held. It keeps the stopped source masked and never clears pending. */
int npu_application_irq_restart(void *);
/* Called only for INTID97; the SRE port owns acknowledge and combined EOI. */
void npu_application_irq(void);
/* System counter ticks at the latest INTID97 delivery; 0 before the first. */
uint64_t npu_application_irq_ticks(void);
/* Actual ISR deliveries; enabled with the finite timestamp instrumentation. */
unsigned npu_application_irq_count(void);
#endif
