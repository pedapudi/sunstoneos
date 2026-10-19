#ifndef SUNSTONE_NPU_IRQ_ROUTE_H
#define SUNSTONE_NPU_IRQ_ROUTE_H
#include <stdint.h>

/* EL3-only saved state, embedded in the monitor's protected storage. Only the
 * fields owned by INTID97 are captured; adjacent interrupt fields stay live. */
struct npu_irq_route_snapshot {
    uint64_t route;
    uint32_t group, modifier, configuration, priority, phase;
};
int npu_irq_route_capture(struct npu_irq_route_snapshot *, uint64_t deadline);
int npu_irq_route_install(struct npu_irq_route_snapshot *, uint64_t deadline);
int npu_irq_route_restore(struct npu_irq_route_snapshot *, uint64_t deadline);
#endif
