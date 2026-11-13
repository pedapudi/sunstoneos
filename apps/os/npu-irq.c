/* INTID 97 on core 1; callbacks and the bind are platform/a733/npu_irq.c's. */
#include "npu.h"
/* The delivery counter and count are kept only under the diagnostic observer (apps/os/npu-observe.c), set before scheduling. */
#define NPU_IRQ_CAPTURE_TICKS (binding.service&&binding.service->observe)
#define NPU_IRQ_CORE OS_NPU_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER os_npu_irq
#define NPU_IRQ_EXTERNAL_BIND os_npu_irq_bind
/* The ring's producer submits from EL0 on this core, so the interrupt gives no idle hint under it. */
#define NPU_IRQ_IDLE_HINT 0u
/* The route table holds the Secure-view priority; bind reads the Nonsecure view. */
#define NPU_IRQ_NS_PRIORITY ((NPU_IRQ_PRIORITY<<1)&0xffu)
#include "../../platform/a733/npu_irq.c"
/* PMR at Nonsecure EL1 with SCR_EL3.FIQ=1 is presented as (value>>1)|0x80, the
 * same transform as the Secure-view route priority, so the FreeRTOS mask
 * compares against the route priority after that conversion. */
_Static_assert(((((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)>>1)|0x80u))<=NPU_IRQ_PRIORITY,
    "NPU route priority must permit FreeRTOS FromISR calls");
