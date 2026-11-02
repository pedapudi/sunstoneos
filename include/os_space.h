#ifndef SUNSTONE_OS_SPACE_H
#define SUNSTONE_OS_SPACE_H
/* Per-application address spaces at the task switch (docs/designs/large-app-regions.md).
 *
 * traceTASK_SWITCHED_IN() runs inside the context switch with interrupts masked, so this is the least that can
 * be done there: compare the core's TTBR0 with the incoming task's and write it when they differ. The task
 * carries its space's TTBR0 value in its application tag; an untagged task runs in the kernel's table. Every
 * upper-memory entry carries an ASID (nG), so no TLB maintenance is needed. No barrier follows the write:
 * the kernel's own mappings are identical in every space, and the exception return that resumes the task is a
 * context synchronization event, after which the new table applies to the task's EL0 accesses.
 *
 * Until a space is installed os_space_active is zero and the hook is one load and one branch. */
#include <stdint.h>
extern uint64_t os_space_kernel_ttbr;      /* the table every core installed at boot (ASID 0) */
extern uint8_t os_space_active;
static inline void os_space_switch_in(const void *tag)
{
    if (!os_space_active) return;
    uint64_t want = tag ? *(const uint64_t *)tag : os_space_kernel_ttbr, have;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(have));
    if (have != want) __asm__ volatile("msr ttbr0_el1, %0" :: "r"(want) : "memory");
}

/* traceTASK_CREATE: the new task takes the creator's tag, so a task that the application creates runs in the
 * application's space with no call of its own. The creator is the task running on the creating core, which is
 * absent (null) while boot code creates tasks before the scheduler starts. A macro, because the task control
 * block is private to the kernel files that expand it. A task of the OS is created from boot code or from an
 * OS task with no tag, and inherits none. */
#define os_space_inherit(creator, created) \
    do { if ((creator) && !(created)->pxTaskTag) (created)->pxTaskTag = (creator)->pxTaskTag; } while (0)
#endif
