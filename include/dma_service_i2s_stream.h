#ifndef SUNSTONE_DMA_SERVICE_I2S_STREAM_H
#define SUNSTONE_DMA_SERVICE_I2S_STREAM_H
#include "a733_i2s_stream.h"
/* Trusted profile and exact catalogue are copied by dma_service_init. */
struct dma_service_i2s_stream_profile {
    struct a733_i2s_stream_profile stream;
    unsigned duplex; /* Exactly one compatible RX/TX pair on distinct channels. */
};
int dma_service_i2s_stream_pair(const struct dma_service_i2s_stream_profile *,
    const struct dma_service_i2s_stream_profile *);
enum a733_i2s_status dma_service_i2s_stream_duplex_open(struct a733_i2s_stream *,
    const struct dma_service_i2s_stream_profile *,const struct a733_dma_transfer *,
    struct a733_dma_controller *,unsigned,uint64_t,struct a733_i2s_stream *);
enum a733_i2s_status dma_service_i2s_stream_open(struct a733_i2s_stream *,
    const struct dma_service_i2s_stream_profile *, const struct a733_dma_transfer *,
    struct a733_dma_controller *, unsigned, uint64_t timeout_us);
enum a733_i2s_status dma_service_i2s_stream_close(struct a733_i2s_stream *, uint64_t timeout_us);
#endif
