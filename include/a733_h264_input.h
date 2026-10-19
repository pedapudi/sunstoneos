#ifndef SUNSTONE_A733_H264_INPUT_H
#define SUNSTONE_A733_H264_INPUT_H
#include "a733_h264_application.h"
#include "a733_video_h264.h"
#include <stdatomic.h>
/* One upload per retained decoder lifetime. Header32bytes, little endian:
 * magic H26U, version1, AnnexB byte count, oracle byte count(0or2304),
 * session epoch64, generation64. Body is stream then optional pixel oracle.
 * Every fragment is bound to the same admitted session and live deadline.
 * Completion hashes and parses owned bytes before release publication.
 * No operation permits reuse, mutation or reset after publication. */
#define A733_H264_UPLOAD_MAGIC UINT32_C(0x55363248)
struct a733_h264_input {
    uint8_t header[32], stream[A733_H264_INPUT_BYTES], oracle[A733_H264_OUTPUT_BYTES];
    uint8_t stream_hash[32], oracle_hash[32];
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t header_used,stream_bytes,oracle_bytes,received;
    unsigned failed;
    atomic_uint committed;
};
/* Producer only; fresh zeroed storage. Returns required bytes for current
 * header/body phase, zero after terminal state. At most4096 bytes per feed. */
size_t a733_h264_input_remaining(const struct a733_h264_input *);
int a733_h264_input_feed(struct a733_h264_input *,const struct a733_h264_session *,
                         int (*now)(void *,uint64_t *),void *,
                         uint64_t deadline,const uint8_t *,size_t);
/* Consumer acquire: only after1 may it inspect stream/oracle and sizes. */
int a733_h264_input_ready(const struct a733_h264_input *);
#endif
