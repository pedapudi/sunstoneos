/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_H264_H
#define SUNSTONE_A733_VIDEO_H264_H
#include <stddef.h>
#include <stdint.h>

#define A733_H264_SLICE_BYTES 2320u
#define A733_H264_OUTPUT_BYTES 2304u
#define A733_H264_PICTURE_INFO_BYTES 0x20800u
#define A733_H264_NEIGHBOR_BYTES 0xc000u
/* H.264 pipeline8..15, intram20, transform21, bitstreamDMA22, writeback23.
 * Pinned h264_hal.h FUNC_STATUS fields; VP8/AVS/startcode modes stay disabled. */
#define A733_H264_BUSY_MASK UINT32_C(0x00f0ff00)
enum a733_h264_buffer { A733_H264_INPUT, A733_H264_Y, A733_H264_C,
    A733_H264_PICTURE_INFO, A733_H264_MV, A733_H264_NEIGHBOR, A733_H264_BUFFERS };
struct a733_h264_span { uint64_t iova, bytes; };
struct a733_h264_job {
    const uint8_t *input;
    struct a733_h264_span buffers[A733_H264_BUFFERS];
};
/* One exact Baseline progressive48x32 IDR/I_PCM fixture. No general H.264
 * parser, DPB, scaling, secondary output, LBC, AFBC or VCU operation.
 * Provider acquisition freezes the CPU alias and reserves masters6/7, IRQ81,
 * clocks/reset/power and every physical alias before any device programming.
 */
struct a733_h264_ops {
    int (*now_us)(void *, uint64_t *);
    int (*acquire)(void *, const struct a733_h264_job *, uint64_t, uint64_t *);
    int (*prepare)(void *, uint64_t, const struct a733_h264_job *, uint64_t);
    int (*read)(void *, uint64_t, uint32_t, uint32_t *, uint64_t);
    int (*write)(void *, uint64_t, uint32_t, uint32_t, uint64_t);
    /* Verify a private copied result against the independent pixel oracle,
     * fault/status/guard checks and live lifetime. Retain all DMA ownership;
     * successful CPU copying does not establish producer or transaction drain.
     * acquire/prepare/copied return1 complete,0 pending,-1 failure.
     * read/write return exactly1 after ordered access, otherwise failure.
     */
    int (*copied)(void *, uint64_t, uint64_t);
};
enum a733_h264_state { A733_H264_NEW, A733_H264_ACQUIRE, A733_H264_HASH,
    A733_H264_IDENTIFY, A733_H264_PREPARE, A733_H264_CHECK,
    A733_H264_PROGRAM, A733_H264_INIT, A733_H264_FLUSH, A733_H264_HEADER_WAIT,
    A733_H264_TRIGGER, A733_H264_WAIT, A733_H264_DISABLE, A733_H264_ACK,
    A733_H264_COPY, A733_H264_COPIED, A733_H264_FAULT };
struct a733_h264_run {
    struct a733_h264_ops ops;
    struct a733_h264_job job;
    void *context;
    uint64_t token, deadline, last_now, version;
    uint32_t identity_high, status, format;
    unsigned index;
    enum a733_h264_state state;
};
/* Fresh zeroed storage, single privileged serialized caller, no reentry.
 * Complete input hash is checked after acquisition. Inclusive VLD end is the
 * padded4096-byte span, not the2320-byte bitstream. Minimum scratch extents
 * are source-derived; they do not prove physical burst bounds. All spans
 * below4GiB, page-aligned and disjoint; neighbor base aligned16KiB.
 * Terminal failure/success never authorizes unmapping, reset, reuse or release.
 */
int a733_h264_begin(struct a733_h264_run *, const struct a733_h264_ops *, void *,
                   const struct a733_h264_job *, uint64_t deadline_us);
/* At most one external operation per poll, plus monotonic time checks.
 * Returns0 pending,1 immutable verified private copy,-1 retained failure. */
int a733_h264_poll(struct a733_h264_run *);
#endif
