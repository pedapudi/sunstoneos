#ifndef SUNSTONE_UART_SNAPSHOT_H
#define SUNSTONE_UART_SNAPSHOT_H
#include "uart_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int uart_session_prepare(const struct a733_smp_contract *);
const struct uart_session *uart_admitted_session(void);
#endif
