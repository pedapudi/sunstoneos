#ifndef SUNSTONE_SPI_DMA_H
#define SUNSTONE_SPI_DMA_H
#include "a733_dma_native.h"
#include "a733_dma_startup.h"
#include "a733_memory_reservations.h"
struct spi_dma_application {
 struct a733_dma_native native;
 struct a733_dma_startup startup;
 struct a733_dma_controller controller;
 struct device_memory_pool *pool;
 struct device_memory_lease lease;
 struct device_memory_extent extent;
 struct device_memory_allocation allocation;
 struct a733_dma_segment segments[2];
 struct a733_dma_mapping mappings[2];
 struct a733_dma_transfer transfers[2];
 unsigned stage,submitted,completed,drained,failed,retained;
 unsigned observations[2];
};
extern struct spi_dma_application spi_dma;
int spi_dma_construct(void);
int spi_dma_start(void);
int spi_dma_observe(void);
int spi_dma_drain(void);
int spi_dma_copy(void);
void spi_dma_retain(void);
#endif
