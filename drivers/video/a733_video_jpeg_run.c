/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_jpeg_run.h"
#include "a733_range.h"

static int span(uint64_t a, uint64_t n, unsigned align)
{
    return a >= 0x1000 && !(a & (align - 1u)) && n && n <= UINT64_C(0x100000000) - a &&
           a < UINT64_C(0x100000000);
}
static int fault(struct a733_video_jpeg_run *r)
{
    r->state = A733_VIDEO_JPEG_FAULT;
    return -1;
}
static int time_ok(struct a733_video_jpeg_run *r)
{
    uint64_t now;
    if (r->ops.now_us(r->context, &now) != 1 || now < r->now || now >= r->deadline)
        return 0;
    r->now = now;
    return 1;
}
int a733_video_jpeg_begin(struct a733_video_jpeg_run *r,
    const struct a733_video_jpeg_ops *ops, void *ctx,
    const struct a733_video_jpeg_job *j, uint64_t deadline)
{
    if (!r || !ops || !j || r->state != A733_VIDEO_JPEG_NEW || !deadline ||
        !ops->now_us || !ops->acquire || !ops->prepare || !ops->read ||
        !ops->write || !ops->finish || !j->input || !j->file_bytes ||
        j->input_bytes > 0x1000000 || j->file_bytes > j->input_bytes ||
        !span(j->input_iova,j->input_bytes,16) ||
        !span(j->luma_iova,j->luma_bytes,32) ||
        !span(j->chroma_iova,j->chroma_bytes,32) ||
        a733_extents_overlap_prechecked(j->input_iova,j->input_bytes,j->luma_iova,j->luma_bytes) ||
        a733_extents_overlap_prechecked(j->input_iova,j->input_bytes,j->chroma_iova,j->chroma_bytes) ||
        a733_extents_overlap_prechecked(j->luma_iova,j->luma_bytes,j->chroma_iova,j->chroma_bytes)) return 0;
    r->ops = *ops; r->job = *j; r->context = ctx; r->deadline = deadline;
    r->state = A733_VIDEO_JPEG_ACQUIRE;
    return 1;
}

int a733_video_jpeg_begin_retained(struct a733_video_jpeg_run *r,
    const struct a733_video_jpeg_ops *ops,void *ctx,
    const struct a733_video_jpeg_job *job,uint64_t deadline,
    int (*copy_verified)(void *,uint64_t,uint64_t))
{
    if(!copy_verified)return 0;
    int result=a733_video_jpeg_begin(r,ops,ctx,job,deadline);
    if(result==1){r->retained_readout=1;r->retained_copy=copy_verified;}
    return result;
}

int a733_video_jpeg_next(struct a733_video_jpeg_run *r,const struct a733_video_jpeg_job *j)
{
    if(!r||!j||r->state!=A733_VIDEO_JPEG_COPIED||!r->retained_readout||
       j->input!=r->job.input||!j->file_bytes||j->file_bytes>j->input_bytes||
       j->input_iova!=r->job.input_iova||j->input_bytes!=r->job.input_bytes||
       j->luma_iova!=r->job.luma_iova||j->luma_bytes!=r->job.luma_bytes||
       j->chroma_iova!=r->job.chroma_iova||j->chroma_bytes!=r->job.chroma_bytes)return 0;
    r->job.file_bytes=j->file_bytes;r->index=0;r->status=0;
    r->state=A733_VIDEO_JPEG_PARSE;
    return 1;
}

/* These compound IDs take the legacy MJPEG decoder and split-register map in
 * the pinned package. None is a physical identity observed on this board.
 */
static int known_version(uint64_t v)
{
    return v == UINT64_C(0x0003332100021321) ||
           v == UINT64_C(0x0003331000021320) ||
           v == UINT64_C(0x0003052000012011);
}

static int program(struct a733_video_jpeg_run *r)
{
    uint32_t off, value, cb = r->stride * r->rows / 4u;
    unsigned width = 4, i = r->index;
    if (i == 0) { off=0x1c0; value=r->tables.restart; }
    else if (i == 1) { off=0x11b; value=3; width=1; }
    else if (i < 130) { off=0x180; value=r->tables.quant[i-2]; }
    else if (i == 130) { off=0x1cc; value=(uint32_t)r->job.luma_iova; }
    else if (i == 131) { off=0x1d0; value=(uint32_t)r->job.chroma_iova; }
    else if (i == 132) { off=0x1b8; value=r->tables.size; }
    else if (i == 133) { off=0x1d4; value=0; }
    else if (i == 134) { off=0x1e0; value=0; }
    else if (i < 647) { off=0x1e4; value=r->tables.huffman[i-135]; }
    else switch (i) {
    case 647: off=0x134; value=(uint32_t)(r->job.input_iova+r->job.input_bytes-1); break;
    case 648: off=0x114; value=0x180; break;
    case 649: off=0xc4; value=cb; break;
    case 650: off=0xc8; value=r->stride | ((r->stride/2u)<<16); break;
    case 651: off=0xec; value=(r->format_old & ~0x77u) | 0x22u; break;
    case 652: off=0xe8; value=0xa0000000u | cb; break;
    case 653: off=0xcc; value=r->stride | ((r->stride/2u)<<16); break;
    case 654: off=0x114; value=0x1fc; break;
    case 655: off=0x12c; value=r->picture.scan_offset*8u; break;
    case 656: off=0x130; value=r->picture.scan_bytes*8u; break;
    case 657:
        off=0x128; value=((uint32_t)r->job.input_iova & 0x0ffffff0u) |
                        ((uint32_t)r->job.input_iova >> 28) | 0x70000000u; break;
    case 658: off=0x118; value=0x0e; width=1; break;
    default: return -1;
    }
    return r->ops.write(r->context,r->token,off,width,value,r->deadline);
}
int a733_video_jpeg_poll(struct a733_video_jpeg_run *r)
{
    int result = 1;
    uint32_t v = 0;
    if (!r || r->state == A733_VIDEO_JPEG_NEW) return -1;
    if (r->state == A733_VIDEO_JPEG_DONE || r->state == A733_VIDEO_JPEG_COPIED) return 1;
    if (r->state == A733_VIDEO_JPEG_FAULT) return -1;
    if (!time_ok(r)) return fault(r);
    switch (r->state) {
    case A733_VIDEO_JPEG_ACQUIRE: {
        uint64_t token = r->token;
        result=r->ops.acquire(r->context,&r->job,r->deadline,&r->token);
        if (!r->token || (token && token != r->token)) return fault(r);
        if (result == 1) r->state=A733_VIDEO_JPEG_PARSE;
        break;
    }
    case A733_VIDEO_JPEG_PARSE:
        if (!a733_video_jpeg_parse(r->job.input,r->job.file_bytes,&r->picture) ||
            r->picture.width > 1920 || r->picture.height > 1080 ||
            !a733_video_jpeg_prepare(&r->picture,&r->tables)) return fault(r);
        r->stride=(r->picture.width+31u)&~31u;
        r->rows=(r->picture.height+31u)&~31u;
        if (r->job.luma_bytes < r->stride*r->rows ||
            r->job.chroma_bytes < r->stride*r->rows/2u) return fault(r);
        r->state=A733_VIDEO_JPEG_IDENTIFY; r->index=0;
        break;
    case A733_VIDEO_JPEG_IDENTIFY: {
        const uint32_t offsets[]={0xf0,0xe0,0xe4};
        result=r->ops.read(r->context,r->token,offsets[r->index],&v,r->deadline);
        if (result != 1) return fault(r);
        if (r->index == 0 && v != 0 && v != 0x16670040) return fault(r);
        if (r->index == 1) r->version_high=v&0xfffffu;
        if (r->index == 2) {
            r->version=((uint64_t)r->version_high<<32)|(v&0xfffffu);
            if (!known_version(r->version)) return fault(r);
            r->state=A733_VIDEO_JPEG_PREPARE;
        }
        ++r->index;
        break;
    }
    case A733_VIDEO_JPEG_PREPARE:
        result=r->ops.prepare(r->context,r->token,&r->job,&r->picture,r->stride,r->rows,r->deadline);
        if (result == 1) { r->state=A733_VIDEO_JPEG_CHECK; r->index=0; }
        break;
    case A733_VIDEO_JPEG_CHECK: {
        const uint32_t offsets[]={0,0x11c,0xec};
        result=r->ops.read(r->context,r->token,offsets[r->index],&v,r->deadline);
        if (result != 1) return fault(r);
        switch (r->index) {
        case 0: /* MPEG selected; JPEG-plus/encode/VCU paths must be absent. */
            if (v & (0xfu | 0x20u | 0xc0u | 0x10u | 0x20000000u |
                     0x1000u | 0x0c000000u)) return fault(r);
            break;
        case 1:
            r->status=v;
            if (v & 0x001c3f0fu) return fault(r); /* Includes stale completion. */
            break;
        case 2: r->format_old=v; break;
        }
        if (++r->index == 3) { r->state=A733_VIDEO_JPEG_PROGRAM; r->index=0; }
        break;
    }
    case A733_VIDEO_JPEG_PROGRAM:
        result=program(r);
        if (result != 1) return fault(r);
        if (++r->index == 659) r->state=A733_VIDEO_JPEG_WAIT;
        break;
    case A733_VIDEO_JPEG_WAIT:
        result=r->ops.read(r->context,r->token,0x11c,&v,r->deadline);
        if (result != 1) return fault(r);
        r->status=v;
        if (v & 0x0018000eu) return fault(r);
        if (v & 1) r->state=A733_VIDEO_JPEG_DISABLE;
        break;
    case A733_VIDEO_JPEG_DISABLE:
        result=r->ops.write(r->context,r->token,0x114,4,0x180,r->deadline);
        if (result != 1) return fault(r);
        r->state=A733_VIDEO_JPEG_ACK;
        break;
    case A733_VIDEO_JPEG_ACK:
        result=r->ops.write(r->context,r->token,0x11c,4,r->status&0xfu,r->deadline);
        if (result != 1) return fault(r);
        r->state=A733_VIDEO_JPEG_FINISH;
        break;
    case A733_VIDEO_JPEG_FINISH:
        result=(r->retained_readout?r->retained_copy:r->ops.finish)(r->context,r->token,r->deadline);
        if (result == 1) r->state=r->retained_readout?A733_VIDEO_JPEG_COPIED:A733_VIDEO_JPEG_DONE;
        break;
    default: return fault(r);
    }
    if ((result != 0 && result != 1) || !time_ok(r)) return fault(r);
    return (r->state == A733_VIDEO_JPEG_DONE || r->state == A733_VIDEO_JPEG_COPIED) ? 1 : 0;
}
