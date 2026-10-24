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
/* The port's ICC_SGI1R_EL1 value for one core: MPIDR Aff1 is the A733 core
 * number, so the target list holds bit 0 and Aff1 the core
 * (a733_cpu_sgi1r). The port sends only SGI 0. */
static inline uint64_t a733_smp_sgi1r(unsigned interrupt, unsigned core)
{
    configASSERT(interrupt == 0 && core < 8);
    return ((uint64_t)core << 16) | 1;
}
#define portSGI1R_VALUE(interrupt, core) a733_smp_sgi1r((interrupt), (core))
#endif
#endif
