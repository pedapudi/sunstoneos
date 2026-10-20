/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_ENCODE_PLATFORM_H
#define SUNSTONE_A733_VIDEO_ENCODE_PLATFORM_H
#include "a733_video_jpeg_encode.h"
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"
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
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_encode_platform {
    struct a733_encode_platform_config config;
    struct a733_encode_platform_ops ops;
    struct a733_jpeg_encode_job job;
    void *cookie;
    uint64_t token,deadline,last_now;
    a733_clock_mask_t shared;
    uint32_t selected_mhz[3];
    unsigned initialized,bound,checked,prepared,locked,failed;
};
int a733_encode_platform_scopes(const struct a733_encode_platform_config *,a733_clock_mask_t *);
int a733_encode_platform_init(struct a733_encode_platform *,const struct a733_encode_platform_config *,
    const struct a733_encode_platform_ops *,void *);
const struct a733_jpeg_encode_ops *a733_encode_platform_jpeg_ops(void);
/* Actual lowest native policy; exact encoder/clock/PCK/GIC addresses only.
 * Require identity-mapped Device-nGnRnE. No authority is acquired here. */
int a733_encode_native_read(void *,uintptr_t,uint32_t *);
int a733_encode_native_write(void *,uintptr_t,uint32_t);
#endif
