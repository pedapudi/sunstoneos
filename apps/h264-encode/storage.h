#ifndef SUNSTONE_H264_ENCODE_STORAGE_H
#define SUNSTONE_H264_ENCODE_STORAGE_H
#include "a733_video_h264_encode.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#define H264_ENCODE_NC_BYTES 6254592u
#define H264_ENCODE_WB_BYTES 3022848u
struct h264_encode_storage {
    struct device_memory_pool *nc,*wb;
    struct device_memory_lease nc_leases[9],wb_leases[3];
    uint32_t *table;
    uint8_t *upload,*copies[2],*buffers[A733_H264_ENCODE_BUFFERS];
    struct a733_iommu_tables tables;
    struct a733_h264_encode_job job;
    unsigned initialized,published;
};
/* Allocate once, before upload; no MMIO/publication/release. Full immutable
 * upload authentication precedes publish, which copies both frames/header and
 * marks all NC extents permanently DEVICE. Failure never releases any lease. */
int h264_encode_storage_init(struct h264_encode_storage *,struct device_memory_pool *,
    struct device_memory_pool *,uint64_t);
int h264_encode_storage_publish(struct h264_encode_storage *,const uint8_t digest[32]);
int h264_encode_storage_guards(const struct h264_encode_storage *);
#endif
