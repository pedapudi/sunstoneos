/* SPDX-License-Identifier: GPL-2.0-only */
/* Image-side hooks of the stage 3a cap image, linked with --wrap (cmake/a733-os.cmake).
 * - The CPU0 task that accesses the AXI2HIF bridges runs at EL1, as the table-walks and interconnect studies do. The
 *   application keeps the public OS ABI.
 * - Every way out of the image clears the cap's enable word, +0x30, on both bridges first, whatever the run
 *   reached, so a failure in a capped sub-phase cannot leave the cap set. Two plain stores between DSBs, with no gate
 *   read: DRAM0's clock and bus gate are open whenever code runs from DRAM, and no code in this image changes the path
 *   select. The stores sit in front of the two exits:
 *   - the EL1 side of the success terminal (apps/latency/axi-exit.S), which every clean return and every failure
 *     return the OS claims reaches through SVC #0x735. The terminal tasks call a733_qualification_worker_done at
 *     EL0, so no store can sit there; a first board run that read CurrentEL there faulted at EL0
 *     (entry 1794750406816673707-79429d7a);
 *   - a733_storage_maintenance_stop, which a733_smp_exit_privileged calls at EL1 with interrupts masked for every
 *     other exit (the SVC exit of platform_exit and platform_panic, the deadline and unexpected-interrupt exits).
 *   The EL3 fault path calls the second hook too; at EL3 the stores are skipped.
 * This unit is reachable from the EL1 vectors and is compiled with -mgeneral-regs-only. */
#include "axi-cap.h"
#include <stdint.h>
extern int __real_os_application_task_privileged(void (*)(void *), void *);
int __wrap_os_application_task_privileged(void (*entry)(void *), void *arg)
{
    return entry == latency_axi_worker || __real_os_application_task_privileged(entry, arg);
}
static void clear_cap(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (((el >> 2) & 3u) != 1u) return;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)UINT32_C(0x0a030030) = 0;
    *(volatile uint32_t *)(uintptr_t)UINT32_C(0x0a040030) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
}
extern void __real_a733_storage_maintenance_stop(unsigned status);
void __wrap_a733_storage_maintenance_stop(unsigned status)
{
    clear_cap();
    __real_a733_storage_maintenance_stop(status);
}
