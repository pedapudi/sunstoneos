#ifndef SUNSTONE_A733_SMP_CONFIG_H
#define SUNSTONE_A733_SMP_CONFIG_H
#include "../../include/FreeRTOSConfig.h"
#define configNUMBER_OF_CORES 8
#define configUSE_CORE_AFFINITY 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configENABLE_MPU 0
#define configUSE_PASSIVE_IDLE_HOOK 0
#define INCLUDE_vTaskDelete 1
/* The patched port obtains the primary redistributor from discovery. */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS 0
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET 0
#ifndef __ASSEMBLER__
#include <stdint.h>
void a733_smp_startup_check(void);
uintptr_t a733_smp_priority_address(void);
uint64_t a733_smp_sgi_value(unsigned interrupt, unsigned core);
/* The port's ICC_SGI1R_EL1 value for one core, from the A733 affinity map. */
#define portSGI1R_VALUE(interrupt, core) a733_smp_sgi_value((interrupt), (core))
#endif
#endif
