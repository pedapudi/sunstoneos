/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_UVC_CAMERA_H
#define SUNSTONE_UVC_CAMERA_H
#include "sunstone_usb_uvc.h"
/* The USB owner installs these once. start uses the transport's asynchronous
 * probe/commit/SET_INTERFACE path; stop proves endpoint retirement. Neither
 * callback may reinitialize the stream while an application holds a frame.
 * Return 1 complete, 0 pending, -1 fault; deadlines are absolute microseconds.
 * now reads the same monotonic clock used by start/stop and returns 1 on success.
 * Repeat a pending start/stop with its original deadline. Expired operations
 * retain ownership; release can retry with a later deadline. */
struct sunstone_uvc_camera_transport {
    int (*start)(void *,const struct sunstone_uvc_mode *,uint64_t);
    int (*stop)(void *,uint64_t);
    void *context;
    int (*now)(void *,uint64_t *);
};
struct sunstone_uvc_camera {
    struct sunstone_camera camera;
    struct sunstone_uvc *stream;
    const struct sunstone_uvc_catalog *catalog;
    struct sunstone_uvc_camera_transport transport;
    struct sunstone_uvc_mode active;
    struct {
        const struct sunstone_uvc_frame *frame;
        uint64_t generation,sequence,token;
    } held[SUNSTONE_UVC_FRAMES];
    uint64_t token;
    uint64_t start_deadline,stop_deadline;
    uint8_t started,starting,stopping,fault;
};
/* Fresh zeroed caller-owned storage. Catalog and stream stay reserved through
 * every release, including after detach. No allocation or service-task hop. */
int sunstone_uvc_camera_init(struct sunstone_uvc_camera *,struct sunstone_uvc *,
    const struct sunstone_uvc_catalog *,const struct sunstone_uvc_camera_transport *);
int sunstone_uvc_camera_mode(const struct sunstone_uvc_mode *,struct sunstone_camera_mode *);
int sunstone_uvc_camera_view(const struct sunstone_uvc_frame *,
    const struct sunstone_uvc_mode *,uint64_t,struct sunstone_camera_frame *);
#endif
