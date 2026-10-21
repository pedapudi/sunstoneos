#ifndef SUNSTONE_DMA_SERVICE_I2S_H
#define SUNSTONE_DMA_SERVICE_I2S_H
#include "dma_service.h"

/* Private to the DMA service. Each channel that runs I2S stream jobs receives
 * one context in dma_service.streams before any job can open. The binding never
 * changes, so a retained or duplex-paired stream keeps its address for the
 * service lifetime. Returns 0 when the jobs need more contexts than exist. */
int dma_service_i2s_bind(struct dma_service *, const struct dma_service_job *, unsigned count);
/* The stream context of a channel, or NULL when no stream job uses it. */
struct a733_i2s_stream *dma_service_i2s_stream(struct dma_service *, unsigned channel);
#endif
