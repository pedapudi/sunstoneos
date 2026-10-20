#ifndef SUNSTONE_G2D_INPUT_H
#define SUNSTONE_G2D_INPUT_H
#include "a733_g2d_application.h"
#include <stdatomic.h>
#include <stddef.h>
#define G2D_UPLOAD_MAGIC UINT32_C(0x55444753)
#define G2D_INPUT_PADDED 3686400u
#define G2D_RESULT_HEADER 192u
/* One immutable job. Header LE32 magic/version/raw length/reserved0, then
 * LE64 epoch/generation. Each fragment is bound to the retained session.
 * Caller owns the complete private WB allocation before the first fragment;
 * its lease and every supplied byte remain retained after commit or failure. */
struct g2d_input {
    uint8_t header[32],hash[32];
    uint8_t *bytes;
    size_t capacity,header_used,received;
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t expected;
    unsigned failed;
    atomic_uint committed;
};
int g2d_input_bind(struct g2d_input *,uint8_t *,size_t);
size_t g2d_input_remaining(const struct g2d_input *);
int g2d_input_ready(const struct g2d_input *);
int g2d_input_feed(struct g2d_input *,const struct a733_g2d_session *,
    int (*)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
#endif
