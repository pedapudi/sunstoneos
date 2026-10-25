/* INTID 97 on core 1; callbacks and the bind are apps/npu-interrupt/irq.c's. */
#include "npu.h"
#define NPU_IRQ_CORE OS_NPU_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER os_npu_irq
#define NPU_IRQ_EXTERNAL_BIND os_npu_irq_bind
#include "../npu-interrupt/irq.c"
