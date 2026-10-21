/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_HEVC_H
#define SUNSTONE_A733_VIDEO_HEVC_H
#include <stddef.h>
#include <stdint.h>
#define A733_HEVC_INPUT_SLOT 262144u
#define A733_HEVC_OUTPUT_SLOT 462848u
#define A733_HEVC_MV_SLOT 16384u
#define A733_HEVC_NEIGHBOR_SLOT 815104u
#define A733_HEVC_ENTRY_SLOT 4096u
/* Visible NV12 picture: 640-byte luma rows, interleaved chroma at 307200. */
#define A733_HEVC_PICTURE_BYTES 460800u
#define A733_HEVC_CHROMA_OFFSET 307200u
/* Cedrus VE_DEC_H265_STATUS busy bits 8..15 and 17..21; bit 16 is MORE_DATA. */
#define A733_HEVC_BUSY_MASK UINT32_C(0x003eff00)
enum a733_hevc_buffer { HEVC_INPUT0,HEVC_INPUT1,HEVC_OUTPUT0,HEVC_OUTPUT1,
 HEVC_MV0,HEVC_MV1,HEVC_NEIGHBOR,HEVC_ENTRY,HEVC_BUFFERS };
struct a733_hevc_span { uint64_t iova,bytes; };
/* offset/bytes locate the slice NAL (after its start code) in the clip.
 * header_bits counts unescaped bits from the NAL header to the end of the
 * slice header, before alignment_bit_equal_to_one. The remaining words are
 * the packed H.265 engine registers of Cedrus cedrus_h265_setup. */
struct a733_hevc_picture {
 uint32_t offset,bytes,header_bits,poc,type,nal_type;
 uint32_t sps,pcm,pps0,pps1,slice0,slice1,slice2;
};
struct a733_hevc_plan { struct a733_hevc_picture pictures[2]; };
_Static_assert(sizeof(struct a733_hevc_plan)==26*sizeof(uint32_t),"HEVC plan holds only uint32_t words");
/* CPU header parsing only. The fixed mode is one VPS, SPS and PPS, an IDR
 * picture and one P picture referencing it: progressive Main 4:2:0 8-bit,
 * 640x480, CTB64, one independent slice per picture, no tiles, wavefronts,
 * scaling lists, PCM, long-term references or weighted prediction. ranges are
 * the session's two picture extents; the IDR range ends with its slice and the
 * P range starts with its start code. The whole plan validates before any
 * publication or device use. Failure leaves the output untouched. The
 * workspace is private, at least a733_hevc_parser_bytes() and serialized. */
#define A733_HEVC_PARSER_BYTES 589824u /* Bound on a733_hevc_parser_bytes(). */
size_t a733_hevc_parser_bytes(void);
int a733_hevc_parse(const uint8_t *,size_t,const uint64_t ranges[4],
 void *,size_t,struct a733_hevc_plan *);
/* Each picture's slice NAL sits at the start of its zero-padded INPUT slot.
 * Each picture writes its own OUTPUT and MV slot; NEIGHBOR and ENTRY are shared
 * scratch. Spans are page aligned, disjoint and below 4 GiB. */
struct a733_hevc_job {
 const uint8_t *input;size_t input_bytes;
 struct a733_hevc_span buffers[HEVC_BUFFERS];
 struct a733_hevc_plan plan;
};
/* acquire/prepare/copied return 1 complete, 0 pending, -1 failure; read and
 * write return exactly 1 after an ordered access. */
struct a733_hevc_ops {
 int (*now_us)(void *,uint64_t *);
 int (*acquire)(void *,const struct a733_hevc_job *,uint64_t,uint64_t *);
 int (*prepare)(void *,uint64_t,const struct a733_hevc_job *,uint64_t);
 int (*read)(void *,uint64_t,uint32_t,uint32_t *,uint64_t);
 int (*write)(void *,uint64_t,uint32_t,uint32_t,uint64_t);
 /* Verify guarded copied output and fresh IOMMU fault state. Return 1 permits
  * publication only. Every DMA extent, reference and lease stays retained. */
 int (*copied)(void *,uint64_t,unsigned,uint64_t);
};
enum a733_hevc_state { HEVC_ACQUIRE,HEVC_IDENTIFY,HEVC_PREPARE,HEVC_CHECK,
 HEVC_PROGRAM,HEVC_INIT,HEVC_FLUSH,HEVC_FLUSH_WAIT,HEVC_TRIGGER,HEVC_WAIT,
 HEVC_COUNT,HEVC_DISABLE,HEVC_ACK_STATUS,HEVC_COPY,HEVC_COPIED,HEVC_FAILED };
struct a733_hevc_run {
 struct a733_hevc_job job;struct a733_hevc_ops ops;void *context;
 uint64_t token,deadline,last_now;uint32_t status,identity;
 unsigned picture,index,flushed,flush_bits;
 enum a733_hevc_state state;
};
/* Fresh storage, single privileged serialized caller. Validates spans and the
 * plan; no callback runs. Returns 1 ready, 0 refused. */
int a733_hevc_begin(struct a733_hevc_run *,const struct a733_hevc_ops *,void *,
 const struct a733_hevc_job *,uint64_t);
/* At most one external operation per poll after a monotonic time check.
 * Returns 0 pending, 1 copied current picture, -1 retained failure. Success
 * requires the finish bit without error, data request or timeout bits, idle
 * busy fields and all 80 CTBs. Nothing is released on success or failure. */
int a733_hevc_poll(struct a733_hevc_run *);
/* Only immutable copied frame0 plus caller ACK permits programming frame1.
 * ACK never releases reference0, input, tables, outputs or shared scratch. */
int a733_hevc_next(struct a733_hevc_run *);
#endif
