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
#endif
