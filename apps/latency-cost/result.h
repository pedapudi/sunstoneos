#ifndef LATENCY_COST_RESULT_H
#define LATENCY_COST_RESULT_H
#include <stdint.h>
#define COST_SAMPLES 48u
#define COST_ROUNDTRIPS 32u
#define COST_IRQ_SAMPLES 96u
#define COST_MAGIC UINT64_C(0x53534c434f535431)
struct cost_core {
    uint64_t ticks[COST_SAMPLES];
    uint64_t switches[COST_SAMPLES];
    uint64_t timer_mask;
};
struct cost_result {
    uint64_t magic, frequency;
    uint32_t enabled, samples, roundtrips, core_mask;
    uint64_t counter_pair_min;
    struct cost_core core[8];
    uint64_t irq_ticks[COST_IRQ_SAMPLES], irq_os_ticks[COST_IRQ_SAMPLES];
};
_Static_assert(sizeof(struct cost_result) == 7784, "cost report layout");
/* Only CPU0's IRQ writer accesses samples until acknowledged is published. */
struct cost_irq_control {
    unsigned enabled, acknowledged, count;
    uint64_t ticks[COST_IRQ_SAMPLES], os_ticks[COST_IRQ_SAMPLES];
};
extern struct cost_irq_control latency_cost_irq;
#endif
