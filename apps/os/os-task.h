#ifndef SUNSTONE_OS_TASK_H
#define SUNSTONE_OS_TASK_H
/* OS-side task creation with the address space as an argument (include/os_space.h). space is the tag of an application's
 * space, or null for a task of the OS, which runs in the kernel's table whoever creates it. */
#include "FreeRTOS.h"
#include "task.h"
TaskHandle_t os_task_create_static_affinity_set(const void *space,TaskFunction_t entry,const char *name,uint32_t stack_words,
    void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *tcb,UBaseType_t affinity);
TaskHandle_t os_task_create_static(const void *space,TaskFunction_t entry,const char *name,uint32_t stack_words,
    void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *tcb);
#endif
