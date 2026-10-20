/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_H264_ENCODE_H
#define SUNSTONE_A733_VIDEO_H264_ENCODE_H
#include <stddef.h>
#include <stdint.h>
#define A733_H264_ENCODE_FRAMES 2u
#define A733_H264_ENCODE_INPUT_FRAME 460800u
#define A733_H264_ENCODE_INPUT_PADDED 462848u
#define A733_H264_ENCODE_RING_BYTES 4194304u
#define A733_H264_ENCODE_RECON_BYTES 491520u
#define A733_H264_ENCODE_RECON_Y 327680u
#define A733_H264_ENCODE_AUX_BYTES 12288u
#define A733_H264_ENCODE_LINE_BYTES 8192u
#define A733_H264_ENCODE_MISC_BYTES 4096u
#define A733_H264_ENCODE_OUTPUT_BYTES 1048576u
/* Device reconstruction/auxiliary formats remain opaque. VGA geometry is
 * derived from H264InitVer2; these extents are not linear copied NV12 output. */
enum a733_h264_encode_buffer { A733_H264_ENCODE_INPUT0,A733_H264_ENCODE_INPUT1,
 A733_H264_ENCODE_RING,A733_H264_ENCODE_RECON0,A733_H264_ENCODE_RECON1,
 A733_H264_ENCODE_AUX0,A733_H264_ENCODE_AUX1,A733_H264_ENCODE_LINE0,
 A733_H264_ENCODE_LINE1,A733_H264_ENCODE_MISC,A733_H264_ENCODE_BUFFERS };
struct a733_h264_encode_span {uint64_t iova,bytes;};
struct a733_h264_encode_job {
    const uint8_t *input[2];
    struct a733_h264_encode_span buffers[A733_H264_ENCODE_BUFFERS];
};
/* Two frames only; no release/rewind/ring reuse. Each successful copied output
 * remains immutable while the next frame uses the retained reconstructed ref.
 * Failure freezes all spans and speculative reference state through power removal. */
struct a733_h264_encode_ops {
    int (*now_us)(void *,uint64_t *);
    int (*acquire)(void *,const struct a733_h264_encode_job *,uint64_t,uint64_t *);
    int (*prepare)(void *,uint64_t,const struct a733_h264_encode_job *,uint64_t);
    int (*prime)(void *,uint64_t,unsigned,size_t,uint64_t);
    int (*read)(void *,uint64_t,uint32_t,uint32_t *,uint64_t);
    int (*write)(void *,uint64_t,uint32_t,uint32_t,uint64_t);
    int (*copied)(void *,uint64_t,unsigned,size_t,size_t,uint64_t);
};
enum a733_h264_encode_state { A733_H264_ENCODE_NEW,A733_H264_ENCODE_ACQUIRE,
 A733_H264_ENCODE_PREPARE,A733_H264_ENCODE_PRIME,A733_H264_ENCODE_RESET_READ,
 A733_H264_ENCODE_RESET_HIGH,A733_H264_ENCODE_RESET_REREAD,A733_H264_ENCODE_RESET_LOW,
 A733_H264_ENCODE_ENABLE_READ,A733_H264_ENCODE_ENABLE6,A733_H264_ENCODE_ENABLE_REREAD,
 A733_H264_ENCODE_ENABLE7,A733_H264_ENCODE_ENABLE_CHECK,A733_H264_ENCODE_PROGRAM,
 A733_H264_ENCODE_STALE,A733_H264_ENCODE_CLEAR,A733_H264_ENCODE_CLEAN,
 A733_H264_ENCODE_HEADER_READY,A733_H264_ENCODE_HEADER_DATA,A733_H264_ENCODE_HEADER_COMMAND,
 A733_H264_ENCODE_TRIGGER,A733_H264_ENCODE_WAIT,A733_H264_ENCODE_COUNT,
 A733_H264_ENCODE_COPY,A733_H264_ENCODE_COPIED,A733_H264_ENCODE_FAULT };
struct a733_h264_encode_run {
    struct a733_h264_encode_ops ops;
    struct a733_h264_encode_job job;
    void *context;
    uint64_t token,deadline,last_now;
    size_t cursor,end[2];
    uint32_t value,status;
    unsigned ordinal,index;
    enum a733_h264_encode_state state;
};
extern const uint8_t a733_h264_encode_sps_pps[22];
const uint8_t *a733_h264_encode_prefix(unsigned,size_t *);
int a733_h264_encode_begin(struct a733_h264_encode_run *,const struct a733_h264_encode_ops *,
    void *,const struct a733_h264_encode_job *,uint64_t);
int a733_h264_encode_poll(struct a733_h264_encode_run *);
/* Only copied picture0 permits submission of picture1; caller separately
 * owns copied-output ACK/backpressure. No wrap, retry, reset-on-fault or reuse. */
int a733_h264_encode_next(struct a733_h264_encode_run *);
#endif
