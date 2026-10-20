#ifndef SUNSTONE_SPI_SESSION_H
#define SUNSTONE_SPI_SESSION_H
#include "a733_board_application.h"
#define A733_SPI_SESSION_MAGIC UINT64_C(0x4153373353504131)
#define A733_SPI_CONTRACT_MAGIC UINT64_C(0x4153373353504331)
#define SPI_OUTPUT_BYTES 96u
/* The existing832-byte ABI selects polling in version1 and deferred IRQ
 * completion in version2. Exact request/electrical/session seals cover both. */
struct spi_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,operation_timeout_us;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t expected[4][3],desired[4][3];
 uint32_t tx_bytes,rx_bytes;
 uint8_t tx[32];
};
_Static_assert(sizeof(struct spi_session)==832,"SPI session geometry");
int spi_session_valid(const struct spi_session *);
#endif
