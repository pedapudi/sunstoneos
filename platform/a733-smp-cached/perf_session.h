#ifndef SUNSTONE_A733_PERF_SESSION_H
#define SUNSTONE_A733_PERF_SESSION_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
/* Board-session measurements (perf_session.c); SUNSTONE_A733_PERF_SESSION only. */
int a733_perf_pre_scheduler(unsigned core, uint64_t deadline);
void a733_perf_tick_irq(void);
/* Suspends the 8 workers among the 16 workload tasks in TASKS until the
 * measurements finish; the measurement tasks borrow the lower halves of the
 * background tasks' STACKS, each STACK_WORDS long. */
void a733_perf_session_start(StaticTask_t *tasks, StackType_t *stacks, unsigned stack_words);
#endif
