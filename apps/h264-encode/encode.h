#ifndef SUNSTONE_H264_ENCODE_PRIVATE_H
#define SUNSTONE_H264_ENCODE_PRIVATE_H
#include "a733_h264_encode_application.h"
#include "input.h"
#include "storage.h"
#include "a733_video_encode_platform.h"
#include "../board/application.h"
struct h264_encode_application {
 struct a733_h264_encode_session session;
 struct h264_encode_input supplied;
 struct h264_encode_storage storage;
 struct a733_h264_encode_platform platform;
 struct a733_h264_encode_run run;
 struct a733_iommu iommu;
 struct a733_clock_token iommu_clock;
 struct a733_rail_token rail;
 struct a733_pmic_request request;
 struct a733_pmic_snapshot supply;
 struct a733_encode_platform_config config;
 struct a733_gpu_power_resources power_ready;
 uint64_t power_token;
 StaticTask_t task;
 StackType_t stack[4096];
 TaskHandle_t handle;
 /* Indexed by picture ordinal. Picture k's bytes live in storage.copies[k&1]. */
 size_t output_bytes[A733_H264_ENCODE_GOP_FRAMES],copied[A733_H264_ENCODE_GOP_FRAMES];
 unsigned stage,prime_step[A733_H264_ENCODE_GOP_FRAMES],copy_step[A733_H264_ENCODE_GOP_FRAMES];
 atomic_uint result[A733_H264_ENCODE_GOP_FRAMES],acked[A733_H264_ENCODE_GOP_FRAMES],fatal;
 uint8_t digest[A733_H264_ENCODE_GOP_FRAMES][32];
};
_Static_assert(sizeof(struct h264_encode_application)<=65536,"H264 encoder CPU metadata budget");
extern struct h264_encode_application h264_encode;
int h264_encode_storage_allocate(void);
void h264_encode_task(void *);
void board_usb0_task(void *);
#endif
