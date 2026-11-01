/* INTID 97 on core 0 at priority 0xb0 for the NPU runtime finite image: the
 * single-core binding of platform/a733/npu_irq.c with the runtime's bind
 * function, npu_finite_runtime_irq_bind. The handler stays
 * npu_application_irq, which the image's private route (irq-route.c) calls. */
#include "FreeRTOS.h"
#define NPU_IRQ_EXTERNAL_BIND npu_finite_runtime_irq_bind
#include "../../platform/a733/npu_irq.c"
_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=NPU_IRQ_PRIORITY,
    "NPU priority must permit FreeRTOS FromISR calls");
