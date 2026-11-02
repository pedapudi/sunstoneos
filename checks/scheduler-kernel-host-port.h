/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FreeRTOS.h loads the real patched ABI first. Override only instructions
 * requiring Arm execution; actual list, tasks and scheduler code stays linked. */
#include "FreeRTOS.h"
#undef portYIELD
void ssFixtureYield(void);
#define portYIELD() ssFixtureYield()
#undef portDISABLE_INTERRUPTS
#define portDISABLE_INTERRUPTS() ((void)0)
#undef portENABLE_INTERRUPTS
#define portENABLE_INTERRUPTS() ((void)0)
#undef portMEMORY_BARRIER
#define portMEMORY_BARRIER() atomic_thread_fence(memory_order_seq_cst)
uint64_t ssFixtureTaskRegister(void);
#define ullPortGetTaskRegister() ssFixtureTaskRegister()

#undef portCLEAN_UP_TCB
void ssFixtureCleanup(void *);
#define portCLEAN_UP_TCB(task) ssFixtureCleanup(task)
