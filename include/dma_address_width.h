#ifndef SUNSTONE_DMA_ADDRESS_WIDTH_H
#define SUNSTONE_DMA_ADDRESS_WIDTH_H

#include <stdint.h>

/* Address limits that DMA owners enforce at admission. Each limit is an
 * exclusive end: an accepted range satisfies address + bytes <= limit.
 * docs/dma-address-width.md gives the rule for every master and its source. */

/* DMA0 data addresses: descriptor parameter bits 11-13 and 15-17 carry
 * source and destination bits 34:32. */
#define A733_DMA0_DATA_LIMIT (UINT64_C(1) << 35)

/* DMA0 descriptors: this value is the end-of-chain link marker, and the
 * driver writes only the low 32 bits of a link. */
#define A733_DMA0_DESCRIPTOR_LIMIT UINT64_C(0xfffff800)

/* System IOMMU leaf and directory entries: bits 9:8 hold physical bits 33:32. */
#define A733_IOMMU_PHYSICAL_LIMIT (UINT64_C(1) << 34)

/* Device addresses that IOMMU masters are given. The hardware IOVA space is
 * 34 bits; every current profile keeps device spans below 4 GiB. */
#define A733_IOMMU_IOVA_LIMIT (UINT64_C(1) << 32)

/* Masters that write 32-bit bus addresses, such as PCIe endpoint DMA and
 * xHCI without 64-bit addressing. */
#define DMA_ADDRESS_32BIT_LIMIT (UINT64_C(1) << 32)

#endif
