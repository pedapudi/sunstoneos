#ifndef SUNSTONE_H264_PRIVATE_H
#define SUNSTONE_H264_PRIVATE_H
#include "a733_h264_application.h"
#include "a733_h264_input.h"
#include "a733_video_platform.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#include "../board/application.h"
#include "stream.h"
struct h264_application {
    struct a733_h264_session session;
    struct a733_h264_input supplied;
    struct a733_h264_platform platform;
    struct a733_h264_run run;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_clock_token iommu_clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct device_memory_pool *pool;
    struct device_memory_lease leases[7];
    struct a733_video_platform_config config;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    StaticTask_t task;
    StackType_t stack[4096];
    TaskHandle_t handle;
    uint8_t *input,*luma,*chroma;
    uint8_t *buffers[A733_H264_BUFFERS];
    struct a733_h264_job job;
    uint32_t *table;
    unsigned stage,copy_step,refill_step;
    atomic_uint result,completed;
    uint8_t payload[A733_H264_PICTURES][A733_H264_OUTPUT_BYTES],digest[A733_H264_PICTURES][32];
};
extern struct h264_application h264;
void h264_task(void *);
void board_usb0_task(void *);
#endif
