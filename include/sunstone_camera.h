/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_CAMERA_H
#define SUNSTONE_CAMERA_H
#include <stddef.h>
#include <stdint.h>
/* Shared application contract for CSI and USB cameras. Formats use fourcc.
 * Buffer pointers remain valid until release; a failed provider retains its
 * resources. Each operation uses one serialized owner and an absolute deadline
 * in microseconds. Return 1 ready, 0 pending/no frame, -1 retained fault.
 * enumerate is read-only and returns 0 at the end of the mode inventory. */
#define SUNSTONE_CAMERA_NV16 UINT32_C(0x3631564e)
#define SUNSTONE_CAMERA_YUYV UINT32_C(0x56595559)
#define SUNSTONE_CAMERA_UYVY UINT32_C(0x59565955)
#define SUNSTONE_CAMERA_MJPEG UINT32_C(0x47504a4d)
struct sunstone_camera_mode { uint32_t width,height,format,interval_num,interval_den; };
/* CSI NV16 uses plane/bytes/stride and no spans. Packed or encoded USB
 * frames use spans, NULL planes and bytes[0] for total frame size. A span's
 * lease is a provider-private cookie; release only the complete frame token. */
struct sunstone_camera_span { const uint8_t *data; size_t bytes; void *lease; };
struct sunstone_camera_frame {
    const uint8_t *plane[2];
    size_t bytes[2];
    uint32_t stride[2];
    const struct sunstone_camera_span *spans;
    size_t span_count;
    uint64_t sequence;
    uint64_t token;
};
struct sunstone_camera_ops {
    int (*enumerate)(void *,unsigned,struct sunstone_camera_mode *);
    int (*start)(void *,const struct sunstone_camera_mode *,uint64_t);
    int (*next)(void *,struct sunstone_camera_frame *,uint64_t);
    int (*release)(void *,uint64_t,uint64_t);
    int (*stop)(void *,uint64_t);
};
struct sunstone_camera { const struct sunstone_camera_ops *ops; void *context; };
#endif
