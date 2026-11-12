#ifndef SUNSTONE_BOARD_HOST_IRQ_H
#define SUNSTONE_BOARD_HOST_IRQ_H
#include <stdint.h>
/* Fixed CPU0 owner and dispatcher hook, with GIC ACK/EOI outside the handler. */
void board_host_irq(void);
int board_host_irq_bind(void);
int board_host_irq_before(void);
int board_host_irq_after(void);
void board_host_irq_wait(void);
void board_host_irq_stop(void);
/* After native_halt proves the controller stopped: one mask/active/pending
 * observation, returning 1 retired, 0 pending or -1 fault. */
int board_host_irq_retire(uint64_t deadline);
#ifdef SUNSTONE_BOARD_HOST_DEADLINE
/* Register from the host owner once. Snapshot while it owns native execution;
 * shared callers release their gate before wait. Deadlines exclude ready hints. */
int board_host_irq_deadline_init(void);
void board_host_irq_schedule(uint64_t hard_deadline_us,int ready);
#endif
#endif
