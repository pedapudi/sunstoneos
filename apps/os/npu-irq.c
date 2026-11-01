/* INTID 97 on core 1; callbacks and the bind are platform/a733/npu_irq.c's. */
#include "npu.h"
#ifdef SUNSTONE_A733_OS_NPU_OBSERVE
#define NPU_IRQ_CAPTURE_TICKS 1
#endif
#define NPU_IRQ_CORE OS_NPU_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER os_npu_irq
#define NPU_IRQ_EXTERNAL_BIND os_npu_irq_bind
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
#define NPU_IRQ_IDLE_HINT 0
#endif
#include "../../platform/a733/npu_irq.c"
