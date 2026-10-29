#ifndef SUNSTONE_SPI_SESSION_H
#define SUNSTONE_SPI_SESSION_H
#include "a733_board_application.h"
#define A733_SPI_SESSION_MAGIC UINT64_C(0x4153373353504131)
#define A733_SPI_CONTRACT_MAGIC UINT64_C(0x4153373353504331)
#ifdef SUNSTONE_SPI_DMA
#define SPI_OUTPUT_BYTES 4192u
#define SPI_WIRE_VERSION 2u
#define SPI_SESSION_VALID_VERSION(v) ((v)==3)
struct spi_dma_authority {
 uint32_t fabric[3],rate_hz[3];
 uint32_t expected_cpu,expected_mcu,desired_cpu,desired_mcu,route_change_mask,cold;
 uint64_t owner,handoff,controller,evidence;
 uint32_t established;
 uint16_t owned_channels,service_channels;
 uint64_t startup_timeout_us;
 uint8_t authority[32];
};
_Static_assert(sizeof(struct spi_dma_authority)==128,"SPI DMA authority geometry");
#else
#define SPI_OUTPUT_BYTES 96u
#define SPI_WIRE_VERSION 1u
#define SPI_SESSION_VALID_VERSION(v) ((v)==1||(v)==2)
#endif
#define SPI_SESSION_USES_IRQ(v) ((v)>=2)
/* The existing832-byte ABI selects polling in version1 and deferred IRQ
 * completion in version2. Exact request/electrical/session seals cover both. */
struct spi_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,operation_timeout_us;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t expected[4][3],desired[4][3];
 uint32_t tx_bytes,rx_bytes;
 uint8_t tx[32];
#ifdef SUNSTONE_SPI_DMA
 struct spi_dma_authority dma;
#endif
};
#ifdef SUNSTONE_SPI_DMA
_Static_assert(sizeof(struct spi_session)==960,"SPI DMA session geometry");
#else
_Static_assert(sizeof(struct spi_session)==832,"SPI session geometry");
#endif
int spi_session_valid(const struct spi_session *);
#endif
