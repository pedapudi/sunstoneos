#ifndef SUNSTONE_HEVC_STORAGE_H
#define SUNSTONE_HEVC_STORAGE_H
#include "a733_video_hevc.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#define HEVC_NC_BYTES 2400256u
#define HEVC_WB_BYTES 1449984u
/* Write-back RAM holds the upload and the two copied pictures; the CPU alone
 * accesses them and their leases are retained at allocation. Uncached RAM
 * holds the IOMMU tables and the eight device buffers, each between two
 * unmapped guard pages filled with 0xa5. Nothing is ever released. */
struct hevc_storage {
 struct device_memory_pool *wb,*nc;
 struct device_memory_lease wb_leases[3],nc_leases[9];
 uint8_t *upload,*copies[2],*buffers[HEVC_BUFFERS];uint32_t *table;
 struct a733_iommu_tables tables;struct a733_hevc_job job;
 unsigned initialized,published;
};
/* Allocates and builds the tables from each allocation's physical address.
 * Device state is unchanged until publish. Returns 1 or 0. */
int hevc_storage_init(struct hevc_storage *,struct device_memory_pool *,struct device_memory_pool *,uint64_t);
/* Once, after the upload is committed: checks its digest, copies each slice
 * NAL to its zero-padded input slot, clears outputs and scratch and transfers
 * every uncached allocation to the device. Returns 1 only if the guards hold. */
int hevc_storage_publish(struct hevc_storage *,size_t,const struct a733_hevc_plan *,const uint8_t[32]);
/* Guard pages and output slack still hold 0xa5 and every buffer is DEVICE. */
int hevc_storage_guards(const struct hevc_storage *);
#endif
