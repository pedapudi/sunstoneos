#ifndef SUNSTONE_UART_SESSION_H
#define SUNSTONE_UART_SESSION_H
#include "a733_board_application.h"
#define A733_UART_SESSION_MAGIC UINT64_C(0x4153373355414131)
#define A733_UART_CONTRACT_MAGIC UINT64_C(0x4153373355414331)
#define UART_OUTPUT_BYTES 144u
struct uart_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,operation_timeout_us;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t expected[4][3],desired[4][3];
 uint32_t tx_bytes,rx_bytes;
 uint8_t tx[32];
};
_Static_assert(sizeof(struct uart_session)==832,"UART session geometry");
int uart_session_valid(const struct uart_session *);
#endif
