/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_OWNER_ADAPTER_H
#define SS_FREERTOS_OWNER_ADAPTER_H
/* Included after FreeRTOS.h. The public application critical functions retain
 * their compatibility locks; only kernel object sources use these entries. */
void ssKernelObjectEnter(void);
void ssKernelObjectExit(void);
UBaseType_t ssKernelObjectEnterFromISR(void);
void ssKernelObjectExitFromISR(UBaseType_t);
void ssKernelObjectSuspend(void);
void ssKernelObjectEnterFor(const void *object, uintptr_t site);
/* Object sections: the lock is chosen by the object (see kernel/scheduler/freertos.c). */
void ssKernelObjectEnterOn(const void *object);
void ssKernelObjectLeave(void);
UBaseType_t ssKernelObjectEnterFromISROn(const void *object);
void ssKernelObjectLeaveFromISR(UBaseType_t);
void ssKernelObjectSuspendOn(const void *object);
BaseType_t ssKernelObjectResumeOn(void);
BaseType_t ssKernelObjectResume(void);
BaseType_t ssKernelOwnerIRQExit(void);
void ssKernelOwnerYield(void);
void ssKernelOwnerYieldTail(void);
#ifdef SS_FREERTOS_OBJECT_TRANSLATION
/* Object sources lock the object they work on and nothing else, so operations on different objects do not wait
 * for each other. The public critical-section API keeps its global locks and does not exclude them. */
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#undef taskENTER_CRITICAL_FROM_ISR
#undef taskEXIT_CRITICAL_FROM_ISR
#define taskENTER_CRITICAL() ssKernelObjectEnterOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL() ssKernelObjectLeave()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectLeaveFromISR(mask)
#define vTaskSuspendAll() ssKernelObjectSuspendOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define xTaskResumeAll() ssKernelObjectResumeOn()
#endif
#endif
