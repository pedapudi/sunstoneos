/* INTID 97 on core 0 at priority 0xb0 for the NPU runtime finite image: the
 * single-core binding of platform/a733/npu_irq.c with the runtime's bind
 * function, npu_finite_runtime_irq_bind. The handler stays
 * npu_application_irq, which the image's private route (irq-route.c) calls. */
#include "FreeRTOS.h"
#include "npu_runtime.h"
/* The finite qualification image requires actual ISR notifications. The
 * shared runtime's optional idle hint can otherwise count a completion even
 * when the GIC never delivered it. Keep that fallback out of this image. */
static int irq_only_bind(struct npu_runtime *rt, const struct npu_service_irq_ops *ops,
    void *cookie, UBaseType_t route, unsigned idle_hint)
{
    (void)idle_hint;
    return npu_runtime_bind_irq(rt, ops, cookie, route, 0);
}
#define npu_runtime_bind_irq irq_only_bind
#define NPU_IRQ_EXTERNAL_BIND npu_finite_runtime_irq_bind
#define NPU_IRQ_QUIET_QUERY npu_finite_runtime_irq_quiet
#include "../../platform/a733/npu_irq.c"
#undef npu_runtime_bind_irq
_Static_assert((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)<=NPU_IRQ_PRIORITY,
    "NPU priority must permit FreeRTOS FromISR calls");
