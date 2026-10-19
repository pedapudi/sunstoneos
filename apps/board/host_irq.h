#ifndef SUNSTONE_BOARD_HOST_IRQ_H
#define SUNSTONE_BOARD_HOST_IRQ_H
/* Fixed CPU0 owner and dispatcher hook, with GIC ACK/EOI outside the handler. */
void board_host_irq(void);
int board_host_irq_bind(void);
int board_host_irq_before(void);
int board_host_irq_after(void);
void board_host_irq_wait(void);
void board_host_irq_stop(void);
#endif
