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
/* The live kernel level-1 entries for slots 0 to 15. OS platforms read through
 * their protected EL1 table alias; the monitor's identity view remains invalid.
 * Every slot from 16 up is zero. */
#define OS_KERNEL_L1_HEAD 16
void os_kernel_l1_head(uint64_t out[OS_KERNEL_L1_HEAD]);
/* The 512 live level-2 entries of level-1 slot 1 (0x40000000 to 0x80000000), copied the same way, so that a space can own
 * blocks above the 128 MiB that EL0 reaches in every space (app_space_build_tables). */
#define OS_KERNEL_L2_ENTRIES 512
void os_kernel_l2_slot1(uint64_t out[OS_KERNEL_L2_ENTRIES]);
/* The main task's stack when the OS took it from the application memory ledger with an unmapped guard block below it
 * (apps/os/large.c), or null when the image keeps its static stack. */
void *os_large_main_stack(void);
static inline void os_space_switch_in(const void *tag)
{
    if (!os_space_active) return;
    uint64_t want = tag ? *(const uint64_t *)tag : os_space_kernel_ttbr, have;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(have));
    if (have != want) __asm__ volatile("msr ttbr0_el1, %0" :: "r"(want) : "memory");
}

/* The space of a new task is an explicit argument of the OS-side creation call (apps/os/os-task.h): os_task_create_static_affinity_set
 * and os_task_create_static take the space's tag, the kernel's callers pass null (the kernel's table), and the kit's
 * ss_task_create_static_affinity_set (include/sunstone_app.h) passes the calling application's. The creation call names the space
 * in os_space_explicit_space for its core, and traceTASK_CREATE, which runs inside the creation before the scheduler publishes the
 * task to the core that owns it, gives the task exactly that space. A tag set after the creation call returns would leave a window
 * in which the partitioned scheduler's owner core can switch the task in with its creator's tag: an OS task created by an
 * application's task would then start in that application's table.
 *
 * traceTASK_CREATE: a task that a running task creates directly, with a FreeRTOS call, takes its creator's tag, so a task that
 * an application's task creates runs in the application's space with no call of its own. Before the scheduler starts there is no
 * creating task, and nothing says which space a directly created task belongs to; the one place an application creates
 * tasks there is its init hook, and the OS sets os_space_app_init_active around that call. A direct FreeRTOS creation
 * inside the hook is refused with the failure OS_SPACE_REFUSED (os_space_refuse_create) instead of becoming a task of the
 * kernel's table; the creation calls above mark their own creation in os_space_explicit and are not refused. The hook
 * and the marks never decide a space: only the tag of a creating task or the argument of a creation call does. */
#define OS_SPACE_REFUSED 0x7a1u
extern volatile uint8_t os_space_app_init_active;
extern volatile uint8_t os_space_explicit[]; /* one byte per core, sized by configNUMBER_OF_CORES in apps/os/large.c */
extern const void *volatile os_space_explicit_space[]; /* the space that core's creation call names (null: the kernel's table) */
void os_space_refuse_create(void);
#define os_space_inherit(creator, created) \
    do { if (os_space_explicit[portGET_CORE_ID()]) \
             (created)->pxTaskTag = (__typeof__((created)->pxTaskTag))os_space_explicit_space[portGET_CORE_ID()]; \
         else if ((creator)) { \
             if (!(created)->pxTaskTag) (created)->pxTaskTag = (creator)->pxTaskTag; \
         } else if (os_space_app_init_active) \
             os_space_refuse_create(); } while (0)
#endif
