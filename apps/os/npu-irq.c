/* INTID 97 on core 1; callbacks and the bind are platform/a733/npu_irq.c's. */
#include "npu.h"
#ifdef SUNSTONE_A733_OS_NPU_OBSERVE
#define NPU_IRQ_CAPTURE_TICKS 1
#endif
#define NPU_IRQ_CORE OS_NPU_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER os_npu_irq
#define NPU_IRQ_EXTERNAL_BIND os_npu_irq_bind
/* The ring's producer submits from EL0 on this core, so the interrupt gives no idle hint under it. */
#define NPU_IRQ_IDLE_HINT (os_npu_queued()?0u:1u)
/* The route table holds the Secure-view priority; bind reads the Nonsecure view. */
#define NPU_IRQ_NS_PRIORITY ((NPU_IRQ_PRIORITY<<1)&0xffu)
#include "../../platform/a733/npu_irq.c"
/* PMR at Nonsecure EL1 with SCR_EL3.FIQ=1 is presented as (value>>1)|0x80, the
 * same transform as the Secure-view route priority, so the FreeRTOS mask
 * compares against the route priority after that conversion. */
_Static_assert(((((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)>>1)|0x80u))<=NPU_IRQ_PRIORITY,
    "NPU route priority must permit FreeRTOS FromISR calls");
