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
#endif
