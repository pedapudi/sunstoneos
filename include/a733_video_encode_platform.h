/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_ENCODE_PLATFORM_H
#define SUNSTONE_A733_VIDEO_ENCODE_PLATFORM_H
#include "a733_video_jpeg_encode.h"
#include "a733_video_unit.h"
/* Encoder-only inherited-powered authority. Parent/fabric/rail/IRQ80/master3
 * leases and initial all-master transaction/walk drain are external admission
 * facts. Readback does not establish them. Every lease persists through power
 * removal; neither copied output nor caller ACK permits release or reuse. */
struct a733_encode_platform_config {
    struct a733_clock_leases *clocks;
    struct a733_clock_token clock;
    struct a733_gpu_resource_manager *power;
    struct a733_gpu_resource_lease pck;
    unsigned power_client;
    uint64_t owner,handoff;
    uint32_t module,module_hz,fabric[3],fabric_hz[3];
};
struct a733_encode_platform_ops {
    int (*now_us)(void *,uint64_t *);
    int (*admit)(void *,const struct a733_encode_platform_config *,const struct a733_jpeg_encode_job *,uint64_t,uint64_t *);
    int (*memory)(void *,uint64_t,const struct a733_jpeg_encode_job *,uint64_t);
    int (*copied)(void *,uint64_t,size_t,uint64_t);
    int (*enter)(void *,uint64_t);
    int (*leave)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
};
/* Codec-neutral encoder sequencing on the shared VE resource core: admission,
 * inherited powered baseline, identity and memory. Each codec binding binds
 * and compares its own typed job and passes it through admit and memory. */
struct a733_video_encoder {
    struct a733_video_unit unit;
    struct a733_encode_platform_config config;
    int (*admit)(void *,const struct a733_encode_platform_config *,const void *,uint64_t,uint64_t *);
    int (*memory)(void *,uint64_t,const void *,uint64_t);
    void *context;
    unsigned bound,checked,prepared;
};
/* JPEG binding. */
struct a733_encode_platform {
    struct a733_video_encoder encoder;
    struct a733_encode_platform_ops ops;
    struct a733_jpeg_encode_job job;
    void *cookie;
};
int a733_encode_platform_scopes(const struct a733_encode_platform_config *,a733_clock_mask_t *);
int a733_encode_platform_init(struct a733_encode_platform *,const struct a733_encode_platform_config *,
    const struct a733_encode_platform_ops *,void *);
const struct a733_jpeg_encode_ops *a733_encode_platform_jpeg_ops(void);
/* Actual lowest native policy; exact encoder/clock/PCK/GIC addresses only.
 * Require identity-mapped Device-nGnRnE. No authority is acquired here.
 * Writes accept width 4 only. */
int a733_encode_native_read(void *,uintptr_t,uint32_t *);
int a733_encode_native_write(void *,uintptr_t,unsigned,uint32_t);
#endif

/* H.264 binding of the same encoder sequencing and leases. Its job, prime
 * and copy contracts stay typed; no JPEG job is presented or fabricated. */
#ifndef SUNSTONE_A733_H264_ENCODE_PLATFORM_H
#define SUNSTONE_A733_H264_ENCODE_PLATFORM_H
#include "a733_video_h264_encode.h"
struct a733_h264_encode_platform_ops {
 int (*now_us)(void *,uint64_t *);
 int (*admit)(void *,const struct a733_encode_platform_config *,const struct a733_h264_encode_job *,uint64_t,uint64_t *);
 int (*memory)(void *,uint64_t,const struct a733_h264_encode_job *,uint64_t);
 int (*prime)(void *,uint64_t,unsigned,size_t,uint64_t);
 int (*copied)(void *,uint64_t,unsigned,size_t,size_t,uint64_t);
 int (*enter)(void *,uint64_t);
 int (*leave)(void *);
 int (*read)(void *,uintptr_t,uint32_t *);
 int (*write)(void *,uintptr_t,unsigned,uint32_t);
};
struct a733_h264_encode_platform {
 struct a733_video_encoder encoder;
 struct a733_h264_encode_platform_ops ops;
 struct a733_h264_encode_job job;
 void *cookie;
 unsigned bound;
};
int a733_h264_encode_platform_init(struct a733_h264_encode_platform *,
 const struct a733_encode_platform_config *,const struct a733_h264_encode_platform_ops *,void *);
const struct a733_h264_encode_ops *a733_encode_platform_h264_ops(void);
int a733_encode_h264_native_write(void *,uintptr_t,unsigned,uint32_t);
#endif
