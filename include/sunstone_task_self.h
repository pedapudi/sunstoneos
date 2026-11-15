#ifndef SUNSTONE_TASK_SELF_H
#define SUNSTONE_TASK_SELF_H
/* The calling task and one of its task-local pointers, from a single read of the current task.
 *
 * On the A733 SMP port the task register TPIDRRO_EL0 holds the running task's TCB in bits [47:0] (docs/smp.md), and
 * a task's own TCB does not change when it migrates, so one MRS names the task and a plain load reads its pointer: the
 * same load that pvTaskGetThreadLocalStoragePointer(NULL, index) makes. StaticTask_t has the TCB's layout, and the
 * kernel's task additions (platform/qemu-smp/freertos_tasks_c_additions.h, which the cached A733 additions include)
 * check that its pointer array has the TCB's offset; the fast path is compiled only in builds that include those
 * additions. While the register's task field is zero (before the core first resumes a task), and in every other
 * build, the result comes from the kernel's two calls.
 *
 * A pointer that another task writes needs the kernel's locked getter. The deadline runtime's pointer qualifies for the
 * plain load: only its own task writes it, once, before its first wait (runtime/deadline.c). */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

static inline __attribute__((always_inline)) TaskHandle_t sunstone_task_self_local(BaseType_t index, void **local)
{
#if defined(portTASK_REGISTER_TCB_MASK) && defined(configINCLUDE_FREERTOS_TASK_C_ADDITIONS_H) && \
    (configINCLUDE_FREERTOS_TASK_C_ADDITIONS_H == 1)
    StaticTask_t *self = (StaticTask_t *)(uintptr_t)(ullPortGetTaskRegister() & portTASK_REGISTER_TCB_MASK);
    if (self) {
        *local = self->pvDummy15[index];
        return (TaskHandle_t)self;
    }
#endif
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    *local = task ? pvTaskGetThreadLocalStoragePointer(NULL, index) : NULL;
    return task;
}
#endif
