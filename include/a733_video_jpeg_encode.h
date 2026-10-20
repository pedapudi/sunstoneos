/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_JPEG_ENCODE_H
#define SUNSTONE_A733_VIDEO_JPEG_ENCODE_H
#include <stddef.h>
#include <stdint.h>
#define A733_JPEG_ENCODE_RAW_BYTES 460800u
#define A733_JPEG_ENCODE_DMA_INPUT_BYTES 462848u
#define A733_JPEG_ENCODE_CAPACITY 1048576u
#define A733_JPEG_ENCODE_HEADER_BYTES 623u
struct a733_jpeg_encode_job {
    const uint8_t *input;
    size_t input_bytes;
    uint64_t input_iova,output_iova;
    uint32_t input_capacity,output_capacity;
};
struct a733_jpeg_encode_ops {
    int (*now_us)(void *,uint64_t *);
    /* Authenticate the exact immutable job and inherited isolation first.
     * Return stable nonzero token before effects. All resources remain held. */
    int (*acquire)(void *,const struct a733_jpeg_encode_job *,uint64_t,uint64_t *);
    int (*prepare)(void *,uint64_t,const struct a733_jpeg_encode_job *,uint64_t);
    int (*read)(void *,uint64_t,uint32_t,uint32_t *,uint64_t);
    int (*write)(void *,uint64_t,uint32_t,uint32_t,uint64_t);
    /* Copy exactly bytes into private WB RAM, checking fault/status/guards
     * before and after bounded chunks. Return1 only after final digest. This
     * authorizes publication alone, never DMA reclamation or another job. */
    int (*copied)(void *,uint64_t,size_t,uint64_t);
};
enum a733_jpeg_encode_state { A733_JPEG_ENCODE_NEW,A733_JPEG_ENCODE_ACQUIRE,
 A733_JPEG_ENCODE_PREPARE,A733_JPEG_ENCODE_RESET_READ,A733_JPEG_ENCODE_RESET_HIGH,
 A733_JPEG_ENCODE_RESET_REREAD,A733_JPEG_ENCODE_RESET_LOW,A733_JPEG_ENCODE_ENABLE_READ,
 A733_JPEG_ENCODE_ENABLE6,A733_JPEG_ENCODE_ENABLE_REREAD,A733_JPEG_ENCODE_ENABLE7,
 A733_JPEG_ENCODE_ENABLE_CHECK,A733_JPEG_ENCODE_PROGRAM,A733_JPEG_ENCODE_QUANT,
 A733_JPEG_ENCODE_STALE,A733_JPEG_ENCODE_CLEAR,A733_JPEG_ENCODE_CLEAN,
 A733_JPEG_ENCODE_TRIGGER,A733_JPEG_ENCODE_WAIT,A733_JPEG_ENCODE_COUNT,
 A733_JPEG_ENCODE_COPY,A733_JPEG_ENCODE_COPIED,A733_JPEG_ENCODE_FAULT };
struct a733_jpeg_encode_run {
    struct a733_jpeg_encode_job job;
    struct a733_jpeg_encode_ops ops;
    void *context;
    uint64_t token,deadline,last_now;
    uint32_t value,status;
    size_t jpeg_bytes;
    unsigned index;
    enum a733_jpeg_encode_state state;
};
extern const uint8_t a733_jpeg_encode_header[A733_JPEG_ENCODE_HEADER_BYTES];
extern const uint32_t a733_jpeg_encode_quant[128];
/* One privileged serialized caller, fresh zeroed storage, VGA640x480 NV12,
 * stride640/quality75. No CPU entropy or numerical qualification oracle.
 * Terminal states never permit releasing hardware, mappings or memory. */
int a733_jpeg_encode_begin(struct a733_jpeg_encode_run *,const struct a733_jpeg_encode_ops *,
    void *,const struct a733_jpeg_encode_job *,uint64_t);
int a733_jpeg_encode_poll(struct a733_jpeg_encode_run *);
#endif
