#ifndef SUNSTONE_DMA_SERVICE_I2S_H
#define SUNSTONE_DMA_SERVICE_I2S_H
#include "a733_i2s.h"

/* Trusted startup profile. Callback state and all parent leases outlive the
 * service. Only the DMA service worker may operate this binding or capture. */
struct dma_service_i2s_profile {
    struct a733_i2s_format format;
    struct a733_i2s_operations operations;
    void *cookie;
};
int dma_service_i2s_profile_valid(const struct dma_service_i2s_profile *);
int dma_service_i2s_profile_equal(const struct dma_service_i2s_profile *,
                                const struct dma_service_i2s_profile *);
/* Relative microseconds become a finite absolute resource deadline. Capture
 * duration remains in DMA counter ticks. Call only from the privileged worker. */
enum a733_i2s_status dma_service_i2s_open(struct a733_i2s_capture *,
    const struct dma_service_i2s_profile *, struct a733_dma_controller *, unsigned, uint64_t timeout_us);
enum a733_i2s_status dma_service_i2s_close(struct a733_i2s_capture *, uint64_t timeout_us);
#endif
