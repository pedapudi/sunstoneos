/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_OWNER_ADAPTER_H
#define SS_FREERTOS_OWNER_ADAPTER_H
/* Included after FreeRTOS.h. The public critical sections (vTaskEnterCritical and its FromISR form) take the two
 * global locks of the port; the queue, event group, stream buffer and timer sources use the per-object entries. */
void ssKernelObjectExit(void);
void ssKernelObjectSuspend(void);
BaseType_t ssKernelObjectResume(void);
UBaseType_t ssKernelObjectEnterFromISR(void);
void ssKernelObjectExitFromISR(UBaseType_t);
void ssKernelObjectEnterFor(const void *object, uintptr_t site);
/* Object sections: the lock is chosen by the object (see kernel/scheduler/freertos.c). */
void ssKernelObjectEnterOn(const void *object);
void ssKernelObjectLeave(void);
UBaseType_t ssKernelObjectEnterFromISROn(const void *object);
void ssKernelObjectLeaveFromISR(UBaseType_t);
UBaseType_t ssKernelObjectEnterWaitSlot(void);
void ssKernelObjectSuspendOn(const void *object);
BaseType_t ssKernelObjectResumeOn(void);
BaseType_t ssKernelOwnerIRQExit(void);
void ssKernelOwnerYield(void);
void ssKernelOwnerYieldTail(void);
#ifdef SS_FREERTOS_OBJECT_TRANSLATION
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#undef taskENTER_CRITICAL_FROM_ISR
#undef taskEXIT_CRITICAL_FROM_ISR
/* A section opened by name, where the source cannot use the translated macro. */
#define ssKernelObjectSectionEnter(object) ssKernelObjectEnterOn((const void *)(object))
/* Object sources lock the object they work on and nothing else, so operations on different objects do not wait
 * for each other. The public critical-section API takes the global locks and does not exclude them. */
#define taskENTER_CRITICAL() ssKernelObjectEnterOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL() ssKernelObjectLeave()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectLeaveFromISR(mask)
/* The timer service suspends the scheduler across timer callbacks, which may call any object's API, so its suspension
 * is not tied to one object: it takes the global task lock. */
#define ssTimerServiceSuspend() ssKernelObjectSuspend()
#define ssTimerServiceResume() ssKernelObjectResume()
/* The timer source's one-time initialisation creates the timer queue, and the queue's own section nests inside it, so
 * it takes the public critical section (the global locks), which orders before every object lock. */
#define ssTimerInitEnter() vTaskEnterCritical()
#define ssTimerInitExit() vTaskExitCritical()
#define vTaskSuspendAll() ssKernelObjectSuspendOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define xTaskResumeAll() ssKernelObjectResumeOn()
/* A walk of an object's waiter list. The owner of a waiter times it out, aborts it or detaches it under the slot's
 * interrupt-side lock alone, so a walk that reads an item's successor before it uses either holds that lock as well as
 * the task-side lock from the first read to the last use. */
#define ssKernelObjectWalkEnter() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define ssKernelObjectWalkLeave(mask) ssKernelObjectLeaveFromISR(mask)
/* Host models delay here, between reading a waiter-list item and its successor and using them (perturb.h). */
#include "perturb.h"
#define ssKernelObjectWalkStep() SS_KERNEL_PERTURB(SS_PERTURB_OBJECT_WALK)
/* A member queue would wake a queue set's waiters under the member's slot, while the set's waiters are timed out under
 * the set's slot. */
#if ( configUSE_QUEUE_SETS == 1 )
#error "queue sets need the set's slot lock held where a member queue wakes the set's waiters"
#endif
#endif
#endif
