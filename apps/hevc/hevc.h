#ifndef SUNSTONE_HEVC_PRIVATE_H
#define SUNSTONE_HEVC_PRIVATE_H
#include "a733_hevc_application.h"
#include "input.h"
#include "storage.h"
#include "snapshot.h"
#include "a733_video_platform.h"
#include "a733_iommu.h"
#include "../board/application.h"
/* Copied pictures are published in order. completed counts them, and
 * acknowledged counts host ACKs of their digests; the controller programs
 * the P picture only after ACK0. result is 0 running, 1 both pictures copied,
 * 2 retained failure. */
struct hevc_application {
    struct a733_hevc_session session;
    struct hevc_input supplied;
    struct hevc_storage storage;
    struct a733_hevc_platform platform;
    struct a733_hevc_run run;
    struct a733_iommu iommu;
    struct a733_clock_token iommu_clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct a733_video_platform_config config;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    StaticTask_t task;
    StackType_t stack[4096];
    TaskHandle_t handle;
    unsigned stage,copy_step;
    size_t copy_offset;
    struct storage_digest copy_hash;
    atomic_uint result,completed,acknowledged;
    uint8_t digest[A733_HEVC_PICTURES][32];
};
extern struct hevc_application hevc;
void hevc_task(void *);
void board_usb0_task(void *);
#endif
