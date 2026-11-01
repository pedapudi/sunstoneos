#ifndef SUNSTONE_NPU_LATENCY_STATS_H
#define SUNSTONE_NPU_LATENCY_STATS_H
#ifndef SUNSTONE_NPU_QUEUE_TIMING
#define SUNSTONE_NPU_QUEUE_TIMING 1
#endif
/* Diagnostic configuration, forced into every unit before FreeRTOS headers
 * so kernel task records and their callers share one counter layout. */
#define configGENERATE_RUN_TIME_STATS 1
#define configRUN_TIME_COUNTER_TYPE uint64_t
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ((void)0)
#define portGET_RUN_TIME_COUNTER_VALUE() a733_smp_counter()
#ifndef __ASSEMBLER__
#include <stdint.h>
uint64_t a733_smp_counter(void);
#endif
#endif
