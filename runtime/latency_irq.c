#include "sunstone_latency.h"

extern void __real_vApplicationIRQHandler(uint64_t interrupt);
void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
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
}
