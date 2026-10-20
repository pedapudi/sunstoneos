#ifndef SUNSTONE_ISP0_INPUT_H
#define SUNSTONE_ISP0_INPUT_H
#include "a733_isp0_application.h"
#include <stdatomic.h>
#include <stddef.h>
#define ISP0_UPLOAD_MAGIC UINT32_C(0x55495053)
#define ISP0_INPUT_PADDED 614400u
#define ISP0_RESULT_HEADER 192u
/* One immutable job. Header LE32 magic/version/raw length/reserved0, then
 * LE64 epoch/generation. Each fragment is bound to the retained session.
 * Caller owns the complete private WB allocation before the first fragment;
 * its lease and every supplied byte remain retained after commit or failure. */
struct isp0_input {
    uint8_t header[32],hash[32];
    uint8_t *bytes;
    size_t capacity,header_used,received;
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t expected;
    unsigned failed;
    atomic_uint committed;
};
int isp0_input_bind(struct isp0_input *,uint8_t *,size_t);
size_t isp0_input_remaining(const struct isp0_input *);
int isp0_input_ready(const struct isp0_input *);
int isp0_input_feed(struct isp0_input *,const struct a733_isp0_session *,
    int (*)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
#endif
