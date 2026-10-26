/* SPDX-License-Identifier: MIT */
#ifndef SUNSTONE_PVR_GPU_CLOCK_H
#define SUNSTONE_PVR_GPU_CLOCK_H
#include "pvr_gpu_control.h"

/* Live clock handshake, serialized by the existing GPU worker. The owner
 * closes job admission and completes all jobs before hold; it changes the
 * clock/rail only in HELD, then resumes with the actual programmed rate. */
struct pvr_gpu_clock_ops {
    int (*now)(void *,uint64_t *);
    int (*sync_write)(void *,uint32_t);
    int (*sync_read)(void *,uint32_t *);
    int (*power_state)(void *,uint32_t *);
    /* Runtime frequency and complete time-correlation record before seq. */
    int (*publish)(void *,uint32_t);
};
enum pvr_gpu_clock_phase { PVR_GPU_CLOCK_NEW, PVR_GPU_CLOCK_CLEAR_IDLE,
    PVR_GPU_CLOCK_SEND_IDLE, PVR_GPU_CLOCK_WAIT_IDLE, PVR_GPU_CLOCK_HELD,
    PVR_GPU_CLOCK_PUBLISH, PVR_GPU_CLOCK_NOTIFY, PVR_GPU_CLOCK_CLEAR_CANCEL,
    PVR_GPU_CLOCK_SEND_CANCEL, PVR_GPU_CLOCK_WAIT_CANCEL, PVR_GPU_CLOCK_READY,
    PVR_GPU_CLOCK_FAILED };
enum pvr_gpu_clock_error { PVR_GPU_CLOCK_NO_ERROR, PVR_GPU_CLOCK_TIME,
    PVR_GPU_CLOCK_CONTROL, PVR_GPU_CLOCK_IO, PVR_GPU_CLOCK_ACK,
    PVR_GPU_CLOCK_COMMAND, PVR_GPU_CLOCK_STATE };
struct pvr_gpu_clock {
    struct pvr_gpu_control *control;
    struct pvr_gpu_clock_ops ops;
    void *context;
    uint64_t deadline, limit, last_now;
    struct pvr_ccb_token token[3];
    uint32_t status[3], phase, error, rate, published;
};
/* Captured deadline covers the complete idle request; limit covers the
 * following owner actions and resume, including time between requests. */
int pvr_gpu_clock_hold(struct pvr_gpu_clock *,struct pvr_gpu_control *,
    const struct pvr_gpu_clock_ops *,void *,uint64_t deadline,uint64_t limit);
int pvr_gpu_clock_resume(struct pvr_gpu_clock *,uint32_t rate,uint64_t deadline);
/* 0 pending, 1 HELD or READY, -1 retained failure. At most one bounded control
 * poll (four events) and one handshake step. No backing or lease release. */
int pvr_gpu_clock_poll(struct pvr_gpu_clock *);
#endif
