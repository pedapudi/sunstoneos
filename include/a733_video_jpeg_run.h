/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_JPEG_RUN_H
#define SUNSTONE_A733_VIDEO_JPEG_RUN_H
#include "a733_video_jpeg.h"

#define A733_VIDEO_DEC_BASE UINT64_C(0x01c0e000)
#define A733_VIDEO_DEC_IRQ 81u
#define A733_VIDEO_DEC_MASTERS 0x00c0u
struct a733_video_jpeg_job {
    const uint8_t *input; /* CPU alias of input_iova, frozen by acquire. */
    uint32_t file_bytes;
    uint64_t input_iova, input_bytes;
    uint64_t luma_iova, luma_bytes, chroma_iova, chroma_bytes;
};
struct a733_video_jpeg_ops {
    int (*now_us)(void *, uint64_t *);
    /* Set a stable nonzero token before acquiring anything. Own the job,
     * input bytes and CPU aliases, both decoder memory masters, shared VE
     * registers, reset/power/clock leases and IRQ81. Establish cold handoff and
     * qualified independent recovery for the legacy drain path. The explicit
     * retained-copy entry may instead authenticate supervised whole-board power
     * removal with permanent device memory/token retention; initial isolation
     * and prior drain are still mandatory. Exclude every conflicting user. Keep all
     * leases after every return, including error; no callback may reclaim.
     * Permit only clock/power prerequisites for identity reads before admission;
     * do not reset/select/program engines or alter translations here.
     * Return 1 complete, 0 pending, -1 failure (other values are failures).
     */
    int (*acquire)(void *, const struct a733_video_jpeg_job *, uint64_t deadline,
                   uint64_t *token);
    /* After parse and supported-identity admission: establish both masters' translations and complete physical
     * access extents, prohibit physical aliases and MMIO/CPU metadata mappings,
     * publish input/prepare output caches, cold-reset/select the MPEG engine
     * with VCU/scale/rotation disabled. Preserve unrelated shared state.
     * stride/rows are padded to 32; YU12 chroma is contiguous Cb then Cr.
     * Input end is inclusive. Merely supplying the minimum plane lengths is
     * insufficient evidence of device access bounds. The fixed JPEG application
     * supplies retained guarded mappings; broader profiles still require their
     * own memory authority. Return 1/0/-1, retain all ownership.
     */
    int (*prepare)(void *, uint64_t token, const struct a733_video_jpeg_job *,
                   const struct a733_video_jpeg *, uint32_t stride, uint32_t rows,
                   uint64_t deadline);
    /* Ordered Device accesses relative to A733_VIDEO_DEC_BASE. width=1 or 4.
     * Successful write includes completion barrier. Return exactly 1 or -1.
     */
    int (*read)(void *, uint64_t token, uint32_t off, uint32_t *, uint64_t deadline);
    int (*write)(void *, uint64_t token, uint32_t off, unsigned width,
                 uint32_t value, uint64_t deadline);
    /* Require stopped producers, completion of accepted external transactions,
     * no IOMMU fault on either master, and output cache invalidation/barrier.
     * A codec status bit alone proves none of these. Retain reservations and
     * leases even on success; DONE permits the caller to consume output under
     * the retained lease. Return 1/0/-1. No native provider exists yet.
     */
    int (*finish)(void *, uint64_t token, uint64_t deadline);

};
enum a733_video_jpeg_state {
    A733_VIDEO_JPEG_NEW, A733_VIDEO_JPEG_ACQUIRE, A733_VIDEO_JPEG_PARSE,
    A733_VIDEO_JPEG_IDENTIFY, A733_VIDEO_JPEG_PREPARE, A733_VIDEO_JPEG_CHECK, A733_VIDEO_JPEG_PROGRAM,
    A733_VIDEO_JPEG_WAIT, A733_VIDEO_JPEG_DISABLE, A733_VIDEO_JPEG_ACK,
    A733_VIDEO_JPEG_FINISH, A733_VIDEO_JPEG_DONE, A733_VIDEO_JPEG_FAULT, A733_VIDEO_JPEG_COPIED
};
struct a733_video_jpeg_run {
    struct a733_video_jpeg_ops ops;
    struct a733_video_jpeg_job job;
    struct a733_video_jpeg picture;
    struct a733_video_jpeg_tables tables;
    void *context;
    uint64_t token, deadline, now, version;
    uint32_t stride, rows, status, format_old, version_high;
    unsigned index,retained_readout;
    int (*retained_copy)(void *,uint64_t,uint64_t);
    enum a733_video_jpeg_state state;
};
/* Fresh zeroed storage, one privileged serialized caller, no callback reentry.
 * One job per object. Restricts execution to <=1920x1080 and <=16MiB input,
 * 32-bit IOVAs, planar YU12, no scaling/rotation/VCU. Copies job/ops.
 * Does not authorize physical execution without the documented providers.
 */
int a733_video_jpeg_begin(struct a733_video_jpeg_run *,
    const struct a733_video_jpeg_ops *, void *, const struct a733_video_jpeg_job *,
    uint64_t deadline_us);
/* Same identity/program path, with distinct COPIED terminal state. Requires
 * retained_copy; never calls finish, never reports DONE or permits DMA reuse. */
int a733_video_jpeg_begin_retained(struct a733_video_jpeg_run *,
    const struct a733_video_jpeg_ops *,void *,const struct a733_video_jpeg_job *,uint64_t,
    int (*copy_verified)(void *,uint64_t,uint64_t));
/* At most one external operation except pre/post monotonic time checks per poll.
 * Returns 0 pending, 1 DONE, -1 retained FAULT. Timeout/backward time/IO failure
 * never releases memory or resets hardware. Initialized state must not be edited.
 */
int a733_video_jpeg_poll(struct a733_video_jpeg_run *);
#endif
