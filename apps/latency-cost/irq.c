/* This wrapper belongs only to the diagnostic image. Its inner enabled
 * wrapper is the unchanged recorder source with a private symbol name. */
#include "FreeRTOS.h"
#include "task.h"
#include "result.h"
extern void __real_vApplicationIRQHandler(uint64_t interrupt);
#ifdef SUNSTONE_LATENCY
extern void latency_cost_inner_irq(uint64_t interrupt);
#define INNER latency_cost_inner_irq
#else
#define INNER __real_vApplicationIRQHandler
#endif
static uint64_t now(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    if (interrupt != 30 || portGET_CORE_ID() != 0) { INNER(interrupt); return; }
    const unsigned active = __atomic_load_n(&latency_cost_irq.enabled, __ATOMIC_ACQUIRE);
    if (!active) {
        INNER(interrupt);
        __atomic_store_n(&latency_cost_irq.acknowledged, 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&latency_cost_irq.acknowledged, 0, __ATOMIC_RELAXED);
    const unsigned i = __atomic_load_n(&latency_cost_irq.count, __ATOMIC_RELAXED);
    if (i >= COST_IRQ_SAMPLES) { INNER(interrupt); return; }
    const TickType_t tick = xTaskGetTickCount();
    const uint64_t start = now();
    INNER(interrupt);
    const uint64_t elapsed = now() - start;
    latency_cost_irq.ticks[i] = elapsed;
    latency_cost_irq.os_ticks[i] = (TickType_t)(xTaskGetTickCount() - tick);
    __atomic_store_n(&latency_cost_irq.count, i + 1, __ATOMIC_RELEASE);
}
