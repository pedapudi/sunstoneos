#include <stdint.h>
/* New SD profiles require this immutable capability in addition to the
 * unchanged legacy session/service geometry and the exact linked build ID.
 * Version 2 admits profiles 0-3 (mask 15), where 3 is UHS-I SDR104, and
 * publishes record version 4 with per-read timing for nonzero profiles. */
__attribute__((used,retain)) const uint64_t storage_read_sd_modes[4]={
 UINT64_C(0x4153373353444d31),2,15,400000000};

#ifndef SUNSTONE_EMMC_READ
#include "storage_read_service.h"
#include "sunxi_mmc.h"
/* Private NC descriptor reservation, profiles 2/3 and copied payload capacity.
 * The preparer checks these immutable bytes from the actual linked image. */
__attribute__((used,retain)) const uint64_t storage_read_dma_geometry[6]={
 UINT64_C(0x4153373353444431),1,12,STORAGE_READ_MAX_BLOCKS,UINT64_C(0x41c00000),
 SUNXI_MMC_DMA_DESCRIPTORS*sizeof(struct sunxi_mmc_dma_descriptor)};
#endif
