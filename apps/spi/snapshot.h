#ifndef SUNSTONE_SPI_SNAPSHOT_H
#define SUNSTONE_SPI_SNAPSHOT_H
#include "spi_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int spi_session_prepare(const struct a733_smp_contract *);
const struct spi_session *spi_admitted_session(void);
#endif
