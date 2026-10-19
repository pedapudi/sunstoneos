#ifndef SUNSTONE_UART_SESSION_H
#define SUNSTONE_UART_SESSION_H
#include "a733_board_application.h"
#define A733_UART_SESSION_MAGIC UINT64_C(0x4153373355414131)
#define A733_UART_CONTRACT_MAGIC UINT64_C(0x4153373355414331)
#ifdef SUNSTONE_UART_DMA
#define UART_SESSION_VERSION 2u
#define UART_TRANSFER_MAX 1024u
#define UART_WIRE_VERSION 3u
#define UART_OUTPUT_BYTES (96u+2u*UART_TRANSFER_MAX)
/* Pointer-free sealed wire metadata, identical for the ARM32 producer and
 * AArch64 consumer. Startup deadline is derived from the admitted session. */
struct uart_dma_authority {
 uint32_t fabric[3],rate_hz[3];
 uint32_t expected_cpu,expected_mcu,desired_cpu,desired_mcu,route_change_mask,cold;
 uint64_t owner,handoff,controller,evidence;
 uint32_t established;
 uint16_t owned_channels,service_channels;
 uint64_t startup_timeout_us;
 uint8_t authority[32];
};
_Static_assert(sizeof(struct uart_dma_authority)==128,"UART DMA authority geometry");
#else
#define UART_SESSION_VERSION 1u
#define UART_TRANSFER_MAX 32u
#define UART_WIRE_VERSION 2u
#define UART_OUTPUT_BYTES 144u
#endif
struct uart_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,operation_timeout_us;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t expected[4][3],desired[4][3];
 uint32_t tx_bytes,rx_bytes;
 uint8_t tx[UART_TRANSFER_MAX];
#ifdef SUNSTONE_UART_DMA
 struct uart_dma_authority dma;
#endif
};
#ifdef SUNSTONE_UART_DMA
_Static_assert(sizeof(struct uart_session)==1952,"UART DMA session geometry");
#else
_Static_assert(sizeof(struct uart_session)==832,"UART session geometry");
#endif
_Static_assert(sizeof(struct uart_session)<=4096,"UART session page bound");
int uart_session_valid(const struct uart_session *);
#endif
