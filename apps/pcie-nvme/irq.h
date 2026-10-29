#ifndef SUNSTONE_PCIE_NVME_IRQ_H
#define SUNSTONE_PCIE_NVME_IRQ_H
#include "a733_pcie_function.h"
#include "FreeRTOS.h"
#include "task.h"
/* Fixed MSI vector zero on Group1NS level INTID185. Bind before scheduling.
 * IRQ is a wake hint; only the NVMe CQ establishes command completion. */
int pcie_nvme_irq_bind(TaskHandle_t);
int pcie_nvme_irq_health(void);
extern const struct a733_pcie_irq_ops pcie_nvme_irq_ops;
void pcie_nvme_application_irq(void);
#endif
