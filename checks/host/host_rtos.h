#ifndef HOST_RTOS_H
#define HOST_RTOS_H
/* Deterministic host runtime for fixtures that link real SunstoneOS code.
 *
 * The FreeRTOS kernel (tasks.c, queue.c, list.c) runs unmodified on a host port.
 * Tasks are user-level contexts. A context switch happens only where the kernel
 * switches: a blocking call, a yield, a tick that readies a higher-priority task,
 * or the return from a script-raised interrupt. Time is virtual. It moves only
 * when the running task calls host_advance_us(), or when every task is blocked and
 * the idle task steps to the next tick. Nothing depends on the host scheduler or
 * wall clock, so a run is the same every time under any sanitizer.
 *
 * Time. One virtual clock in microseconds. The tick count is time / 1000. The
 * A733 generic counter reads time * 24 (24 MHz). Interrupts that fall due while a
 * critical section or interrupt mask is held are delivered at its exit.
 *
 * One scheduler session runs per process. A fixture puts its scenarios in the test
 * task passed to host_rtos_run(), and creates and deletes helper tasks there.
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define HOST_COUNTER_HZ 24000000ull
/* Starts the scheduler with `test` as a task of the given priority (0 is the idle
 * priority; use 1 or higher). Returns when `test` returns. A run in which every
 * task stays blocked for more than max_idle_us of virtual time with no timer,
 * idle callback or interrupt source left to make progress fails the fixture. */
void host_rtos_run(void (*test)(void *), void *argument, unsigned priority, uint64_t max_idle_us);
/* Creates a task on static storage; the host supplies the stack. */
TaskHandle_t host_task_create(const char *name, void (*entry)(void *), void *argument, unsigned priority);

uint64_t host_time_us(void);
uint64_t host_counter(void);
/* Moves virtual time forward as the running task. Ticks and the programmed timer
 * compare fall due at their exact times; the running task is preempted if a tick
 * or interrupt readies a higher-priority task. */
void host_advance_us(uint64_t us);
/* Sets time. Allowed before the scheduler starts, or in a task while no task is
 * blocked on a timeout. Time never moves backward. */
void host_set_time_us(uint64_t us);

/* Runs an interrupt handler now, as the CPU would on exception entry, and switches
 * tasks on return if the handler requested it. The handler runs on the current
 * stack with interrupts masked. */
void host_isr(void (*handler)(void *), void *argument);
/* Schedules an interrupt at an exact virtual time. It runs when that time is
 * reached, in order with the tick and the compare timer, and after any interrupt
 * masking ends if it fell due inside one. */
void host_irq_at(uint64_t us, void (*handler)(void *), void *argument);
/* The most task-notification gives from interrupt context in any one interrupt. */
uint64_t host_max_gives_per_isr(void);
/* The total number of task-notification gives made from interrupt context. */
uint64_t host_gives_total(void);
/* Runs fn in a task of the highest priority at virtual time us, before any other task
 * that the same interrupts ready. fn must not block. host_run_now() runs it at once
 * in that task and returns when it is done: use it to act as a second task. */
void host_run_at(uint64_t us, void (*fn)(void *), void *argument);
void host_run_now(void (*fn)(void *), void *argument);
/* Holds back the compare interrupt until at least this time, as a lost or late
 * alarm; 0 releases it. */
void host_timer_hold_until(uint64_t us);
unsigned host_in_isr(void);
unsigned host_critical_depth(void);
/* Identity returned by portGET_CORE_ID(). */
extern unsigned host_core_id;

/* Called each time the idle task runs, before time steps to the next tick. The
 * callback may raise interrupts with host_isr() or call task APIs that are legal
 * from the idle task. */
void host_on_idle(void (*callback)(void *), void *argument);

/* Counts every context switch and each switch's destination, for fixtures that
 * assert on scheduling order. */
uint64_t host_switches(void);

/* Compare timer: sunstone_deadline_program() sets it. It fires `handler` as an
 * interrupt when virtual time reaches the programmed microsecond. */
void host_timer_handler(void (*handler)(void));
uint64_t host_timer_compare_us(void);
/* When nonzero, sunstone_deadline_kick() does not rearm at once; the fixture
 * delivers it with host_deadline_deliver_kick(), as a cross-core SGI arrives. */
void host_deadline_defer_kicks(unsigned defer);
unsigned host_deadline_kick_pending(void);
void host_deadline_deliver_kick(void);
#endif
