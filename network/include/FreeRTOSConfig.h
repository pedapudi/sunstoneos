#ifndef SUNSTONE_NETWORK_FREERTOS_CONFIG_H
#define SUNSTONE_NETWORK_FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include "../../include/FreeRTOSConfig.h"
#undef configSUPPORT_DYNAMIC_ALLOCATION
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE (256u * 1024u)
#define configAPPLICATION_ALLOCATED_HEAP 1
#define configUSE_MALLOC_FAILED_HOOK 0
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_vTaskDelete 1
#endif
