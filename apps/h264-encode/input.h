#ifndef SUNSTONE_H264_ENCODE_INPUT_H
#define SUNSTONE_H264_ENCODE_INPUT_H
#include "a733_h264_encode_application.h"
#include <stdatomic.h>
#include <stddef.h>
#define H264_ENCODE_UPLOAD_MAGIC UINT32_C(0x55454853)
#define H264_ENCODE_INPUT_PADDED 925696u
#define H264_ENCODE_RESULT_HEADER 160u
/* One immutable job. Header LE32 magic/version/raw length/frame count2, then
 * LE64 epoch/generation. Each fragment is bound to the retained session.
 * Caller owns the complete private WB allocation before the first fragment;
 * its lease and every supplied byte remain retained after commit or failure. */
struct h264_encode_input {
    uint8_t header[32],hash[32];
    uint8_t *bytes;
    size_t capacity,header_used,received;
    uint64_t epoch,generation,authority,deadline,last_now;
    unsigned failed;
    atomic_uint committed;
};
int h264_encode_input_bind(struct h264_encode_input *,uint8_t *,size_t);
size_t h264_encode_input_remaining(const struct h264_encode_input *);
int h264_encode_input_ready(const struct h264_encode_input *);
int h264_encode_input_feed(struct h264_encode_input *,const struct a733_h264_encode_session *,
    int (*)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
#endif
