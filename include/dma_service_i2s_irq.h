#ifndef SUNSTONE_DMA_SERVICE_I2S_IRQ_H
#define SUNSTONE_DMA_SERVICE_I2S_IRQ_H
#include "dma_service.h"
#include "gicv3_spi.h"
/* DMA0 interrupt binding for an image whose DMA service runs I2S streams.
 * A finite stream bridges each period boundary with the I2S FIFO alone, so
 * the worker should see queue end as soon as it happens. The interrupt wakes
 * the worker at that event; the one-tick fallback keeps the existing tick
 * polling as the bound when an interrupt is late, missing or masked, so a
 * delivery fault never lengthens the bridge beyond the polled path. */
#define A733_DMA0_INTID 143u
#define DMA_SERVICE_I2S_IRQ_FALLBACK 1u
/* Private storage; the masked flag is shared between the handler and the
 * worker's rearm under the kernel critical section. */
struct dma_service_i2s_irq {
    struct dma_service *service;
    struct gicv3_spi spi;
    unsigned masked;
};
/* EL1, after dma_service initialization and before the scheduler. The EL3
 * boot has routed INTID 143 to the worker's core with the SPI disabled; the
 * first rearm enables it. counter_hz converts the fallback to DMA ticks. */
int dma_service_i2s_irq_bind(struct dma_service_i2s_irq *, struct dma_service *, uint64_t counter_hz);
/* IRQ context, from the image's interrupt table: mask, then notify. */
void dma_service_i2s_irq_handler(struct dma_service_i2s_irq *);
#endif
