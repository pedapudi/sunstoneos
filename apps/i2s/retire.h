#ifndef SUNSTONE_I2S_RETIRE_H
#define SUNSTONE_I2S_RETIRE_H
#include "dma_service.h"
/* One original DMA service client owns the exact outstanding RX/TX leases.
 * Begin only after its earlier copied request has been collected. */
struct i2s_retirement {
    struct dma_service *service;
    uint64_t held[2],pending;
    struct dma_service_reply reply;
    unsigned jobs,job,stage,failed;
};
int i2s_retirement_step(struct i2s_retirement *);
#endif
