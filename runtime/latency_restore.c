/* SPDX-License-Identifier: Apache-2.0 */
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_latency.h"
#include <stdbool.h>

void __real_ss_latency_mask_close(unsigned core, uintptr_t site);
void __wrap_ss_latency_mask_close(unsigned core, uintptr_t site)
{
    /* An EL0 unmask uses SVC. Keep its earlier interval until that return;
     * never read privileged state while the task register identifies EL0. */
#ifdef SS_RESTORE_SPAN_TEST
    extern bool ss_restore_test_el1(void);
    extern uint64_t ss_restore_test_daif(void);
    if (!ss_restore_test_el1()) return;
    const uint64_t flags = ss_restore_test_daif();
#else
    if (!((ullPortGetTaskRegister() >> portTASK_REGISTER_EL1_BIT) & 1U)) return;
    uint64_t flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
#endif
    if (!(flags & UINT64_C(0x80))) __real_ss_latency_mask_close(core, site);
}
void ss_latency_exception_enter(unsigned core)
{
    /* Existing open retains an earlier outer interval. */
    ss_latency_mask_open(core, ss_latency_now(), (uintptr_t)__builtin_return_address(0));
}
void ss_latency_exception_return(unsigned core, uint64_t spsr, uint64_t pmr)
{
    if (!(spsr & UINT64_C(0x80)) && pmr > portMAX_API_PRIORITY_MASK)
        __real_ss_latency_mask_close(core, (uintptr_t)__builtin_return_address(0));
}
