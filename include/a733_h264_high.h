/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef SUNSTONE_A733_H264_HIGH_H
#define SUNSTONE_A733_H264_HIGH_H
#include "a733_video_h264.h"
#define H264_HIGH_METADATA_BYTES 1048576u
#define H264_HIGH_SLOTS 5u
#define H264_HIGH_COPIES 5u
struct h264_high;
/* Immutable per-AU plan. Slot addresses are derived only from this geometry,
 * never from caller frame/POC hints. Both tables are rewritten each picture. */
struct h264_high_plan {
    struct a733_h264_picture picture;
    uint32_t slot,slots,y_step,c_step,mv_step,mv_field,picture_used;
    uint32_t gop,poc_bottom,reference,idr,mmco5,reorder,dpb_limit;
    uint32_t ref_count[2],lists[2][8]; /* each entry is a physical slot position */
    uint32_t valid[5],pocs[5][2];
    uint32_t weights[192],weight_denom,scaling[56],custom_scaling;
};
size_t h264_high_bytes(void);
struct h264_high *h264_high_init(void *,size_t);
int h264_high_prepare(struct h264_high *,const uint8_t *,size_t,
    struct h264_high_plan *,size_t *config_bytes);
/* Called only after exact native completion/copy and post-callback liveness.
 * Failure never changes committed reference/POC state. */
int h264_high_commit(struct h264_high *,const struct h264_high_plan *);
#endif
