#ifndef SUNSTONE_UART_PRIVATE_H
#define SUNSTONE_UART_PRIVATE_H
#include "uart_session.h"
#include "../board/application.h"
#include "a733_uart_platform_native.h"
#include "gicv3_spi.h"
struct uart_application {
 struct uart_session session;
 struct a733_uart_platform platform;
 struct a733_uart_platform_native native;
 struct a733_uart_platform_config config;
 struct a733_uart_platform_ops operations;
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
 struct gicv3_spi irq;
 atomic_uint result,failed,pending,irq_fault,notifications;
 unsigned masked,armed,tx_irqs,rx_irqs,other_irqs;
 unsigned stage;
 size_t sent,received;
 uint64_t deadline;
 uint8_t tx[32],rx[32],status[32],payload[UART_OUTPUT_BYTES],digest[32],session_digest[32];
};
extern struct uart_application uart;
void uart_application_irq(void);
int uart_irq_rearm(void);
int uart_irq_stop(void);
int uart_construct(void);
int uart_step(void);
void uart_task(void *);
int uart_output_ready(void);
int uart_live(void);
#endif
