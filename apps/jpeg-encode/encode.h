#ifndef SUNSTONE_JPEG_ENCODE_PRIVATE_H
#define SUNSTONE_JPEG_ENCODE_PRIVATE_H
#include "a733_jpeg_encode_application.h"
#include "input.h"
#include "a733_video_encode_platform.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#include "../board/application.h"
struct jpeg_encode_application {
    struct a733_jpeg_encode_session session;
    struct jpeg_encode_input supplied;
    struct a733_encode_platform platform;
    struct a733_jpeg_encode_run run;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_clock_token iommu_clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct device_memory_pool *pool,*wb_pool;
    struct device_memory_lease leases[3],wb_leases[2];
    struct a733_encode_platform_config config;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    StaticTask_t task;
    StackType_t stack[4096];
    TaskHandle_t handle;
    uint8_t *input,*output,*payload;
    uint32_t *table;
    size_t jpeg_bytes,copied;
    unsigned stage,copy_step;
    atomic_uint result;
    uint8_t digest[32];
};
_Static_assert(sizeof(struct jpeg_encode_application)<=65536,"encoder CPU metadata budget");
extern struct jpeg_encode_application jpeg_encode;
int jpeg_encode_upload_allocate(void);
int jpeg_encode_allocate(void);
void jpeg_encode_task(void *);
void board_usb0_task(void *);
#endif
