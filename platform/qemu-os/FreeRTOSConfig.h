#ifndef SUNSTONE_QEMU_OS_CONFIG_H
#define SUNSTONE_QEMU_OS_CONFIG_H
/* Kernel configuration of the virtual eight-core OS board. It has the
 * A733 image's scheduler settings (platform/a733-smp-cached/FreeRTOSConfig.h):
 * eight cores, core affinity, default placement on the first six cores and an
 * idle task that waits in WFI. The interrupt controller is the QEMU virt GICv3. */
#include "../../include/FreeRTOSConfig.h"
#define configNUMBER_OF_CORES 8
#define configUSE_CORE_AFFINITY 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configENABLE_MPU 0
#define configUSE_PASSIVE_IDLE_HOOK 1
#define configIDLE_AFFINITY 1
#include "../../include/a733_smp_placement.h"
#define configTASK_DEFAULT_CORE_AFFINITY A733_SMP_A55_MASK
#define configCORES_SHARE_TYPE(a, b) \
    ((((A733_SMP_A55_MASK >> (a)) ^ (A733_SMP_A55_MASK >> (b))) & 1u) == 0u)
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS 0x080b0000UL
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET 0
#define INCLUDE_vTaskDelete 1
#define INCLUDE_xSemaphoreGetMutexHolder 1
#ifndef INCLUDE_xTaskGetSchedulerState
#define INCLUDE_xTaskGetSchedulerState 1
#endif
#ifndef INCLUDE_xTaskGetCurrentTaskHandle
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#endif
#ifndef __ASSEMBLER__
#include "../../include/a733_cpu_power.h"
#define vApplicationPassiveIdleHook a733_cpu_idle
#endif
/* The board application's headers include the network stack's, which adds
 * its own requirements (apps/board/FreeRTOSConfig.h does the same). */
#include "../../network/include/FreeRTOSConfig.h"
#endif
