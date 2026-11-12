#ifndef SS_PARTITIONED_QEMU_CONFIG_H
#define SS_PARTITIONED_QEMU_CONFIG_H
#include "../../platform/qemu-smp/FreeRTOSConfig.h"
#ifdef SS_QEMU_DYNAMIC_LIFECYCLE
#undef configSUPPORT_DYNAMIC_ALLOCATION
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE (128U * 1024U)
#endif
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskAbortDelay 1
#define configUSE_TASK_PREEMPTION_DISABLE 1
/* Measurement: list integrity values on every list operation (-DSUNSTONE_LIST_CHECK=1). */
#ifdef SUNSTONE_LIST_CHECK
#define configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES 1
#endif
/* The object-lock check runs a hook inside every successful queue send, within the queue's critical section. */
#if defined(SS_OBJECT_HOOK) && !defined(__ASSEMBLER__)
extern void ss_object_hook(void *queue);
#define traceQUEUE_SEND(pxQueue) ss_object_hook((void *)(pxQueue))
#endif
#endif
