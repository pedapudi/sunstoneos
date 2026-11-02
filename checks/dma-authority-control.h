#ifndef DMA_AUTHORITY_CONTROL_H
#define DMA_AUTHORITY_CONTROL_H
#include <stdint.h>
#define DMA_AUTHORITY_SAMPLES 8u
#define DMA_AUTHORITY_CHANNELS 16u
#define DMA_AUTHORITY_WORDS 6u
#define DMA_AUTHORITY_MAGIC UINT64_C(0x31414d444e535353)
struct dma_authority_sample {
    uint64_t counter;
    uint32_t bus,mbus,e902_clock,e902_bus,cpuidle;
    uint32_t gic_control,gic_enable,gic_group,gic_modifier,gic_pending,gic_active;
    uint32_t aperture_sampled,autogate,busy,cpu_route,mcu_route;
    uint32_t channel[DMA_AUTHORITY_CHANNELS][DMA_AUTHORITY_WORDS];
};
struct dma_authority_result {
    uint64_t magic,version,bytes,frequency;
    uint32_t samples,device_writes;
    uint64_t scr_el3,sctlr_el1;
    struct dma_authority_sample sample[DMA_AUTHORITY_SAMPLES];
};
void dma_authority_collect(struct dma_authority_sample *);
#endif
