/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CSI_RECEIVER_H
#define SUNSTONE_A733_CSI_RECEIVER_H
#include <stdint.h>
#include "a733_camera_common.h"

/* Local D-PHY, packet-port and parser controller for two-lane OV5640 YUV422.
 * One privileged serialized owner. Fresh zeroed storage; no callback reentry.
 * All callbacks are bounded by deadline (absolute monotonic microseconds).
 * Returns: 1 complete, 0 pending for admission/producer/drain/release only,
 * -1 failure (possibly after side effects). Faults retain all ownership.
 */
enum a733_csi_region { A733_CSI_TOP, A733_CSI_PHY, A733_CSI_PORT, A733_CSI_PARSER };
enum a733_csi_state {
    A733_CSI_NEW, A733_CSI_ADMIT, A733_CSI_PRECHECK, A733_CSI_CONFIGURE,
    A733_CSI_ARMED, A733_CSI_SAMPLE, A733_CSI_PRODUCER_STOP,
    A733_CSI_DISABLE, A733_CSI_DRAIN, A733_CSI_RELEASE,
    A733_CSI_CLOSED, A733_CSI_FAULT, A733_CSI_REARM
};
struct a733_csi_config {
    /* CAM1=port/parser 0, CAM2=1. Native routing must verify this mapping.
     * Settle is an explicitly admitted PHY timing count, 1..255; it is not
     * inferred from a stale hardware counter. deskew is 1..31, source default
     * 2 on port 0 and 7 on port 1. Qualification remains platform-specific.
     */
    uint8_t port, parser, settle, deskew;
    uint32_t top_hz;
};
struct a733_csi_ops {
    int (*now_us)(void *, uint64_t *);
    /* Reserve local PHY/port/parser and all downstream clocks/routes/buffers.
     * Ready proves independent recovery; global PHY power/calibration/reset;
     * stable top clock; sensor quiet; local reset state; valid downstream
     * capture path prepared; pin voltages and physical routing checked.
     * Publish nonzero token before any partial mutation; continue same token.
     * This controller never initializes global analog power/calibration.
     */
    int (*admit)(void *, const struct a733_csi_config *, uint64_t deadline,
                 uint64_t *token);
    /* The mapping binds TOP=0x05810000, PHY=0x05810100+port*0x100,
     * PORT=0x05811000+port*0x400, PARSER=0x05820000+parser*0x1000.
     * Accesses are aligned 32-bit Device accesses. Successful writes include
     * ordering barriers; exclusive leases cover every local register writer.
     */
    int (*read)(void *, uint64_t token, enum a733_csi_region, uint16_t offset,
                uint32_t *value, uint64_t deadline);
    int (*write)(void *, uint64_t token, enum a733_csi_region, uint16_t offset,
                 uint32_t value, uint64_t deadline);
    /* Stop the sensor and establish no new packets before receiver disable. */
    int (*producer_stop)(void *, uint64_t token, uint64_t deadline);
    /* After local disable, independently isolate/drain downstream DMA/ISP.
     * Local enable-bit readback and CSI frame-end do not establish bus drain.
     */
    int (*drain)(void *, uint64_t token, uint64_t deadline);
    /* Release only verified quiescent resources; preserve other PHY users. */
    int (*release)(void *, uint64_t token, uint64_t deadline);
};
struct a733_csi_receiver {
    struct a733_csi_ops ops;
    struct a733_csi_config config;
    void *context;
    uint64_t token, deadline, now;
    uint32_t saved, mipi_events, parser_events;
    struct a733_camera_format format;
    enum a733_csi_state state;
    uint16_t index;
    uint8_t phase, initialized, image_mode;
};
/* The 640x480 UYVY profile. */
int a733_csi_init(struct a733_csi_receiver *, const struct a733_csi_ops *,
                  void *context, const struct a733_csi_config *);
/* Any valid YUV422 format: the packet port and parser take its byte order
 * and the parser its output size. */
int a733_csi_init_format(struct a733_csi_receiver *, const struct a733_csi_ops *,
                         void *context, const struct a733_csi_config *,
                         const struct a733_camera_format *);
int a733_csi_open(struct a733_csi_receiver *, uint64_t deadline);
/* Prepare SCAP instead of continuous video. Producer must stay quiet through
 * ARMED. SCAP captures one frame and self-clears; never re-arm automatically.
 */
int a733_csi_open_image(struct a733_csi_receiver *, uint64_t deadline);
/* Explicit SCAP rearm for the next image, only from ARMED in image mode while
 * the caller keeps the producer stopped. Packet-port and parser status must
 * hold no flag at all since the last sample acknowledged them, so no packet
 * arrived in between, and a fresh parser read must show every capture field
 * clear. SCAP is then written and read back. Any other observation faults
 * without acknowledgement; the caller supplies the quiet interval.
 */
int a733_csi_rearm_image(struct a733_csi_receiver *, uint64_t deadline);
/* One status sample latches observed packet/parser flags. Any documented
 * packet error (including corrected ECC warning) or parser error faults.
 * A frame-end flag alone does not count or complete a DMA frame.
 */
int a733_csi_sample(struct a733_csi_receiver *, uint64_t deadline);
int a733_csi_close(struct a733_csi_receiver *, uint64_t deadline);
/* At most one read/write/platform callback per poll plus pre/post time checks.
 * Every configuration RMW uses read, write and verification polls.
 */
int a733_csi_poll(struct a733_csi_receiver *);
#endif
