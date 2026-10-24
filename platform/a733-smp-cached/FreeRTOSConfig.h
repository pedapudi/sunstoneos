#ifndef SUNSTONE_A733_SMP_CONFIG_H
#define SUNSTONE_A733_SMP_CONFIG_H
#include "../../include/FreeRTOSConfig.h"
#define configNUMBER_OF_CORES 8
#define configUSE_CORE_AFFINITY 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configENABLE_MPU 0
/* Idle tasks wait in WFI (docs/cpu-power.md). Both idle loops call the
 * passive hook. Idle task n stays on core n, so the task that reclaims
 * deleted tasks runs on core 0, which the tick wakes every millisecond. */
#define configUSE_PASSIVE_IDLE_HOOK 1
#define configIDLE_AFFINITY 1
#ifdef SUNSTONE_A733_OS
/* The integrated OS image keeps every task created without an affinity on
 * the A55 cluster; only explicit placements reach the Cortex-A76 cores. */
#include "../../include/a733_smp_placement.h"
#define configTASK_DEFAULT_CORE_AFFINITY A733_SMP_A55_MASK
#endif
#define INCLUDE_vTaskDelete 1
/* The patched port obtains the primary redistributor from discovery. */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS 0
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET 0
#ifdef SUNSTONE_SMP_NEWLIB
#define configSET_TLS_BLOCK(block) ((void)0)
#define configINCLUDE_FREERTOS_TASK_C_ADDITIONS_H 1
#undef configUSE_RECURSIVE_MUTEXES
#define configUSE_RECURSIVE_MUTEXES 1
#define INCLUDE_xSemaphoreGetMutexHolder 1
#endif
#ifndef __ASSEMBLER__
#include "../../include/a733_cpu_power.h"
#define vApplicationPassiveIdleHook a733_cpu_idle
#include <stdint.h>
void a733_smp_startup_check(void);
uintptr_t a733_smp_priority_address(void);
uint64_t a733_smp_sgi_value(unsigned interrupt, unsigned core);
/* The port's ICC_SGI1R_EL1 value for one core, from the A733 affinity map. */
#define portSGI1R_VALUE(interrupt, core) a733_smp_sgi_value((interrupt), (core))
#endif
#endif
