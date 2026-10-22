#ifndef SUNSTONE_DEINTERLACE_INPUT_H
#define SUNSTONE_DEINTERLACE_INPUT_H
#include "a733_deinterlace_application.h"
#include <stdatomic.h>
#include <stddef.h>
#define DEINTERLACE_UPLOAD_MAGIC UINT32_C(0x55494453)
#define DEINTERLACE_INPUT_PADDED ((A733_DEINTERLACE_INPUT_CAPACITY+4095u)&~4095u)
#define DEINTERLACE_RESULT_HEADER 192u
/* One immutable upload holding every frame of the session. Header LE32
 * magic/version/raw length/reserved0, then LE64 epoch/generation.
 * Each fragment is bound to the retained session.
 * Caller owns the complete private WB allocation before the first fragment;
 * its lease and every supplied byte remain retained after commit or failure. */
struct deinterlace_input {
    uint8_t header[32],hash[32];
    uint8_t *bytes;
    size_t capacity,header_used,received;
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t expected;
    unsigned failed;
    atomic_uint committed;
};
int deinterlace_input_bind(struct deinterlace_input *,uint8_t *,size_t);
size_t deinterlace_input_remaining(const struct deinterlace_input *);
int deinterlace_input_ready(const struct deinterlace_input *);
int deinterlace_input_feed(struct deinterlace_input *,const struct a733_deinterlace_session *,
    int (*)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
#endif
