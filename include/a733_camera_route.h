/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_ROUTE_H
#define SUNSTONE_A733_CAMERA_ROUTE_H
#include <stdint.h>
/* Parser0/1 CH0 -> pass-through ISP1 CH0 -> VIPP0 CH0 -> DMA0.
 * Fixed progressive 640x480 YUV422, unity scaling, no overlays/conversion.
 * One serialized privileged owner; fresh zeroed storage; no callback reentry.
 */
enum a733_route_region { A733_ROUTE_TOP, A733_ROUTE_VIPP };
enum a733_route_state {
    A733_ROUTE_NEW,A733_ROUTE_ADMIT,A733_ROUTE_CHECK,A733_ROUTE_PUBLISH,
    A733_ROUTE_CONFIGURE,A733_ROUTE_ARMED,A733_ROUTE_STOP,A733_ROUTE_DISABLE,
    A733_ROUTE_STATUS,A733_ROUTE_DRAIN,A733_ROUTE_RELEASE,A733_ROUTE_CLOSED,
    A733_ROUTE_FAULT
};
struct a733_route_config {
    uint64_t parameter_address, mapped_bytes; /* Full span including guards. */
    uint8_t parser; /* 0 or 1, virtual channel 0. */
};
struct a733_route_ops {
    int (*now_us)(void *,uint64_t *);
    /* 1 complete, 0 pending, -1 uncertain failure. Token before side effects.
     * Reserve ISP1 CH0 crossbar input/output selections, all VIPP0 channels,
     * parameter span and clocks/reset. Prove the actual bus/IOMMU mapping and
     * read/prefetch guard size, cache isolation, local CSI clocks, independent
     * recovery and quiet producer. DMA0 must be safely owned by the same path.
     * Do not enable the sensor or parser as part of this admission.
     */
    int (*admit)(void *,const struct a733_route_config *,uint64_t,uint64_t *token);
    /* Copy this complete little-endian 1024-byte image into the admitted span,
     * clean caches and publish it with barriers. Retain the mapping until
     * drain/release. The immutable source image belongs to this controller.
     */
    int (*publish)(void *,uint64_t token,const uint8_t image[1024],uint64_t);
    /* Ordered aligned 32-bit Device accesses. TOP=0x05800800, VIPP=0x05910000.
     * MMIO callbacks return 1 or -1 and include write ordering barriers.
     */
    int (*read)(void *,uint64_t token,enum a733_route_region,uint16_t,uint32_t *,uint64_t);
    int (*write)(void *,uint64_t token,enum a733_route_region,uint16_t,uint32_t,uint64_t);
    /* Establish sensor/receiver quiet, completed SCAP and DMA capture, and no
     * upstream errors. Parameter-load status alone does not prove a frame.
     */
    int (*producer_stop)(void *,uint64_t token,uint64_t);
    /* Prove VIPP parameter reads/prefetch and downstream writes are drained. */
    int (*drain)(void *,uint64_t token,uint64_t);
    int (*release)(void *,uint64_t token,uint64_t);
};
struct a733_camera_route {
    struct a733_route_ops ops;
    struct a733_route_config config;
    void *context;
    uint64_t token,now,deadline;
    uint32_t saved,events;
    enum a733_route_state state;
    uint16_t index;
    uint8_t phase,initialized;
    uint8_t parameters[1024];
};
int a733_camera_route_init(struct a733_camera_route *,const struct a733_route_ops *,
                          void *,const struct a733_route_config *);
int a733_camera_route_open(struct a733_camera_route *,uint64_t deadline);
/* Only after one completed image. Errors/interruption require coordinator
 * recovery. This component does not start the producer or release frame data.
 */
int a733_camera_route_finish(struct a733_camera_route *,uint64_t deadline);
/* One callback per poll plus time checks; terminal faults retain all leases. */
int a733_camera_route_poll(struct a733_camera_route *);
#endif
