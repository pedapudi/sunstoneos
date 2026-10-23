#ifndef SUNSTONE_JPEG_PRIVATE_H
#define SUNSTONE_JPEG_PRIVATE_H
#include "a733_jpeg_application.h"
#include "input.h"
#include "a733_video_platform.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#include "../board/application.h"
struct jpeg_application {
    struct a733_jpeg_session session;
    struct jpeg_input supplied;
    struct a733_video_platform platform;
    struct a733_video_jpeg_run run;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_clock_token iommu_clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct device_memory_pool *pool;
    struct device_memory_lease leases[4];
    struct a733_video_platform_config config;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    StaticTask_t task;
    StackType_t stack[4096];
    TaskHandle_t handle;
    uint8_t *input,*luma,*chroma;
    uint32_t *table;
    unsigned stage,copy_step,decoded; /* decoded: jobs started. */
    uint32_t copy_row; /* Next output row: luma, then Cb, then Cr. */
    atomic_uint result;
    uint8_t payload[JPEG_OUTPUT_CAPACITY],digest[32];
};
extern struct jpeg_application jpeg;
void jpeg_task(void *);
void board_usb0_task(void *);
#endif
