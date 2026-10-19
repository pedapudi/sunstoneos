#ifndef SUNSTONE_PCIE_TEST_IRQ_H
#define SUNSTONE_PCIE_TEST_IRQ_H
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
/* Bind once before scheduler start. Later calls belong to that privileged task.
 * EL3 must establish disabled, inactive, nonpending Group1NS level SPI185. */
int pcie_test_irq_bind(TaskHandle_t owner);
/* 0: RWP pending; 1: armed; -1: refused. Only pending propagation may be
 * polled within the caller's fixed deadline. No arm after an event or stop. */
int pcie_test_irq_arm(void);
/* 0: no event; 1: one event; -1: fault. An event does not establish DMA
 * completion or MSI target visibility; the endpoint transaction checks those. */
int pcie_test_irq_event(uint32_t *notifications);
/* Permanent closure. Success means disable issued, not propagated, and never
 * establishes endpoint shutdown, drained DMA or reusable memory. */
int pcie_test_irq_stop(void);
/* Dispatcher owns acknowledge/EOI/deactivation. No controller/provider/mutex
 * access occurs in this handler. The owner polls the published event. */
void pcie_test_application_irq(void);
#endif
