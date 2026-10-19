/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_H264_H
#define SUNSTONE_A733_VIDEO_H264_H
#include <stddef.h>
#include <stdint.h>

#define A733_H264_INPUT_BYTES 8192u
#define A733_H264_PICTURES 2u
#define A733_H264_SLOT_BYTES 4096u
#define A733_H264_VGA_INPUT_BYTES 524288u
#define A733_H264_VGA_OUTPUT_BYTES 460800u
#define A733_H264_VGA_MV_FIELD_BYTES 19456u
#define A733_H264_VGA_MV_SLOT_BYTES 40960u
#define A733_H264_FULLHD_INPUT_BYTES 4194304u
#define A733_H264_FULLHD_OUTPUT_BYTES 3110400u
#define A733_H264_FULLHD_MV_FIELD_BYTES 131072u
#define A733_H264_FULLHD_MV_SLOT_BYTES 262144u
/* Historical PCM fixture length, not a stream admission bound. */
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
enum a733_h264_profile { A733_H264_LEGACY, A733_H264_VGA_STREAM, A733_H264_FULLHD_STREAM };
struct a733_h264_job {
    const uint8_t *input;
    size_t input_bytes;
    struct a733_h264_span buffers[A733_H264_BUFFERS];
    enum a733_h264_profile profile;
};
/* Bounded AnnexB mode: one SPS/PPS, one IDR I and optionally one P slice.
 * Baseline8-bit4:2:0, progressive macroblock-aligned16..64 by16..32,
 * CAVLC, IDR POC0/2, IDR+P POC2 and one retained short-term reference.
 * No FMO, cropping, HRD, weighted P, list changes, MMCO or secondary outputs.
 * Macroblock prediction, residual transforms and entropy decoding run on VE.
 * Header validation does not establish that slice data forms a complete picture.
 * Provider acquisition freezes the CPU alias and reserves masters6/7, IRQ81,
 * clocks/reset/power and every physical alias before any device programming.
 */
struct a733_h264_picture {
    uint32_t width, height, slice_offset, slice_bytes, header_bits;
    uint32_t sps, pps, shs, shs2, qp;
    uint32_t frame_num, poc, slice_type;
    uint32_t visible_width,visible_height,crop_left,crop_top,crop_right,crop_bottom;
};
struct a733_h264_sequence {
    uint32_t count;
    struct a733_h264_picture pictures[A733_H264_PICTURES];
};
int a733_h264_parse_sequence(const uint8_t *,size_t,struct a733_h264_sequence *);
/* No MMIO. Failure leaves the output untouched. Bounds apply before every read.
 * Adapted syntax/field order from pinned CedarC H264DecodeSps/Pps/SliceHeader;
 * bounded byte reader replaces its hardware GetBits callbacks. */
int a733_h264_parse(const uint8_t *, size_t, struct a733_h264_picture *);
/* VGA AUs repeat identical SPS/PPS before one IDR/P slice; frame_num0..15.
 * config_bytes returns the immutable SPS/PPS prefix for exact comparison. */
int a733_h264_parse_vga(const uint8_t *,size_t,unsigned,
    struct a733_h264_picture *,size_t *config_bytes);
int a733_h264_parse_stream(enum a733_h264_profile,const uint8_t *,size_t,unsigned,
    struct a733_h264_picture *,size_t *config_bytes);
struct a733_h264_ops {
    int (*now_us)(void *, uint64_t *);
    int (*acquire)(void *, const struct a733_h264_job *, uint64_t, uint64_t *);
    /* Publish each encoded slice at its4096-byte INPUT slot, preserve NAL/EPB
     * bytes and zero-pad the slot. Retain immutable CPU stream and DMA copies.
     * Pictures own separate output/MV slots; shared context scratch persists.
     * Cedrus requires VLD_OFFSET0 to avoid offset corruption. */
    int (*prepare)(void *, uint64_t, const struct a733_h264_job *, uint64_t);
    int (*read)(void *, uint64_t, uint32_t, uint32_t *, uint64_t);
    int (*write)(void *, uint64_t, uint32_t, uint32_t, uint64_t);
    /* Verify owned private copied bytes; an optional qualification oracle,
     * fault/status/guard checks and live lifetime govern publication. Retain DMA;
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
    A733_H264_COPY, A733_H264_MB_COUNT, A733_H264_COPIED, A733_H264_FAULT };
struct a733_h264_run {
    struct a733_h264_ops ops;
    struct a733_h264_job job;
    void *context;
    uint64_t token, deadline, last_now, version;
    uint32_t identity_high, status, format;
    unsigned index, flushed, flush_bits, picture_index;
    size_t frame_bytes;
    struct a733_h264_sequence sequence;
    struct a733_h264_picture picture;
    enum a733_h264_state state;
};
/* Fresh zeroed storage, single privileged serialized caller, no reentry.
 * Stream headers are parsed after acquisition. Inclusive VLD end is the
 * padded4096-byte slot; slice length is parsed and VLD offset remains zero.
 * Minimum scratch extents
 * are source-derived; they do not prove physical burst bounds. All spans
 * below4GiB, page-aligned and disjoint; neighbor base aligned16KiB.
 * Terminal failure/success never authorizes unmapping, reset, reuse or release.
 */
int a733_h264_begin(struct a733_h264_run *, const struct a733_h264_ops *, void *,
                   const struct a733_h264_job *, uint64_t deadline_us);
/* At most one external operation per poll, plus monotonic time checks.
 * Returns0 pending,1 immutable verified private copy,-1 retained failure. */
int a733_h264_poll(struct a733_h264_run *);
/* Fixed VGA pool. Input capacity is immutable; frame_bytes is the current
 * authenticated AU length. Only COPIED permits a healthy next-frame submission.
 * Caller retains immutable AU until COPIED and owns its refill/copy ACK protocol.
 * No failure permits another submission, reset, unmap or allocation release. */
int a733_h264_vga_begin(struct a733_h264_run *,const struct a733_h264_ops *,void *,
    const struct a733_h264_job *,size_t frame_bytes,uint64_t deadline_us);
int a733_h264_vga_next(struct a733_h264_run *,size_t frame_bytes,unsigned frame_num);
int a733_h264_stream_begin(struct a733_h264_run *,const struct a733_h264_ops *,void *,
    const struct a733_h264_job *,size_t frame_bytes,uint64_t deadline_us);
int a733_h264_stream_next(struct a733_h264_run *,size_t frame_bytes,unsigned frame_num);
#endif
