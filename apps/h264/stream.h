#ifndef SUNSTONE_H264_STREAM_H
#define SUNSTONE_H264_STREAM_H
#include "stream-profile.h"
#include "a733_h264_application.h"
#include <stdatomic.h>
#define H264_STREAM_FRAMES 256u
#define H264_STREAM_DESCRIPTOR_BYTES 96u
#define H264_STREAM_MANIFEST_MAGIC UINT32_C(0x4d363248)
#define H264_STREAM_FRAME_MAGIC UINT32_C(0x46363248)
#define H264_STREAM_QUERY_MAGIC UINT32_C(0x51363248)
#define H264_STREAM_ACK_MAGIC UINT32_C(0x41363248)
/* Manifest descriptors: LE32 input_bytes, oracle_bytes, frame_num, zero;
 * SHA256 input, oracle;16 zero bytes. Ordinal is the descriptor array index.
 * All commands have32-byte headers: LE32 magic,version1,arg,aux; epoch/gen64.
 * Manifest arg=count/aux=count*96. Frame/query/ACK arg=ordinal/aux=0.
 * Frame body is admitted AU then optional oracle; ACK body is output SHA256. */
struct h264_stream_output {
    atomic_uint state; /*0 empty,1 controller-private,2 published. */
    atomic_uint delivered;
    unsigned ordinal;
    struct a733_h264_picture picture;
    uint8_t digest[32];
#ifdef SUNSTONE_H264_FULLHD
    uint8_t *payload;
#else
    uint8_t payload[H264_STREAM_OUTPUT_BYTES];
#endif
};
struct h264_stream {
    uint8_t manifest[H264_STREAM_FRAMES*H264_STREAM_DESCRIPTOR_BYTES];
    #ifdef SUNSTONE_H264_FULLHD
    uint8_t *input,*oracle;
#else
    uint8_t input[H264_STREAM_INPUT_BYTES],oracle[H264_STREAM_OUTPUT_BYTES];
#endif
    struct h264_stream_output output[2];
    uint8_t header[32],ack[32],manifest_hash[32],config_hash[32];
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t header_used,received,body_bytes,input_bytes,oracle_bytes,config_bytes;
    unsigned command,argument,count,active,copy_slot,config_set;
    atomic_uint manifest_ready,au_state,next,failed; /*AU0 writable,1 ready,2 active. */
    unsigned query_pending,query_ordinal; /* USB task only. */
    struct a733_h264_picture picture;
};
extern struct h264_stream h264_stream;
size_t h264_stream_remaining(const struct h264_stream *);
int h264_stream_feed(struct h264_stream *,const struct a733_h264_session *,
    int (*now)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
/* Controller task only. Complete is called only after actual executor COPIED
 * and its post-callback liveness checks. It retires input, not any allocation. */
int h264_stream_take(struct h264_stream *);
int h264_stream_authorized(const struct h264_stream *,const struct a733_h264_run *);
int h264_stream_complete(struct h264_stream *);
void h264_stream_fail(struct h264_stream *);
const uint8_t *h264_stream_descriptor(const struct h264_stream *,unsigned);
uint32_t h264_stream_u32(const uint8_t *);
int h264_stream_storage_init(uint64_t owner);
int h264_stream_storage_valid(void);
void h264_stream_storage_retain(void);
void h264_stream_usb(void);
void h264_stream_tx_complete(unsigned);
#endif
