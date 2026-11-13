#include "sunstone_latency.h"
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#include "latency_trace.h"
extern volatile uint64_t ullPortInterruptNestings[];
#endif
#ifdef SUNSTONE_SCHED_PMU
#include "latency_pmu.h"
#endif

#ifdef SUNSTONE_LATENCY_CAUSE
#include "latency_cause.h"
#endif

extern void __real_vApplicationIRQHandler(uint64_t interrupt);
void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    uint64_t trace_core, trace_pc, trace_spsr;
    __asm__ volatile("mrs %0, tpidr_el1\n mrs %1, elr_el1\n mrs %2, spsr_el1"
                     : "=r"(trace_core), "=r"(trace_pc), "=r"(trace_spsr));
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_irq_begin((unsigned)trace_core, (unsigned)interrupt);
#endif
    const uint64_t dispatch = !trace_core && ullPortInterruptNestings[0] == 1 ?
        ss_sched_trace_dispatch_begin(0, (unsigned)interrupt, trace_pc, trace_spsr) : 0;
#endif
#ifdef SUNSTONE_LATENCY_PMU
    ss_latency_pmu_enable(); /* once per core, at EL1, before the span it first measures */
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    /* PMU setup precedes the existing tick timestamp. Only the outer CPU0
     * timer dispatch owns this window; nested interrupts preserve its capture. */
#ifdef SUNSTONE_SCHED_PMU
    unsigned char pmu_tick_scope;
    if (interrupt == 30 && !trace_core && ullPortInterruptNestings[0] == 1 &&
        ss_sched_pmu_prepare(0, SS_SCHED_PMU_TICK) == SS_SCHED_PMU_OK)
        (void)ss_sched_pmu_begin_scope(0, SS_SCHED_PMU_TICK, UINT32_MAX, 0, &pmu_tick_scope);
#endif
    if (interrupt == 30 && !trace_core) ss_sched_trace_tick_begin();
#ifdef SUNSTONE_LATENCY_CAUSE
    if (interrupt == 30 && !trace_core && ullPortInterruptNestings[0] == 1)
        ss_latency_cause_tick_begin();
#endif
#endif
    if (interrupt == 30) {
        uint64_t core, due;
        const uint64_t now = ss_latency_now();
        __asm__ volatile("mrs %0, tpidr_el1\n mrs %1, cntp_cval_el0"
                         : "=r"(core), "=r"(due));
        /* CNTVOFF_EL2 is zero in both platforms. Compare assertion is the
         * timer's IRQ condition, so timestamp before the handler changes it. */
        if (now >= due) {
            ss_latency_record_at((unsigned)core, SS_LAT_IRQ, due, now,
                                  (uintptr_t)__wrap_vApplicationIRQHandler);
            if (!core) ss_latency_tick_hook();
        }
    }
    __real_vApplicationIRQHandler(interrupt);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#ifdef SUNSTONE_SCHED_PMU
    if (interrupt == 30 && !trace_core && ullPortInterruptNestings[0] == 1)
        (void)ss_sched_pmu_end_scope(0, &pmu_tick_scope);
#endif
#ifdef SUNSTONE_LATENCY_CAUSE
    if (interrupt == 30 && !trace_core && ullPortInterruptNestings[0] == 1)
        ss_latency_cause_tick_end();
#endif
    if (interrupt == 30 && !trace_core) ss_sched_trace_tick_end();
    if (dispatch) ss_sched_trace_dispatch_end(0, dispatch);
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_irq_end((unsigned)trace_core);
#endif
#endif
}
