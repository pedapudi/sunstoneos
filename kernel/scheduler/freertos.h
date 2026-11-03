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
void ssKernelObjectEnterOnGlobal(const void *object);
UBaseType_t ssKernelObjectEnterFromISROnGlobal(const void *object);
void ssKernelObjectSuspendOnGlobal(const void *object);
BaseType_t ssKernelObjectResume(void);
BaseType_t ssKernelOwnerIRQExit(void);
void ssKernelOwnerYield(void);
void ssKernelOwnerYieldTail(void);
#ifdef SS_FREERTOS_OBJECT_TRANSLATION
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#undef taskENTER_CRITICAL_FROM_ISR
#undef taskEXIT_CRITICAL_FROM_ISR
#ifdef SS_FREERTOS_OBJECT_GLOBAL
/* The timer source keeps the global locks: its sections enclose queue creation and queue sections, so they sit
 * outside the object locks in the lock order. */
#define taskENTER_CRITICAL() ssKernelObjectEnter()
#define taskEXIT_CRITICAL() ssKernelObjectExit()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISR()
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectExitFromISR(mask)
#define vTaskSuspendAll() ssKernelObjectSuspend()
#define xTaskResumeAll() ssKernelObjectResume()
#elif defined(SS_OBJECT_LOCKS_GLOBAL)
/* Control build (SUNSTONE_OBJECT_LOCKS=global): object sources take the two global locks, as they did before the
 * object locks, and name their object for the lock trace. */
#define taskENTER_CRITICAL() ssKernelObjectEnterOnGlobal((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL() ssKernelObjectExit()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISROnGlobal((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectExitFromISR(mask)
#define vTaskSuspendAll() ssKernelObjectSuspendOnGlobal((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define xTaskResumeAll() ssKernelObjectResume()
#else
/* Object sources lock the object they work on and nothing else, so operations on different objects do not wait
 * for each other. The public critical-section API keeps its global locks and does not exclude them. */
#define taskENTER_CRITICAL() ssKernelObjectEnterOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL() ssKernelObjectLeave()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectLeaveFromISR(mask)
#define vTaskSuspendAll() ssKernelObjectSuspendOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define xTaskResumeAll() ssKernelObjectResumeOn()
#endif
#endif
#endif
