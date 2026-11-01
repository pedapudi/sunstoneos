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
BaseType_t ssKernelObjectResume(void);
BaseType_t ssKernelOwnerIRQExit(void);
void ssKernelOwnerYield(void);
#ifdef SS_FREERTOS_OBJECT_TRANSLATION
/* Series B retains object metadata exclusion without stopping other owners.
 * Application critical/suspend APIs keep their separate global contract. */
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#undef taskENTER_CRITICAL_FROM_ISR
#undef taskEXIT_CRITICAL_FROM_ISR
#define taskENTER_CRITICAL() ssKernelObjectEnter()
#define taskEXIT_CRITICAL() ssKernelObjectExit()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISR()
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectExitFromISR(mask)
#define vTaskSuspendAll() ssKernelObjectSuspend()
#define xTaskResumeAll() ssKernelObjectResume()
#endif
#endif
