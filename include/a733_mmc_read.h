/* Shared read-only SD module and IDMAC admission used by the runtime and
 * cold loader. Callers own controller access, clocks and cache transitions. */
#ifndef SUNSTONE_A733_MMC_READ_H
#define SUNSTONE_A733_MMC_READ_H
#include "sunxi_mmc.h"
#include "sunxi_smhc.h"
static inline int a733_mmc_dma_value(uintptr_t area,uint32_t off,uint32_t value)
{
    switch(off) {
    case SUNXI_SMHC_FTRGL:return area&&value==SUNXI_SMHC_FTRGL_VALUE;
    case SUNXI_SMHC_DLBA:return area&&value==(uint32_t)(area>>2);
    case SUNXI_SMHC_IDST:return area&&!(value&~SUNXI_SMHC_IDST_CLEAR);
    case SUNXI_SMHC_IDIE:return !value;
    case SUNXI_SMHC_DMAC:return !value||(area&&(value==SUNXI_SMHC_DMAC_RESET||value==SUNXI_SMHC_DMAC_ON));
    case SUNXI_SMHC_GCTRL:return area||!(value&SUNXI_SMHC_GCTRL_DMA_ENABLE);
    default:return 1;
    }
}
/* Storage and buffer spans must already be admitted, disjoint, aligned and
 * inside the controller's 34-bit physical address range. */
static inline int a733_mmc_dma_chain(volatile const struct sunxi_mmc_dma_descriptor *area,
                                   const void *buffer,size_t bytes,uint32_t count)
{
    uintptr_t first=(uintptr_t)buffer;
    if(!count||count!=bytes||count>SUNXI_MMC_MAX_DMA_BLOCKS*512u)return 0;
    unsigned n=(count+SUNXI_MMC_DMA_SEGMENT-1)/SUNXI_MMC_DMA_SEGMENT;
    for(unsigned i=0;i<n;++i) {
        volatile const struct sunxi_mmc_dma_descriptor *d=&area[i];
        uint32_t expected=SUNXI_SMHC_DES_OWN|SUNXI_SMHC_DES_CH|SUNXI_SMHC_DES_DIC;
        if(!i)expected|=SUNXI_SMHC_DES_FD;
        if(i==n-1)expected=(expected|SUNXI_SMHC_DES_LD)&~SUNXI_SMHC_DES_DIC;
        uint32_t left=count-i*SUNXI_MMC_DMA_SEGMENT;
        if(d->config!=expected||d->size!=(left<SUNXI_MMC_DMA_SEGMENT?left:SUNXI_MMC_DMA_SEGMENT)||
           d->buffer!=(uint32_t)((first+(uintptr_t)i*SUNXI_MMC_DMA_SEGMENT)>>2)||
           d->next!=(uint32_t)((uintptr_t)&area[i+1]>>2))return 0;
    }
    volatile const struct sunxi_mmc_dma_descriptor *spare=&area[n];
    return !spare->config&&!spare->size&&!spare->buffer&&!spare->next;
}
/* Profiles 1-2 use PERI0_400M/4..32. UHS-I also uses 400M/2 or 300M/1. */
static inline int a733_mmc_module_setting(enum sunxi_mmc_sd_profile profile,
                                        unsigned divisor,uint32_t *out)
{
    unsigned m=divisor&0xffu,source=(divisor&SUNXI_MMC_MODULE_300M)?2u:1u;
    int ok=!(divisor&~(0xffu|SUNXI_MMC_MODULE_300M))&&(profile==SUNXI_MMC_SD_UHS_SDR104?
        ((source==2&&m==1)||(source==1&&(m==2||m==4))):(source==1&&m>=4&&m<=32));
    if(ok)*out=UINT32_C(0x80000000)|((uint32_t)source<<24)|(m-1);
    return ok;
}
#endif
